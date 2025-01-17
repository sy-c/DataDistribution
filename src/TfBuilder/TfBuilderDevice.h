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

#ifndef ALICEO2_TF_BUILDER_DEVICE_H_
#define ALICEO2_TF_BUILDER_DEVICE_H_

#include "TfBuilderInput.h"
#include "TfBuilderRpc.h"

#include <ConfigConsul.h>

#include <SubTimeFrameDataModel.h>
#include <SubTimeFrameFileSink.h>
#include <SubTimeFrameFileSource.h>
#include <ConcurrentQueue.h>
#include <Utilities.h>
#include <FmqUtilities.h>

#include <deque>
#include <mutex>
#include <memory>
#include <condition_variable>

namespace o2
{
namespace DataDistribution
{

class ConsulConfig;
class StfToDplAdapter;

enum TfBuilderPipeline {
  eTfBuilderOut = 0,
  eTfFileSourceOut = 0,

  // input/output stages
  eTfFileSinkIn = 0,
  eTfFileSinkOut = 1,

  eTfFwdIn = 1,

  eTfPipelineSize = 2,
  eTfInvalidStage = -1,
};

class TfBuilderDevice : public DataDistDevice,
                        public IFifoPipeline<std::unique_ptr<SubTimeFrame>>
{
 public:
  static constexpr const char* OptionKeyStandalone = "stand-alone";
  static constexpr const char* OptionKeyTfMemorySize = "tf-memory-size";

  static constexpr const char* OptionKeyDplChannelName = "dpl-channel-name";

  /// Default constructor
  TfBuilderDevice();

  /// Default destructor
  ~TfBuilderDevice() override;

  bool start();
  void stop();

  void PostRun() override final;

  void Init() override final;
  void Reset() override final;

  void InitTask() final;
  void ResetTask() final;


  SyncMemoryResources& MemI() { return *mMemI; }
  TimeFrameBuilder& TfBuilderI() const { return *mTfBuilder; }

 protected:
  void PreRun() final;
  bool ConditionalRun() final;

  // Run the TFBuilder pipeline
  unsigned getNextPipelineStage(unsigned pStage) final
  {
    TfBuilderPipeline lNextStage = eTfInvalidStage;

    switch (pStage) {
      case eTfBuilderOut:
      /*case eTfFileSourceOut:*/
      {
        lNextStage = mFileSink.enabled() ? eTfFileSinkIn : eTfFwdIn;
        break;
      }
      case eTfFileSinkOut:
      {
        lNextStage = eTfFwdIn;
        break;
      }
      default:
      {
        throw std::runtime_error("pipeline error");
      }
    }

    assert(lNextStage >= eTfFileSinkIn && lNextStage <= eTfFwdIn);

    return lNextStage;
  }

  void TfForwardThread();


  const std::string& getDplChannelName() const { return mDplChannelName; }

  bool dplEnabled() const noexcept { return mDplEnabled; }

  /// Memory region singletons
  std::unique_ptr<SyncMemoryResources> mMemI;

  /// Configuration
  std::string mDplChannelName;
  bool mStandalone;
  std::uint64_t mTfBufferSize;
  std::string mPartitionId;
  bool mDplEnabled = false;

  /// Discovery configuration
  std::shared_ptr<ConsulTfBuilder> mDiscoveryConfig;

  /// RPC service
  std::shared_ptr<TfBuilderRpcImpl> mRpc;

  /// Input Interface handler
  std::unique_ptr<TfBuilderInput> mFlpInputHandler;
  /// prepare TF for output (standard or DPL)
  std::unique_ptr<TimeFrameBuilder> mTfBuilder;
  /// Serializer for DPL channel
  std::unique_ptr<StfToDplAdapter> mTfDplAdapter;

  /// File sink
  SubTimeFrameFileSink mFileSink;

  /// File source
  SubTimeFrameFileSource mFileSource;

  /// TF forwarding thread
  std::thread mTfFwdThread;

  /// Info thread
  void InfoThread();
  std::thread mInfoThread;

  RunningSamples<uint64_t> mTfSizeSamples;
  RunningSamples<float> mTfTimeSamples;

  std::atomic_bool mRunning = false;
  std::atomic_bool mShouldExit = false;
};

}
} /* namespace o2::DataDistribution */

#endif /* ALICEO2_TF_BUILDER_DEVICE_H_ */
