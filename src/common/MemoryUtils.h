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


#ifndef DATADIST_MEMORY_UTILS_H_
#define DATADIST_MEMORY_UTILS_H_

#include <boost/container/pmr/memory_resource.hpp>
#include <boost/filesystem.hpp>
#include <boost/icl/interval_map.hpp>
#include <boost/icl/right_open_interval.hpp>

#include <fairmq/FairMQDevice.h>
#include <fairmq/FairMQChannel.h>

#include <Headers/DataHeader.h>

#include "DataDistLogger.h"

#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <chrono>

#include <sys/mman.h>
#include <cstdlib>
#include <unistd.h>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace icl = boost::icl;

class DataHeader;
class FairMQUnmanagedRegion;

namespace o2
{
namespace DataDistribution
{

static constexpr const char *ENV_NOLOCK = "DATADIST_NO_MLOCK";
static constexpr const char *ENV_SHM_PATH = "DATADIST_SHM_PATH";
static constexpr const char *ENV_SHM_DELAY = "DATADIST_SHM_DELAY";

template<size_t ALIGN = 64>
class RegionAllocatorResource
{

public:
  RegionAllocatorResource() = delete;

  RegionAllocatorResource(std::string pSegmentName, FairMQTransportFactory& pShmTrans,
                          std::size_t pSize, std::uint64_t pRegionFlags = 0)
  : mSegmentName(pSegmentName), mTransport(pShmTrans)
  {
    static_assert(ALIGN && !(ALIGN & (ALIGN - 1)), "Alignment must be power of 2");

    pSize = align_size_up(pSize);

    int lMapFlags = 0;
    std::string lSegmentRoot = "";

    // don't reserve swap space
#if defined(MAP_NORESERVE)
    lMapFlags |= MAP_NORESERVE;
#endif

    // and try to lock the memory
#if defined(MAP_LOCKED) && defined(__linux__)
{
    struct rlimit lMyLimits;
    getrlimit(RLIMIT_MEMLOCK, &lMyLimits);

    if (lMyLimits.rlim_cur >= pSize) {
      lMapFlags |= MAP_LOCKED;
    } else {
      if (std::getenv(ENV_NOLOCK)) {
        WDDLOG("MemoryResource: Memory locking disabled via {} env variable. Not suitable for production.",
          ENV_NOLOCK);
      } else {
        EDDLOG("MemoryResource: Failed to lock the memory region. Increase your memory lock limits (ulimit -l).");
        EDDLOG("MemoryResource: To run without memory locking define {} env variable. Not suitable for production.",
          ENV_NOLOCK);
        throw std::bad_alloc();
      }
    }
}
#endif

    // populate the mapping
#if defined(MAP_POPULATE)
    lMapFlags |= MAP_POPULATE;
#endif

    // try to use different file mapping (hugetlbfs)
    const auto lHugetlbfsPath = std::getenv(ENV_SHM_PATH);
    if (lHugetlbfsPath) {
      do {
        // make sure directory exists
        namespace bfs = boost::filesystem;
        bfs::path lDirPath(lHugetlbfsPath);
        if (!bfs::is_directory(lDirPath)) {
          EDDLOG("Hugetlbfs mountpoint does not exist. Not using huge pages. {}={}",
            ENV_SHM_PATH, lHugetlbfsPath);
          break;
        }

        // check if the hugetlbs is writeable
        if (0 != access(lHugetlbfsPath, W_OK)) {
          EDDLOG("Hugetlbfs mountpoint is not writeable. "
            "Make sure the permissions are properly set. {}={}", ENV_SHM_PATH, lHugetlbfsPath);
          break;
        }

        lSegmentRoot = lHugetlbfsPath;
        lSegmentRoot += bfs::path::preferred_separator;
      } while (false);
    }

    IDDLOG("Creating new UnmanagedRegion name={} path={} size={}",
      mSegmentName, lSegmentRoot, pSize);

    mRegion = pShmTrans.CreateUnmanagedRegion(
      pSize,
      pRegionFlags,
      [this](const std::vector<FairMQRegionBlock>& pBlkVect) {

        static thread_local icl::interval_map<std::size_t, std::size_t> lIntMap;
        static thread_local double sMergeRatio = 0.5;

        std::int64_t lReclaimed = 0;

        lIntMap.clear();

        for (const auto &lInt : pBlkVect) {
          if (lInt.size == 0) {
            continue;
          }

          lIntMap += std::make_pair(
            icl::discrete_interval<std::size_t>::right_open(
              std::size_t(lInt.ptr) , std::size_t(lInt.ptr) + lInt.size), std::size_t(1));
        }

        {
          // callback to be called when message buffers no longer needed by transports
          std::scoped_lock lock(mReclaimLock);

          for (const auto &lIntMerged : lIntMap) {
            if (lIntMerged.second > 1) {
              EDDLOG("CreateUnmanagedRegion reclaim BUG! Multiple overlapping intervals:");
              for (const auto &i : lIntMap) {
                EDDLOG("- [{},{}) : count={}", i.first.lower(), i.first.upper(), i.second);
              }

              continue; // skip the overlapping thing
            }

            const auto lLen = lIntMerged.first.upper() - lIntMerged.first.lower();
            lReclaimed += lLen;

            reclaimSHMMessage((void *) lIntMerged.first.lower(), lLen);
          }
        }

        mFree += lReclaimed;

        // weighted average merge ratio
        sMergeRatio = sMergeRatio * 0.75 + double(pBlkVect.size() - lIntMap.iterative_size()) /
          double(pBlkVect.size()) * 0.25;
        DDDLOG_RL(5000, "Memory segment '{}'::block merging ratio average={:.4}", mSegmentName, sMergeRatio);
      },
      lSegmentRoot.c_str(),
      lMapFlags
    );

    if (!mRegion) {
      EDDLOG("Creation of new memory region failed. name={} size={} path={}",
      mSegmentName, pSize, lSegmentRoot);
      throw std::bad_alloc();
    }

    mStart = static_cast<char*>(mRegion->GetData());
    mSegmentSize = mRegion->GetSize();
    mLength = mRegion->GetSize();
    mFree = mSegmentSize;

    memset(mStart, 0xAA, mLength);

    // Insert delay for testing
    const auto lShmDelay = std::getenv(ENV_SHM_DELAY);
    if (lShmDelay) {
      try {
        double lDelaySec = std::stod(lShmDelay);
        lDelaySec = std::abs(lDelaySec);

        WDDLOG("Memory segment '{}': delaying processing for specified={}s",
          mSegmentName, lDelaySec);
        std::this_thread::sleep_for(std::chrono::duration<double>(lDelaySec));
      } catch (const std::logic_error &e) {
        EDDLOG("Memory segment '{}': invalid delay specified={} error={}",
          mSegmentName, lShmDelay, e.what());
      }
    }

    // start the allocations
    mRunning = true;
  }


