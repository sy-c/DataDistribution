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

#include "TfSchedulerTfBuilderInfo.h"
#include "TfSchedulerInstanceRpc.h"

#include <StfSenderRpcClient.h>

#include <set>
#include <tuple>
#include <algorithm>

namespace o2
{
namespace DataDistribution
{

using namespace std::chrono_literals;

void TfSchedulerTfBuilderInfo::updateTfBuilderInfo(const TfBuilderUpdateMessage &pTfBuilderUpdate)
{
  using namespace std::chrono_literals;
  const auto lLocalTime = std::chrono::system_clock::now();

  // recreate timepoint from the received millisecond time stamp
  const std::chrono::milliseconds lUpdateDuration(pTfBuilderUpdate.info().last_update_t());
  const std::chrono::time_point<std::chrono::system_clock> lUpdateTimepoint(lUpdateDuration);
  const auto &lTfBuilderId = pTfBuilderUpdate.info().process_id();

  // check for system time drifts; account for gRPC latency
  const auto lTimeDiff = lLocalTime - lUpdateTimepoint;
  if (std::chrono::abs(lTimeDiff) > 1s) {
    WDDLOG("Large system clock drift detected. tfb_id={:s} drift_ms={:d}", lTfBuilderId,
      std::chrono::duration_cast<std::chrono::milliseconds>(lTimeDiff).count());
  }

  {
    // lock the global info
    std::scoped_lock lLock(mGlobalInfoLock);

    // check if should remove
    if (BasicInfo::NOT_RUNNING == pTfBuilderUpdate.info().process_state()) {

      const auto &lTfIter = mGlobalInfo.find(lTfBuilderId);
      if (lTfIter != mGlobalInfo.end()) {
        // remove from available
        removeReadyTfBuilder(lTfBuilderId);
        // remove from global
        mGlobalInfo.erase(lTfIter);
        IDDLOG("TfBuilder left the partition. tfb_id={:s} reason=NOT_RUNNING", lTfBuilderId);
      }
      return;
    }

    if (0 == mGlobalInfo.count(lTfBuilderId)) {
      // new info, insert it
      mGlobalInfo.try_emplace(
        lTfBuilderId,
        std::make_shared<TfBuilderInfo>(lLocalTime, pTfBuilderUpdate)
      );
      addReadyTfBuilder(mGlobalInfo.at(lTfBuilderId));

      IDDLOG("TfBuilder joined the partition. tfb_id={:s}", lTfBuilderId);
    } else {
      auto &lInfo = mGlobalInfo.at(lTfBuilderId);

      // acquire the ready lock, since the data is shared
      std::scoped_lock lLockReady(mReadyInfoLock);
      lInfo->mUpdateLocalTime = lLocalTime;

      // update only when the last scheduled tf is built!
      if (pTfBuilderUpdate.last_built_tf_id() == lInfo->last_scheduled_tf_id()) {
        // store the new information
        lInfo->mTfBuilderUpdate = pTfBuilderUpdate;

        // verify the memory estimation is correct
        if (lInfo->mEstimatedFreeMemory > pTfBuilderUpdate.free_memory() ) {
          DDDLOG("TfBuilder memory estimate is too high. tfb_id={:s} mem_estimate={}", lTfBuilderId,
            (double(lInfo->mEstimatedFreeMemory) / double(pTfBuilderUpdate.free_memory())));
        }

        lInfo->mEstimatedFreeMemory = pTfBuilderUpdate.free_memory();

      } else if (pTfBuilderUpdate.last_built_tf_id() < lInfo->last_scheduled_tf_id()) {

        // update scheduler's estimate to be on the safe side
        if (lInfo->mEstimatedFreeMemory > pTfBuilderUpdate.free_memory() ) {

          DDDLOG("Ignoring TfBuilder info (last_build < last_scheduled). Fixing the estimate ratio. "
            "tfb_id={:s} new_mem_estimate={}", lTfBuilderId, pTfBuilderUpdate.free_memory());

          lInfo->mEstimatedFreeMemory = pTfBuilderUpdate.free_memory();

        } else {
          // if (last_build > last_scheduled)
          // NOTE: there is a "race" between notifying the EPN to build and updating last_scheduled_tf_id
          // in our record. Thus, this codepath is possible, and we should update the est memory since we
          // hold the lock
          lInfo->mEstimatedFreeMemory = std::min(lInfo->mEstimatedFreeMemory, pTfBuilderUpdate.free_memory());
        }
      }
    }
  } // mGlobalInfoLock unlock
}

bool TfSchedulerTfBuilderInfo::findTfBuilderForTf(const std::uint64_t pSize, std::string& pTfBuilderId /*out*/)
{
  static std::atomic_uint64_t sNoTfBuilderAvailable = 0;
  static std::atomic_uint64_t sNoMemoryAvailable = 0;

  // NOTE: we will overestimate memory requirement by a factor, until TfBuilder updates
  //       us with the actual size.
  const std::uint64_t lTfEstSize = pSize * (sTfSizeOverestimatePercent + 100) / 100;

  std::scoped_lock lLock(mReadyInfoLock);

  uint64_t lMaxMem = 0;
  auto lIt = mReadyTfBuilders.begin();
  for (; lIt != mReadyTfBuilders.end(); ++lIt) {
    lMaxMem = std::max(lMaxMem, (*lIt)->mEstimatedFreeMemory);
    if ((*lIt)->mEstimatedFreeMemory >= lTfEstSize) {
      break;
    }
  }

  // TfBuilder not found?
  if ( lIt == mReadyTfBuilders.end() ) {
    if (mReadyTfBuilders.empty()) {
      ++sNoTfBuilderAvailable;
      WDDLOG_RL(1000, "FindTfBuilder: TF cannot be scheduled. reason=NO_TFBUILDERS total={}",
        sNoTfBuilderAvailable);
    } else {
      ++sNoMemoryAvailable;
      WDDLOG_RL(1000, "FindTfBuilder: TF cannot be scheduled. reason=NO_MEMORY total={} tf_size={} ready_tfb={}",
        sNoMemoryAvailable, lTfEstSize, mReadyTfBuilders.size());
    }
    return false;
  }

  // reposition the selected StfBuilder to the end of the list
  auto lTfBuilder = std::move(*lIt);

  assert (lTfBuilder->mEstimatedFreeMemory >= lTfEstSize);

  // copy the string out
  assert (!lTfBuilder->id().empty());
  pTfBuilderId = lTfBuilder->id();

  // deque erase reverse_iterator
  mReadyTfBuilders.erase(lIt);

  lTfBuilder->mEstimatedFreeMemory -= lTfEstSize;
  mReadyTfBuilders.push_back(std::move(lTfBuilder));

  return true;
}

void TfSchedulerTfBuilderInfo::HousekeepingThread()
{
  using namespace std::chrono_literals;

  DDDLOG("Starting TfBuilderInfo-Housekeeping thread.");

  std::vector<std::string> lIdsToErase;

  while (mRunning) {
    std::this_thread::sleep_for(2000ms);

    {
      std::scoped_lock lLock(mGlobalInfoLock);

      // reap stale TfBuilders
      assert (lIdsToErase.empty());
      for (const auto &lIdInfo : mGlobalInfo) {
        const auto &lInfo = lIdInfo.second;
        const auto lNow = std::chrono::system_clock::now();
        const auto lTimeDiff = std::chrono::abs(lNow - lInfo->mUpdateLocalTime);
        if (lTimeDiff >= sTfBuilderDiscardTimeout) {
          lIdsToErase.push_back(lInfo->mTfBuilderUpdate.info().process_id());
        }

        DDDLOG("TfBuilder information: tfb_id={:s} free_memory={:d} num_buffered_tfs={:d}",
          lInfo->mTfBuilderUpdate.info().process_id(), lInfo->mTfBuilderUpdate.free_memory(),
          lInfo->mTfBuilderUpdate.num_buffered_tfs());
      }

    } // mGlobalInfoLock unlock (to be able to sleep)

    if (!lIdsToErase.empty()) {
      for (const auto &lId : lIdsToErase) {
        std::scoped_lock lLock(mGlobalInfoLock); // CHECK if we need this lock?

        mGlobalInfo.erase(lId);
        removeReadyTfBuilder(lId);
        WDDLOG("TfBuilder removed from the partition. reason=STALE_INFO tfb_id={:s}", lId);
      }
      lIdsToErase.clear();
    }
  }

  DDDLOG("Exiting TfBuilderInfo-Housekeeping thread.");
}

}
} /* o2::DataDistribution */
