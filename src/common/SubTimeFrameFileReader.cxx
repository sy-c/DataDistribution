// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "SubTimeFrameFile.h"
#include "SubTimeFrameFileReader.h"
#include "SubTimeFrameBuilder.h"

#include "DataDistLogger.h"

#if __linux__
#include <sys/mman.h>
#endif

namespace o2
{
namespace DataDistribution
{

using namespace o2::header;

////////////////////////////////////////////////////////////////////////////////
/// SubTimeFrameFileReader
////////////////////////////////////////////////////////////////////////////////

SubTimeFrameFileReader::SubTimeFrameFileReader(boost::filesystem::path& pFileName)
{
  mFileName = pFileName.string();
  mFileMap.open(mFileName);
  if (! mFileMap.is_open()) {
    EDDLOG("Failed to open TF file for reading (mmap).");
    return;
  }

  mFileSize = mFileMap.size();
  mFileMapOffset = 0;

#if __linux__
  madvise((void*)mFileMap.data(), mFileMap.size(), MADV_HUGEPAGE | MADV_SEQUENTIAL | MADV_DONTDUMP);
#endif
}

SubTimeFrameFileReader::~SubTimeFrameFileReader()
{
  if (! mFileMap.is_open()) {
#if __linux__
    madvise((void*)mFileMap.data(), mFileMap.size(), MADV_DONTNEED);
#endif
    mFileMap.close();
  }
}

void SubTimeFrameFileReader::visit(SubTimeFrame& pStf)
{
  for (auto& lStfDataPair : mStfData) {
    pStf.addStfData(std::move(lStfDataPair));
  }
}

std::size_t SubTimeFrameFileReader::getHeaderStackSize() // throws ios_base::failure
{
  // Expect valid Stack in the file.
  // First Header must be DataHeader. The size is unknown since there are multiple versions.
  // Each header in the stack extends BaseHeader

  // Read first the base header then the rest of the extended header. Keep going until the next flag is set.
  // reset the file pointer to the original incoming position, so the complete Stack can be read in

  bool readNextHeader = true;
  std::size_t lStackSize = 0;
  DataHeader lBaseHdr; // Use DataHeader  since the BaseHeader has no default contructor.

  const auto lFilePosStart = position();

  const auto cMaxHeaders = 16; /* make sure we don't loop forever */
  auto lNumHeaders = 0;
  while (readNextHeader && (++lNumHeaders <= cMaxHeaders)) {
    // read BaseHeader only!
    if(!read_advance(&lBaseHdr, sizeof(BaseHeader))) {
      return 0;
    }

    if (!ignore_nbytes(lBaseHdr.size())) {
      return 0;
    }

    lStackSize += lBaseHdr.size();
    readNextHeader = (lBaseHdr.next() != nullptr);
  }
  // reset the file pointer
  set_position(lFilePosStart);

  if (lNumHeaders >= cMaxHeaders) {
    EDDLOG("FileReader: Reached max number of headers allowed: {}.", cMaxHeaders);
    return 0;
  }

  return lStackSize;
}

Stack SubTimeFrameFileReader::getHeaderStack(std::size_t &pOrigsize)
{
  const auto lStackSize = getHeaderStackSize();
  pOrigsize = lStackSize;

  if (lStackSize < sizeof(BaseHeader)) {
    // error in the stream
    pOrigsize = 0;
    return Stack{};
  }

  o2::byte* lStackMem = peek();
  if (!ignore_nbytes(lStackSize) ) {
    // error in the stream
    pOrigsize = 0;
    return Stack{};
  }

  // This must handle different versions of DataHeader
  // check if DataHeader needs an upgrade by looking at the version number
  const BaseHeader *lBaseOfDH = BaseHeader::get(lStackMem);
  if (!lBaseOfDH) {
    return Stack{};
  }

  if (lBaseOfDH->headerVersion < DataHeader::sVersion) {
    DataHeader lNewDh;

    // Write over the new DataHeader. We need to update some of the BaseHeader values.
    assert (sizeof (DataHeader) > lBaseOfDH->size() ); // current DataHeader must be larger
    std::memcpy(&lNewDh, lBaseOfDH->data(), lBaseOfDH->size());

    // make sure to bump the version in the BaseHeader.
    // TODO: Is there a better way?
    lNewDh.headerSize = sizeof(DataHeader);
    lNewDh.headerVersion = DataHeader::sVersion;

    if (lBaseOfDH->headerVersion == 1 || lBaseOfDH->headerVersion == 2) {
      /* nothing to do for the upgrade */
    } else {
      EDDLOG_RL(1000, "FileReader: DataHeader v{} read from file is not upgraded to the current version {}",
        lBaseOfDH->headerVersion, DataHeader::sVersion);
      EDDLOG_RL(1000, "Try using a newer version of DataDistribution or file a BUG");
    }

    if (lBaseOfDH->size() == lStackSize) {
      return Stack(lNewDh);
    } else {
      assert(lBaseOfDH->size() < lStackSize);

      return Stack(
        lNewDh,
        Stack(lStackMem + lBaseOfDH->size())
      );
    }
  }

  return Stack(lStackMem);
}

std::uint64_t SubTimeFrameFileReader::sStfId = 0; // TODO: add id to files metadata

std::unique_ptr<SubTimeFrame> SubTimeFrameFileReader::read(SubTimeFrameFileBuilder &pFileBuilder)
{
  // make sure headers and chunk pointers don't linger
  mStfData.clear();

  // record current position
  const auto lTfStartPosition = position();

  if (lTfStartPosition == size()) {
    return nullptr;
  }

  // If mFile is good, we're positioned to read a TF
  if (!mFileMap.is_open() || eof()) {
    return nullptr;
  }

  // NOTE: StfID will be updated from the stf header
  std::unique_ptr<SubTimeFrame> lStf = std::make_unique<SubTimeFrame>(sStfId++);

  std::size_t lMetaHdrStackSize = 0;
  const DataHeader *lStfMetaDataHdr = nullptr;
  SubTimeFrameFileMeta lStfFileMeta;

  // Read DataHeader + SubTimeFrameFileMeta
  auto lMetaHdrStack = getHeaderStack(lMetaHdrStackSize);
  if (lMetaHdrStackSize == 0) {
    EDDLOG("Failed to read the TF file header. The file might be corrupted.");
    mFileMap.close();
    return nullptr;
  }

  lStfMetaDataHdr = o2::header::DataHeader::Get(lMetaHdrStack.first());

  if (!read_advance(&lStfFileMeta, sizeof(SubTimeFrameFileMeta))) {
    return nullptr;
  }

  // verify we're actually reading the correct data in
  if (!(SubTimeFrameFileMeta::getDataHeader().dataDescription == lStfMetaDataHdr->dataDescription)) {
    WDDLOG("Reading bad data: SubTimeFrame META header");
    mFileMap.close();
    return nullptr;
  }

  // prepare to read the TF data
  const auto lStfSizeInFile = lStfFileMeta.mStfSizeInFile;
  if (lStfSizeInFile == (sizeof(DataHeader) + sizeof(SubTimeFrameFileMeta))) {
    WDDLOG("Reading an empty TF from file. Only meta information present");
    mFileMap.close();
    return nullptr;
  }

  // check there's enough data in the file
  if ((lTfStartPosition + lStfSizeInFile) > this->size()) {
    WDDLOG_RL(200, "Not enough data in file for this TF. Required: {}, available: {}",
      lStfSizeInFile, (this->size() - lTfStartPosition));
    mFileMap.close();
    return nullptr;
  }

  // Index
  // TODO: skip the index for now, check in future all data is there
  std::size_t lStfIndexHdrStackSize = 0;
  const DataHeader *lStfIndexHdr = nullptr;

  // Read DataHeader + SubTimeFrameFileMeta
  auto lStfIndexHdrStack = getHeaderStack(lStfIndexHdrStackSize);
  if (lStfIndexHdrStackSize == 0 ) {
    mFileMap.close();
    return nullptr;
  }
  lStfIndexHdr = o2::header::DataHeader::Get(lStfIndexHdrStack.first());
  if (!lStfIndexHdr) {
    EDDLOG("Failed to read the TF index structure. The file might be corrupted.");
    return nullptr;
  }

  if (!ignore_nbytes(lStfIndexHdr->payloadSize)) {
    return nullptr;
  }

  // Remaining data size of the TF:
  // total size in file - meta (hdr+struct) - index (hdr + payload)
  const auto lStfDataSize = lStfSizeInFile - (lMetaHdrStackSize + sizeof(SubTimeFrameFileMeta))
    - (lStfIndexHdrStackSize + lStfIndexHdr->payloadSize);

  // read all data blocks and headers
  assert(mStfData.empty());

  std::int64_t lLeftToRead = lStfDataSize;

  // read <hdrStack + data> pairs
  while (lLeftToRead > 0) {

    // allocate and read the Headers
    std::size_t lDataHeaderStackSize = 0;
    Stack lDataHeaderStack = getHeaderStack(lDataHeaderStackSize);
    if (lDataHeaderStackSize == 0) {
      mFileMap.close();
      return nullptr;
    }
    const DataHeader *lDataHeader = o2::header::DataHeader::Get(lDataHeaderStack.first());
    if (!lDataHeader) {
      EDDLOG("Failed to read the TF HBF DataHeader structure. The file might be corrupted.");
      mFileMap.close();
      return nullptr;
    }

    auto lHdrStackMsg = pFileBuilder.newHeaderMessage(lDataHeaderStack, lStf->id());
    if (!lHdrStackMsg) {
      DDDLOG_RL(1000, "Header memory resource stopped. Exiting.");
      mFileMap.close();
      return nullptr;
    }

    // read the data
    const std::uint64_t lDataSize = lDataHeader->payloadSize;

    auto lDataMsg = pFileBuilder.newDataMessage(lDataSize);
    if (!lDataMsg) {
      IDDLOG("Data memory resource stopped. Exiting.");
      mFileMap.close();
      return nullptr;
    }
    if (!read_advance(lDataMsg->GetData(), lDataSize) ) {
      return nullptr;
    }

    // Try to figure out the first orbit
    try {
      const auto lHdr = reinterpret_cast<DataHeader*>(lDataHeaderStack.data());

      if (lHdr && lHdr->firstTForbit == 0 && lHdr->dataDescription == o2::header::gDataDescriptionRawData) {
        const auto R = RDHReader(lDataMsg);
        lStf->updateFirstOrbit(R.getOrbit());
      }
    } catch (...) {
      EDDLOG("Error getting RDHReader instace. Not setting firstOrbit for file data");
    }

    mStfData.emplace_back(std::move(lHdrStackMsg), std::move(lDataMsg));

    // update the counter
    lLeftToRead -= (lDataHeaderStackSize + lDataSize);
  }

  if (lLeftToRead < 0) {
    EDDLOG("FileRead: Read more data than it is indicated in the META header!");
    return nullptr;
  }

  // build the SubtimeFrame
  lStf->accept(*this);

  return lStf;
}
}
} /* o2::DataDistribution */
