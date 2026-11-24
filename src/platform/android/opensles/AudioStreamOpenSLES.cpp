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

#include "AudioStreamOpenSLES.h"
#include "OpenSLESUtilities.h"

#include <coral/audio/AudioUtils.h>

#include <minilog/log.h>
#include <utils/api_level.h>

#include <sys/types.h>

using namespace utils;

namespace coral {

AudioStreamOpenSLES::AudioStreamOpenSLES(const AudioStreamBuilder &builder)
    : AudioStreamBuffered(builder) {
  // OpenSL ES does not support device IDs. So overwrite value from builder.
  mDeviceId = Definitions::kUnspecified;
  // OpenSL ES does not support session IDs. So overwrite value from builder.
  mSessionId = SessionId::None;
}

SLuint32
AudioStreamOpenSLES::channelCountToChannelMaskDefault(int channelCount) const {
  if (channelCount > kAudioChannelCountMax) {
    return SL_ANDROID_UNKNOWN_CHANNELMASK;
  }

  SLuint32 bitfield = (1 << channelCount) - 1;

  // Check for OS at run-time.
  if (api_level() >= __ANDROID_API_N__) {
    return SL_ANDROID_MAKE_INDEXED_CHANNEL_MASK(bitfield);
  }

  // Indexed channels masks were added in N.
  // For before N, the best we can do is use a positional channel mask.
  return bitfield;
}

static bool s_isLittleEndian() {
  static uint32_t value = 1;
  return (*reinterpret_cast<uint8_t *>(&value) == 1); // Does address point to LSB?
}

SLuint32 AudioStreamOpenSLES::getDefaultByteOrder() {
  return s_isLittleEndian() ? SL_BYTEORDER_LITTLEENDIAN
      : SL_BYTEORDER_BIGENDIAN;
}

Result AudioStreamOpenSLES::open() {

  // OpenSL ES only supports I16 and Float
  if (mFormat != AudioSampleFormat::kInt16 && mFormat != AudioSampleFormat::kFloat) {
    MINILOG_WARN("Android's OpenSL ES implementation only supports Int16 and Float. Format: {}",
        convertToText(mFormat));
    return Result::ErrorInvalidFormat;
  }

  SLresult result = EngineOpenSLES::getInstance().open();
  if (SL_RESULT_SUCCESS != result) {
    return Result::ErrorInternal;
  }

  Result opResult = AudioStreamBuffered::open();
  if (opResult != Result::OK) {
    EngineOpenSLES::getInstance().close();
    return opResult;
  }
  // Convert to defaults if UNSPECIFIED
  if (mSampleRate == Definitions::kUnspecified) {
    mSampleRate = Definitions::DEFAULT_SAMPLE_RATE;
  }
  if (mChannelCount == Definitions::kUnspecified) {
    mChannelCount = Definitions::DEFAULT_CHANNEL_COUNT;
  }
  // if (mContentType == Definitions::kUnspecified) {
  //   mContentType = ContentType::Music;
  // }
   if (static_cast<const int32_t>(mUsage) == Definitions::kUnspecified) {
     mUsage = Usage::Media;
   }

  mSharingMode = SharingMode::Shared;

  return Result::OK;
}

SLresult
AudioStreamOpenSLES::finishCommonOpen(SLAndroidConfigurationItf configItf) {
  // Setting privacy sensitive mode is not supported for OpenSL ES.
  mPrivacySensitiveMode = PrivacySensitiveMode::Unspecified;

  SLresult result = registerBufferQueueCallback();
  if (SL_RESULT_SUCCESS != result) {
    return result;
  }

  result = updateStreamParameters(configItf);
  if (SL_RESULT_SUCCESS != result) {
    return result;
  }

  Result opResult = configureBufferSizes(mSampleRate);
  if (Result::OK != opResult) {
    return (SLresult)opResult;
  }

  allocateFifo();

  calculateDefaultDelayBeforeCloseMillis();

  return SL_RESULT_SUCCESS;
}

static int32_t roundUpDivideByN(int32_t x, int32_t n) {
  return (x + n - 1) / n;
}

int32_t AudioStreamOpenSLES::calculateOptimalBufferQueueLength() {
  int32_t queueLength = kBufferQueueLengthDefault;
  int32_t likelyFramesPerBurst = estimateNativeFramesPerBurst();
  int32_t minCapacity = mBufferCapacityInFrames; // specified by app or zero
  // The buffer capacity needs to be at least twice the size of the requested
  // callbackSize so that we can have double buffering.
  minCapacity = std::max(minCapacity, static_cast<int32_t>(kDoubleBufferCount * mFramesPerCallback));
  if (minCapacity > 0) {
    int32_t queueLengthFromCapacity = roundUpDivideByN(minCapacity, likelyFramesPerBurst);
    queueLength = std::max(queueLength, queueLengthFromCapacity);
  }
  queueLength = std::min(queueLength, kBufferQueueLengthMax); // clip to max
  // TODO Investigate the effect of queueLength on latency for normal streams.
  // (not low latency)
  MINILOG_DEBUG("calculateOptimalBufferQueueLength: {}", queueLength);
  return queueLength;
}

int32_t AudioStreamOpenSLES::estimateNativeFramesPerBurst() {
  int32_t framesPerBurst = Definitions::DEFAULT_FRAME_PER_BURST;
  framesPerBurst = std::max(framesPerBurst, 16);
  // Calculate the size of a fixed duration high latency buffer based on sample
  // rate. Estimate sample based on default options in order of priority.
  int32_t sampleRate = 48000;
  sampleRate = (Definitions::DEFAULT_SAMPLE_RATE > 0) ? Definitions::DEFAULT_SAMPLE_RATE
      : sampleRate;
  sampleRate = (mSampleRate > 0) ? mSampleRate : sampleRate;
  int32_t framesPerHighLatencyBuffer =
      (kHighLatencyBufferSizeMillis * sampleRate) / Definitions::kMillisPerSecond;
  // For high latency streams, use a larger buffer size.
  // Performance Mode support was added in N_MR1 (7.1)
  if (api_level() >= __ANDROID_API_N_MR1__
      && mPerformanceMode != PerformanceMode::LowLatency
      && framesPerBurst < framesPerHighLatencyBuffer) {
    // Find a multiple of framesPerBurst >= framesPerHighLatencyBuffer.
    auto numBursts = roundUpDivideByN(framesPerHighLatencyBuffer, framesPerBurst);
    framesPerBurst *= numBursts;
    MINILOG_DEBUG("NOT low latency, numBursts={}, mSampleRate={},set framesPerBurst={}",
        numBursts,
        mSampleRate,
        framesPerBurst);
  }
  return framesPerBurst;
}

Result AudioStreamOpenSLES::configureBufferSizes(int32_t sampleRate) {
  mFramesPerBurst = estimateNativeFramesPerBurst();
  mFramesPerCallback = (mFramesPerCallback > 0) ? mFramesPerCallback : mFramesPerBurst;
  MINILOG_DEBUG("Final mSampleRate={}, mFramesPerBurst={}, mFramesPerCallback={}", mSampleRate, mFramesPerBurst, mFramesPerCallback);

  mBytesPerCallback = mFramesPerCallback * getBytesPerFrame();
  if (mBytesPerCallback <= 0) {
    MINILOG_ERROR("open() bytesPerCallback < 0 = {} bad format?", mBytesPerCallback);
    return Result::ErrorInvalidFormat; // causing bytesPerFrame == 0
  }

  for (int i = 0; i < mBufferQueueLength; ++i) {
    mCallbackBuffer[i] = std::make_unique<uint8_t[]>(mBytesPerCallback);
  }

  if (!usingFIFO()) {
    mBufferCapacityInFrames = mFramesPerBurst * mBufferQueueLength;
    // Check for overflow.
    if (mBufferCapacityInFrames <= 0) {
      mBufferCapacityInFrames = 0;
      MINILOG_ERROR("open() numeric overflow because mFramesPerBurst={}", mFramesPerBurst);
      return Result::ErrorOutOfRange;
    }
    mBufferSizeInFrames = mBufferCapacityInFrames;
  }

  return Result::OK;
}

SLuint32 AudioStreamOpenSLES::convertPerformanceMode(PerformanceMode performanceMode) const {
  SLuint32 openslMode = SL_ANDROID_PERFORMANCE_NONE;
  switch (performanceMode) {
    case PerformanceMode::None: {
      openslMode = SL_ANDROID_PERFORMANCE_NONE;
      break;
    }
    case PerformanceMode::LowLatency: {
      openslMode = (mSessionId == SessionId::None)
          ? SL_ANDROID_PERFORMANCE_LATENCY
          : SL_ANDROID_PERFORMANCE_LATENCY_EFFECTS;
      break;
    }
    case PerformanceMode::PowerSaving: {
      openslMode = SL_ANDROID_PERFORMANCE_POWER_SAVING;
      break;
    }
    default: {
      break;
    }
  }
  return openslMode;
}

PerformanceMode
AudioStreamOpenSLES::convertPerformanceMode(SLuint32 openslMode) const {
  PerformanceMode performanceMode = PerformanceMode::None;
  switch (openslMode) {
    case SL_ANDROID_PERFORMANCE_NONE: {
      performanceMode = PerformanceMode::None;
      break;
    }
    case SL_ANDROID_PERFORMANCE_LATENCY:
    case SL_ANDROID_PERFORMANCE_LATENCY_EFFECTS: {
      performanceMode = PerformanceMode::LowLatency;
      break;
    }
    case SL_ANDROID_PERFORMANCE_POWER_SAVING: {
      performanceMode = PerformanceMode::PowerSaving;
      break;
    }
    default: {
      break;
    }
  }
  return performanceMode;
}

void AudioStreamOpenSLES::logUnsupportedAttributes() {
  // Log unsupported attributes
  // only report if changed from the default
  // Device ID
  if (mDeviceId != Definitions::kUnspecified) {
    MINILOG_WARN("Device ID [AudioStreamBuilder::setDeviceId()] is not supported on OpenSLES streams.");
  }
  // Sharing Mode
  if (mSharingMode != SharingMode::Shared) {
    MINILOG_WARN("SharingMode [AudioStreamBuilder::setSharingMode()] is not supported on OpenSLES streams.");
  }
  // Performance Mode
  int sdkVersion = api_level();
  if (mPerformanceMode != PerformanceMode::None && sdkVersion < __ANDROID_API_N_MR1__) {
    MINILOG_WARN("PerformanceMode [AudioStreamBuilder::setPerformanceMode()] is not supported on OpenSLES streams running on pre-Android N-MR1 versions.");
  }

  // Content Type
  if (mContentType != ContentType::Music) {
    MINILOG_WARN("ContentType [AudioStreamBuilder::setContentType()] is not supported on OpenSLES streams.");
  }

  // Session Id
  if (mSessionId != SessionId::None) {
    MINILOG_WARN("SessionId [AudioStreamBuilder::setSessionId()] is not supported on OpenSLES streams.");
  }

  // Privacy Sensitive Mode
  if (mPrivacySensitiveMode != PrivacySensitiveMode::Unspecified) {
    MINILOG_WARN("PrivacySensitiveMode [AudioStreamBuilder::setPrivacySensitiveMode()] is not supported on OpenSLES streams.");
  }
}

SLresult AudioStreamOpenSLES::configurePerformanceMode(SLAndroidConfigurationItf configItf) {
  if (configItf == nullptr) {
    MINILOG_WARN("Called with NULL configuration");
    mPerformanceMode = PerformanceMode::None;
    return SL_RESULT_INTERNAL_ERROR;
  }
  if (api_level() < __ANDROID_API_N_MR1__) {
    MINILOG_WARN("Not supported until N_MR1");
    mPerformanceMode = PerformanceMode::None;
    return SL_RESULT_SUCCESS;
  }

  SLresult result = SL_RESULT_SUCCESS;
  SLuint32 performanceMode = convertPerformanceMode(getPerformanceMode());
  result = (*configItf)->SetConfiguration(
      configItf, SL_ANDROID_KEY_PERFORMANCE_MODE, &performanceMode, sizeof(performanceMode));
  if (SL_RESULT_SUCCESS != result) {
    MINILOG_WARN("SetConfiguration(PERFORMANCE_MODE, SL {} returned: {}", performanceMode, OpenSLES::SLErrToStr(result));
    mPerformanceMode = PerformanceMode::None;
  }

  return result;
}

SLresult AudioStreamOpenSLES::updateStreamParameters(SLAndroidConfigurationItf configItf) {
  SLresult ret = SL_RESULT_SUCCESS;
  if (api_level() >= __ANDROID_API_N_MR1__ && configItf != nullptr) {
    SLuint32 performanceMode = 0;
    SLuint32 performanceModeSize = sizeof(performanceMode);
    ret = (*configItf)->GetConfiguration(
        configItf, SL_ANDROID_KEY_PERFORMANCE_MODE, &performanceModeSize, &performanceMode);
    // A bug in GetConfiguration() before P caused a wrong result code to be returned.
    if (api_level() <= __ANDROID_API_O_MR1__) {
      ret = SL_RESULT_SUCCESS; // Ignore actual result before P.
    }

    if (SL_RESULT_SUCCESS != ret) {
      MINILOG_WARN("GetConfiguration(SL_ANDROID_KEY_PERFORMANCE_MODE) returned: {}", OpenSLES::SLErrToStr(ret));
      // If we can't query it then assume None.
      mPerformanceMode = PerformanceMode::None;
    } else {
      // convert SL to Coral mode
      mPerformanceMode = convertPerformanceMode(performanceMode);
    }
  } else {
    // If we can't query it then assume None.
    mPerformanceMode = PerformanceMode::None;
  }
  return ret;
}

// This is called under mLock.
Result AudioStreamOpenSLES::closeLocked() {
  if (mState == StreamState::Closed) {
    return Result::ErrorClosed;
  }

  AudioStreamBuffered::close();

  onBeforeDestroy();

  if (mObjectInterface != nullptr) {
    (*mObjectInterface)->Destroy(mObjectInterface);
    mObjectInterface = nullptr;
  }

  onAfterDestroy();

  mSimpleBufferQueueInterface = nullptr;
  EngineOpenSLES::getInstance().close();

  setState(StreamState::Closed);
  return Result::OK;
}

SLresult AudioStreamOpenSLES::enqueueCallbackBuffer(SLAndroidSimpleBufferQueueItf bq) {
  SLresult result = (*bq)->Enqueue(bq, mCallbackBuffer[mCallbackBufferIndex].get(), mBytesPerCallback);
  mCallbackBufferIndex = (mCallbackBufferIndex + 1) % mBufferQueueLength;
  return result;
}

int32_t AudioStreamOpenSLES::getBufferDepth(SLAndroidSimpleBufferQueueItf bq) {
  SLAndroidSimpleBufferQueueState queueState;
  SLresult result = (*bq)->GetState(bq, &queueState);
  return (result == SL_RESULT_SUCCESS) ? queueState.count : -1;
}

bool AudioStreamOpenSLES::processBufferCallback(SLAndroidSimpleBufferQueueItf bq) {
  bool shouldStopStream = false;
  // Ask the app callback to process the buffer.
  DataCallbackResult result = fireDataCallback(mCallbackBuffer[mCallbackBufferIndex].get(), mFramesPerCallback);
  if (result == DataCallbackResult::Continue) {
    // Pass the buffer to OpenSLES.
    SLresult enqueueResult = enqueueCallbackBuffer(bq);
    if (enqueueResult != SL_RESULT_SUCCESS) {
      shouldStopStream = true;
    }
    // Update Coral client position with frames handled by the callback.
    if (getDirection() == Direction::Capture) {
      mFramesRead += mFramesPerCallback;
    } else {
      mFramesWritten += mFramesPerCallback;
    }
  } else if (result == DataCallbackResult::Stop) {
    MINILOG_DEBUG("Callback returned Stop");
    shouldStopStream = true;
  } else {
    MINILOG_WARN("Callback returned unexpected value: {}", convertToText(result));
    shouldStopStream = true;
  }
  if (shouldStopStream) {
    mCallbackBufferIndex = 0;
  }
  return shouldStopStream;
}

// This callback handler is called every time a buffer has been processed by
// OpenSL ES.
static void bqCallbackGlue(SLAndroidSimpleBufferQueueItf bq, void *context) {
  auto shouldStopStream =
      (reinterpret_cast<AudioStreamOpenSLES *>(context))->processBufferCallback(bq);
  if (shouldStopStream) {
    (reinterpret_cast<AudioStreamOpenSLES *>(context))->requestStop();
  }
}

SLresult AudioStreamOpenSLES::registerBufferQueueCallback() {
  // The BufferQueue
  SLresult result =
      (*mObjectInterface)->GetInterface(
          mObjectInterface, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &mSimpleBufferQueueInterface);
  if (SL_RESULT_SUCCESS != result) {
    MINILOG_ERROR("Failed to get sample buffer queue interface: {}", OpenSLES::SLErrToStr(result));
  } else {
    // Register the BufferQueue callback
    result = (*mSimpleBufferQueueInterface)
        ->RegisterCallback(mSimpleBufferQueueInterface, bqCallbackGlue, this);
    if (SL_RESULT_SUCCESS != result) {
      MINILOG_ERROR("RegisterCallback result: {}", OpenSLES::SLErrToStr(result));
    }
  }
  return result;
}

int64_t AudioStreamOpenSLES::getFramesProcessedByServer() {
  updateServiceFrameCounter();
  auto framesProcessed =
      mPositionMillis.get() * getSampleRate() / Definitions::kMillisPerSecond;
  return framesProcessed;
}

Result AudioStreamOpenSLES::waitForStateChange(const StreamState currentState,
  StreamState *nextState,
  const int64_t timeoutNanoseconds) {
  auto result = Result::ErrorTimeout;
  int64_t sleepTimeNanos = 20 * Definitions::kNanosPerMillisecond; // arbitrary
  int64_t timeLeftNanos = timeoutNanoseconds;

  while (true) {
    const auto state = getState(); // this does not require a lock
    if (nextState != nullptr) {
      *nextState = state;
    }
    if (currentState != state) { // state changed?
      result = Result::OK;
      break;
    }

    // Did we timeout or did user ask for non-blocking?
    if (timeLeftNanos <= 0) {
      break;
    }

    if (sleepTimeNanos > timeLeftNanos) {
      sleepTimeNanos = timeLeftNanos;
    }
    AudioUtils::sleepForNanos(sleepTimeNanos);
    timeLeftNanos -= sleepTimeNanos;
  }

  return result;
}

} // namespace coral