  ~RegionAllocatorResource() {
    // Ensure the region is destructed before anything else in this object
    mRegion.reset();
  }

  inline
  std::unique_ptr<FairMQMessage> NewFairMQMessage(std::size_t pSize) {
    auto* lMem = do_allocate(pSize, ALIGN);
    if (lMem) {
      return mTransport.CreateMessage(mRegion, lMem, pSize);
    } else {
      return nullptr;
    }
  }

  inline
  std::unique_ptr<FairMQMessage> NewFairMQMessageFromPtr(void *pPtr, const std::size_t pSize) {
    assert(pPtr >= static_cast<char*>(mRegion->GetData()));
    assert(static_cast<char*>(pPtr)+pSize <= static_cast<char*>(mRegion->GetData()) + mRegion->GetSize());

    return mTransport.CreateMessage(mRegion, pPtr, pSize);
  }

  void stop() {
    std::scoped_lock lock(mReclaimLock);
    mRunning = false;
  }

protected:
  // NOTE: we align sizes of returned messages, but keep the exact size for allocation
  //       otherwise the shm messages would be larger than requested
  static constexpr inline
  std::size_t align_size_up(const std::size_t pSize) {
    return (pSize + ALIGN - 1) / ALIGN * ALIGN;
  }

  void* do_allocate(std::size_t pSize, std::size_t /* pAlign */)
  {

    if (!mRunning) {
      return nullptr;
    }

    if (pSize == 0) {
      // return last address of the segment
      return reinterpret_cast<char*>(mRegion->GetData()) + mRegion->GetSize();
    }

    // align up
    pSize = align_size_up(pSize);

    auto lRet = try_alloc(pSize);
    // we cannot fail! report problem if failing to allocate block often
    while (!lRet && mRunning) {
      // try to reclaim if possible
      if (try_reclaim(pSize)) {
        // try again
        lRet = try_alloc(pSize);
      }

      if (!lRet) {
        using namespace std::chrono_literals;
        WDDLOG_RL(1000, "RegionAllocatorResource: waiting to allocate a message. region={} alloc={} region_size={} free={}",
          mSegmentName, pSize, mRegion->GetSize(), mFree);
        WDDLOG_RL(1000, "Memory region '{}' is too small, or there is a large backpressure.", mSegmentName);
        std::this_thread::sleep_for(5ms);
      }
    }

    // check the running again
    if (!mRunning && !lRet) {
      WDDLOG("Memory segment '{}' is stopped. No allocations are possible.", mSegmentName);
      return nullptr;
    }

    mFree -= pSize;

    static std::size_t sLogRateLimit = 0;
    if (sLogRateLimit++ % 1024 == 0) {
      const std::int64_t lFree = mFree;
      DDDLOG_RL(2000, "DataRegionResource {} memory free={} allocated={}", mSegmentName, lFree, (mSegmentSize - lFree));
    }

    return lRet;
  }

private:
  inline
  void* try_alloc(const std::size_t pSize) {
    if (mLength >= pSize) {
      const auto lObjectPtr = mStart;

      mStart += pSize;
      mLength -= pSize;

      if (mLength == 0) {
        mStart = nullptr;
      }

      return lObjectPtr;
    }

    return nullptr;
  }

