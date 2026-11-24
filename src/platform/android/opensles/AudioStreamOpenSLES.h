/*
 * Copyright (C) 2023 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_AUDIO_ANDROID_AUDIO_STREAM_OPEN_SLES_H
#define CORAL_AUDIO_ANDROID_AUDIO_STREAM_OPEN_SLES_H

#include "AudioStreamBuffered.h"

#include "EngineOpenSLES.h"
#include "platform/android/MonotonicCounter.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <atomic>
#include <memory>

namespace coral {

/**
 * Internal use only
 *
 * A stream that wraps OpenSL ES.
 *
 * Do not instantiate this class directly.
 */
class AudioStreamOpenSLES : public AudioStreamBuffered {
public:
  explicit AudioStreamOpenSLES(const AudioStreamBuilder& builder);

  ~AudioStreamOpenSLES() override = default;

  virtual Result open() override;

  /**
   * Query the current state.
   *
   * @return state or a negative error.
   */
  StreamState getState() override {
    return mState.load();
  }

  AudioApi getAudioApi() const override {
    return AudioApi::OpenSLES;
  }

  /**
   * Process next OpenSL ES buffer.
   * Called by by OpenSL ES framework.
   *
   * This is public, but don't call it directly.
   *
   * @return whether the current stream should be stopped.
   */
  bool processBufferCallback(SLAndroidSimpleBufferQueueItf bq);

  Result waitForStateChange(StreamState currentState, StreamState *nextState, int64_t timeoutNanoseconds) override;

protected:
  static constexpr int kBitsPerByte = 8;
  //!< double buffered for callbacks
  static constexpr int kBufferQueueLengthDefault = 2;
  //!< AudioFlinger won't use more than 8
  static constexpr int kBufferQueueLengthMax = 8;

  /**
   * Finish setting up the stream. Common for INPUT and OUTPUT.
   *
   * @param configItf
   * @return SL_RESULT_SUCCESS if OK.
   */
  SLresult finishCommonOpen(SLAndroidConfigurationItf configItf);

  // This must be called under mLock.
  Result closeLocked();

  SLuint32 channelCountToChannelMaskDefault(int channelCount) const;

  virtual Result onBeforeDestroy() { return Result::OK; }

  virtual Result onAfterDestroy() { return Result::OK; }

  static SLuint32 getDefaultByteOrder();

  int32_t getBufferDepth(SLAndroidSimpleBufferQueueItf bq);

  int32_t calculateOptimalBufferQueueLength();

  int32_t estimateNativeFramesPerBurst();

  SLresult enqueueCallbackBuffer(SLAndroidSimpleBufferQueueItf bq);

  SLresult configurePerformanceMode(SLAndroidConfigurationItf configItf);

  PerformanceMode convertPerformanceMode(SLuint32 openslMode) const;

  SLuint32 convertPerformanceMode(PerformanceMode performanceMode) const;

  void logUnsupportedAttributes();

  /**
   * Internal use only.
   * Use this instead of directly setting the internal state variable.
   */
  void setState(StreamState state) { mState.store(state); }

  int64_t getFramesProcessedByServer();

  // OpenSLES stuff
  SLObjectItf mObjectInterface = nullptr;
  SLAndroidSimpleBufferQueueItf mSimpleBufferQueueInterface = nullptr;
  int mBufferQueueLength = 0;

  int32_t mBytesPerCallback = Definitions::kUnspecified;
  MonotonicCounter mPositionMillis{}; // for tracking OpenSL ES service position

private:
  static constexpr int kDoubleBufferCount = 2;
  static constexpr int32_t kHighLatencyBufferSizeMillis = 20; // typical Android period
  static constexpr SLuint32 kAudioChannelCountMax = 30;
  static constexpr SLuint32 SL_ANDROID_UNKNOWN_CHANNELMASK = 0; // Matches name used internally.

  SLresult registerBufferQueueCallback();

  SLresult updateStreamParameters(SLAndroidConfigurationItf configItf);

  Result configureBufferSizes(int32_t sampleRate);

  std::unique_ptr<uint8_t[]> mCallbackBuffer[kBufferQueueLengthMax];
  int mCallbackBufferIndex = 0;
  std::atomic<StreamState> mState{StreamState::Uninitialized};

};

} // namespace coral

#endif // CORAL_AUDIO_ANDROID_AUDIO_STREAM_OPEN_SLES_H