  bool try_reclaim(const std::size_t pSize) {
    // First declare any leftover memory as free
    std::scoped_lock lock(mReclaimLock);

    if (mLength > 0) {
      assert(mStart != nullptr);
      // NOTE: caller must hold mReclaimLock lock
      reclaimSHMMessage(mStart, mLength);
    }

    // invalidate the working extent
    mStart = nullptr;
    mLength = 0;

    if (mFreeRanges.empty()) {
      return false;
    }

    auto lMaxIter = std::max_element(std::begin(mFreeRanges), std::end(mFreeRanges),
      [](const auto& l, const auto& r) {
        return (l.first.upper() - l.first.lower()) < (r.first.upper() - r.first.lower());
      });

    // check if the size is adequate
    const auto lFoudSize = lMaxIter->first.upper() - lMaxIter->first.lower();
    if (pSize > lFoudSize) {
      return false;
    }

    if (lMaxIter->second > 1) {
      EDDLOG("RegionAllocator BUG: Overlapping interval found: ptr={:p} length={} overlaps={}",
        reinterpret_cast<char*>(lMaxIter->first.lower()), lFoudSize, lMaxIter->second);

      // erase this segment
      mFree -= lFoudSize;
      mFreeRanges.erase(lMaxIter);
      return false;
    }

    // return the extent
    mStart = reinterpret_cast<char*>(lMaxIter->first.lower());
    mLength = lFoudSize;
    mFreeRanges.erase(lMaxIter);

    {
      // estimated fragmentation
      static thread_local double sFree = 0.0;
      static thread_local double sNumFragments = 0.0;
      static thread_local double sFragmentation = 0.0;

      const std::size_t lFree = mFree;
      sFree = sFree * 0.75 + double(lFree) * 0.25;
      sFragmentation = sFragmentation * 0.75 + double(lFree - mLength)/double(lFree) * 0.25;
      sNumFragments = sNumFragments * 0.75 + double(mFreeRanges.iterative_size() + 1) * 0.25;

      DDDLOG_RL(5000, "DataRegionResource {} estimated: free={:.4} num_fragments={:.4} fragmentation={:.4}",
        mSegmentName, sFree, sNumFragments, sFragmentation);
    }

    return true;
  }

  void reclaimSHMMessage(void* pData, size_t pSize)
  {
    // align up
    pSize = align_size_up(pSize);

    mFreeRanges += std::make_pair(icl::discrete_interval<std::size_t>::right_open(
      reinterpret_cast<std::size_t>(pData), reinterpret_cast<std::size_t>(pData) + pSize),
      std::size_t(1)
    );

#if !defined(NDEBUG)
    for (const auto &lInt : mFreeRanges) {
      if (lInt.second > 1) {
        EDDLOG("RegionAllocator BUG: Overlapping interval found on reclaim: ptr={:p} length={} overlaps={}",
          reinterpret_cast<char*>(lInt.first.lower()), lInt.first.upper() - lInt.first.lower(), lInt.second);
      }
    }
#endif
  }

  /// fields
  std::string mSegmentName;
  std::size_t mSegmentSize;
  std::atomic_bool mRunning = true;

  FairMQTransportFactory &mTransport;
  std::unique_ptr<FairMQUnmanagedRegion> mRegion;

  char *mStart = nullptr;
  std::size_t mLength = 0;

  // free space accounting
  std::atomic_int64_t mFree = 0;

  // two step reclaim to avoid lock contention in the allocation path
  std::mutex mReclaimLock;
  icl::interval_map<std::size_t, std::size_t> mFreeRanges;
};


class MemoryResources {

public:
  MemoryResources() = delete;
  explicit MemoryResources(std::shared_ptr<FairMQTransportFactory> pShmTransport)
  : mShmTransport(pShmTransport) { }

  virtual ~MemoryResources() {
    // make sure to delete regions before dropping the transport
    mHeaderMemRes.reset();
    mDataMemRes.reset();
    mShmTransport.reset();
  }

  inline
  FairMQMessagePtr newHeaderMessage(const std::size_t pSize) {
    assert(mHeaderMemRes);
    return mHeaderMemRes->NewFairMQMessage(pSize);
  }

  inline
  FairMQMessagePtr newDataMessage(const std::size_t pSize) {
    assert(mDataMemRes);
    return mDataMemRes->NewFairMQMessage(pSize);
  }

  inline
  bool running() { return mRunning == true; }

  void stop() {
    assert(mShmTransport);

    mRunning = false;

    if (mHeaderMemRes) {
      mHeaderMemRes->stop();
    }
    if (mDataMemRes) {
      mDataMemRes->stop();
    }
  }

  std::unique_ptr<RegionAllocatorResource<alignof(o2::header::DataHeader)>> mHeaderMemRes;
  std::unique_ptr<RegionAllocatorResource<64>> mDataMemRes;

  // shm transport
  std::shared_ptr<FairMQTransportFactory> mShmTransport;

private:
  bool mRunning = true;
};


class SyncMemoryResources : public MemoryResources {
public:
  SyncMemoryResources() = delete;
  explicit SyncMemoryResources(std::shared_ptr<FairMQTransportFactory> pShmTransport)
  : MemoryResources(pShmTransport) { }

  virtual ~SyncMemoryResources() {}

  inline
  FairMQMessagePtr newHeaderMessage(const std::size_t pSize) {
    assert(mHeaderMemRes);
    std::scoped_lock lock(mHdrLock);
    return mHeaderMemRes->NewFairMQMessage(pSize);
  }

  inline
  FairMQMessagePtr newDataMessage(const std::size_t pSize) {
    assert(mDataMemRes);
    std::scoped_lock lock(mDataLock);
    return mDataMemRes->NewFairMQMessage(pSize);
  }


private:
  std::mutex mHdrLock;
  std::mutex mDataLock;
};

}
} /* o2::DataDistribution */

#endif /* DATADIST_MEMORY_UTILS_H_ */
