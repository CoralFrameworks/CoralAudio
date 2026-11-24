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

#include "AudioStreamOutputOpenSLES.h"
#include "OpenSLESUtilities.h"
#include "OutputMixerOpenSLES.h"

#include <minilog/log.h>
#include <utils/api_level.h>

using namespace utils;

namespace coral {

static SLuint32 OpenSLES_convertOutputUsage(Usage usage) {
  SLuint32 openslStream = SL_ANDROID_STREAM_MEDIA;
  switch (usage) {
    case Usage::Media:openslStream = SL_ANDROID_STREAM_MEDIA;
      break;
    case Usage::VoiceCommunication:
    case Usage::VoiceCommunicationSignalling:openslStream = SL_ANDROID_STREAM_VOICE;
      break;
    case Usage::Alarm:openslStream = SL_ANDROID_STREAM_ALARM;
      break;
    case Usage::Notification:
    case Usage::NotificationRingtone:
    case Usage::NotificationEvent:openslStream = SL_ANDROID_STREAM_NOTIFICATION;
      break;
    case Usage::AssistanceAccessibility:
    case Usage::AssistanceNavigationGuidance:
    case Usage::AssistanceSonification:openslStream = SL_ANDROID_STREAM_SYSTEM;
      break;
    case Usage::Game:openslStream = SL_ANDROID_STREAM_MEDIA;
      break;
    case Usage::Assistant:
    default:openslStream = SL_ANDROID_STREAM_SYSTEM;
      break;
  }
  return openslStream;
}

AudioStreamOutputOpenSLES::AudioStreamOutputOpenSLES(const AudioStreamBuilder &builder) noexcept
    : AudioStreamOpenSLES(builder) {
}

// These will wind up in <SLES/OpenSLES_Android.h>
constexpr int SL_ANDROID_SPEAKER_STEREO = (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT);

constexpr int SL_ANDROID_SPEAKER_QUAD = (SL_ANDROID_SPEAKER_STEREO | SL_SPEAKER_BACK_LEFT | SL_SPEAKER_BACK_RIGHT);

constexpr int SL_ANDROID_SPEAKER_5DOT1 = (SL_ANDROID_SPEAKER_QUAD | SL_SPEAKER_FRONT_CENTER | SL_SPEAKER_LOW_FREQUENCY);

constexpr int SL_ANDROID_SPEAKER_7DOT1 = (SL_ANDROID_SPEAKER_5DOT1 | SL_SPEAKER_SIDE_LEFT | SL_SPEAKER_SIDE_RIGHT);

SLuint32 AudioStreamOutputOpenSLES::channelCountToChannelMask(int channelCount) const {
  SLuint32 channelMask = 0;

  switch (channelCount) {
    case 1: // mono
      channelMask = SL_SPEAKER_FRONT_CENTER;
      break;

    case 2: // stereo
      channelMask = SL_ANDROID_SPEAKER_STEREO;
      break;

    case 4: // Quad
      channelMask = SL_ANDROID_SPEAKER_QUAD;
      break;

    case 6: // 5.1
      channelMask = SL_ANDROID_SPEAKER_5DOT1;
      break;

    case 8: // 7.1
      channelMask = SL_ANDROID_SPEAKER_7DOT1;
      break;

    default: // default mask
      channelMask = channelCountToChannelMaskDefault(channelCount);
      break;
  }
  return channelMask;
}

Result AudioStreamOutputOpenSLES::open() {
  logUnsupportedAttributes();

  SLAndroidConfigurationItf configItf = nullptr;

  if (api_level() < __ANDROID_API_L__ &&
      mFormat == AudioSampleFormat::kFloat) {
    // TODO: Allow floating point format on API <21 using float->int16 converter
    return Result::ErrorInvalidFormat;
  }

  // If audio format is unspecified then choose a suitable default.
  // API 21+: FLOAT
  // API <21: INT16
  if (mFormat == AudioSampleFormat::kDefault) {
    mFormat = (api_level() < __ANDROID_API_L__)
        ? AudioSampleFormat::kInt16
        : AudioSampleFormat::kFloat;
  }

  Result opResult = AudioStreamOpenSLES::open();
  if (Result::OK != opResult) {
    return opResult;
  }

  SLresult result = OutputMixerOpenSLES::getInstance().open();
  if (SL_RESULT_SUCCESS != result) {
    AudioStreamOpenSLES::close();
    return Result::ErrorInternal;
  }

  SLuint32 bitsPerSample = static_cast<SLuint32>(getBytesPerSample() * kBitsPerByte);

  // configure audio source
  mBufferQueueLength = calculateOptimalBufferQueueLength();
  SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
      SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,    // locatorType
      static_cast<SLuint32>(mBufferQueueLength)}; // numBuffers

  // Define the audio data format.
  SLDataFormat_PCM format_pcm = {
      SL_DATAFORMAT_PCM,                          // formatType
      static_cast<SLuint32>(mChannelCount),       // numChannels
      static_cast<SLuint32>(
          mSampleRate *
              Definitions::kMillisPerSecond),    // milliSamplesPerSec
      bitsPerSample,                             // bitsPerSample
      bitsPerSample,                             // containerSize;
      channelCountToChannelMask(mChannelCount),  // channelMask
      getDefaultByteOrder(),
  };

  SLDataSource audioSrc = {&loc_bufq, &format_pcm};

  /**
   * API 21 (Lollipop) introduced support for floating-point data representation
   * and an extended data format type: SLAndroidDataFormat_PCM_EX. If running on
   * API 21+ use this newer format type, creating it from our original format.
   */
  SLAndroidDataFormat_PCM_EX format_pcm_ex;
  if (api_level() >= __ANDROID_API_L__) {
    SLuint32 representation = OpenSLES::ConvertFormatToRepresentation(getFormat());
    // Fill in the format structure.
    format_pcm_ex = OpenSLES::CreateExtendedFormat(format_pcm, representation);
    // Use in place of the previous format.
    audioSrc.pFormat = &format_pcm_ex;
  }

  result = OutputMixerOpenSLES::getInstance().createAudioPlayer(
      &mObjectInterface, &audioSrc);
  if (SL_RESULT_SUCCESS != result) {
    MINILOG_ERROR("createAudioPlayer() result: {}", OpenSLES::SLErrToStr(result));
    goto error;
  }

  // Configure the stream.
  result = (*mObjectInterface)
      ->GetInterface(mObjectInterface, SL_IID_ANDROIDCONFIGURATION,
          (void *)&configItf);
  if (SL_RESULT_SUCCESS != result) {
    MINILOG_ERROR("GetInterface(SL_IID_ANDROIDCONFIGURATION) failed with {}", OpenSLES::SLErrToStr(result));
  } else {
    result = configurePerformanceMode(configItf);
    if (SL_RESULT_SUCCESS != result) {
      goto error;
    }

    SLuint32 presetValue = OpenSLES_convertOutputUsage(mUsage);
    result = (*configItf)
        ->SetConfiguration(configItf, SL_ANDROID_KEY_STREAM_TYPE,
            &presetValue, sizeof(presetValue));
    if (SL_RESULT_SUCCESS != result) {
      goto error;
    }
  }

  result = (*mObjectInterface)->Realize(mObjectInterface, SL_BOOLEAN_FALSE);
  if (SL_RESULT_SUCCESS != result) {
    MINILOG_ERROR("Realize player object result: {}", OpenSLES::SLErrToStr(result));
    goto error;
  }

  result = (*mObjectInterface)->GetInterface(mObjectInterface, SL_IID_PLAY, &mPlayInterface);
  if (SL_RESULT_SUCCESS != result) {
    MINILOG_ERROR("GetInterface PLAY result: {}", OpenSLES::SLErrToStr(result));
    goto error;
  }

  result = finishCommonOpen(configItf);
  if (SL_RESULT_SUCCESS != result) {
    goto error;
  }

  setState(StreamState::Open);
  return Result::OK;

error:
  close(); // Clean up various OpenSL objects and prevent resource leaks.
  // TODO convert error from SLES to CORAL
  return Result::ErrorInternal;
}

Result AudioStreamOutputOpenSLES::onAfterDestroy() {
  OutputMixerOpenSLES::getInstance().close();
  return Result::OK;
}

Result AudioStreamOutputOpenSLES::close() {
  std::lock_guard<std::mutex> lock(mLock);
  Result result = Result::OK;
  if (getState() == StreamState::Closed) {
    result = Result::ErrorClosed;
  } else {
    (void)requestPauseLocked();
    sleepBeforeClose();
    // invalidate any interfaces
    mPlayInterface = nullptr;
    result = AudioStreamOpenSLES::closeLocked();
  }
  return result;
}

Result AudioStreamOutputOpenSLES::setPlayStateLocked(SLuint32 newState) {
  Result result = Result::OK;

  if (mPlayInterface == nullptr) {
    MINILOG_ERROR("mPlayInterface is null");
    return Result::ErrorInvalidState;
  }

  SLresult slResult = (*mPlayInterface)->SetPlayState(mPlayInterface, newState);
  if (SL_RESULT_SUCCESS != slResult) {
    MINILOG_WARN("SetPlayState returned {}", OpenSLES::SLErrToStr(slResult));
    // TODO convert slResult to Result::Error
    result = Result::ErrorInternal;
  }
  return result;
}

Result AudioStreamOutputOpenSLES::requestStart() {
  mLock.lock();
  auto initialState = getState();
  switch (initialState) {
    case StreamState::Starting:
    case StreamState::Started: {
      mLock.unlock();
      return Result::OK;
    }
    case StreamState::Closed: {
      mLock.unlock();
      return Result::ErrorClosed;
    }
    default:
      break;
  }

  // We use a callback if the user requests one
  // OR if we have an internal callback to read the blocking IO buffer.
  setDataCallbackEnabled(true);

  setState(StreamState::Starting);

  if (getBufferDepth(mSimpleBufferQueueInterface) == 0) {
    // Enqueue the first buffer if needed to start the streaming.
    // We may need to stop the current stream.
    bool shouldStopStream = processBufferCallback(mSimpleBufferQueueInterface);
    if (shouldStopStream) {
      MINILOG_DEBUG("Stopping the current stream.");
      if (requestStopLocked() != Result::OK) {
        MINILOG_WARN("Failed to flush the stream. Error {}", convertToText(flush()));
      }
      setState(initialState);
      mLock.unlock();
      return Result::ErrorClosed;
    }
  }

  Result result = setPlayStateLocked(SL_PLAYSTATE_PLAYING);
  if (result == Result::OK) {
    setState(StreamState::Started);
    mLock.unlock();
  } else {
    setState(initialState);
    mLock.unlock();
  }
  return result;
}

Result AudioStreamOutputOpenSLES::requestPause() {
  std::lock_guard<std::mutex> lock(mLock);
  return requestPauseLocked();
}

Result AudioStreamOutputOpenSLES::requestPauseLocked() {
  auto initialState = getState();
  switch (initialState) {
    case StreamState::Pausing:
    case StreamState::Paused: {
      return Result::OK;
    }
    case StreamState::Uninitialized:
    case StreamState::Closed: {
      return Result::ErrorClosed;
    }
    default:
      break;
  }

  setState(StreamState::Pausing);
  Result result = setPlayStateLocked(SL_PLAYSTATE_PAUSED);
  if (result == Result::OK) {
    // Note that OpenSL ES does NOT reset its millisecond position when OUTPUT
    // is paused.
    int64_t framesWritten = getFramesWritten();
    if (framesWritten >= 0) {
      setFramesRead(framesWritten);
    }
    setState(StreamState::Paused);
  } else {
    setState(initialState);
  }
  return result;
}

Result AudioStreamOutputOpenSLES::requestFlush() {
  std::lock_guard<std::mutex> lock(mLock);
  return requestFlushLocked();
}

Result AudioStreamOutputOpenSLES::requestFlushLocked() {
  if (getState() == StreamState::Closed) {
    return Result::ErrorClosed;
  }

  Result result = Result::OK;
  if (mPlayInterface == nullptr || mSimpleBufferQueueInterface == nullptr) {
    result = Result::ErrorInvalidState;
  } else {
    SLresult slResult =
        (*mSimpleBufferQueueInterface)->Clear(mSimpleBufferQueueInterface);
    if (slResult != SL_RESULT_SUCCESS) {
      MINILOG_WARN("Failed to clear buffer queue. OpenSLES error: {}", convertToText(result));
      result = Result::ErrorInternal;
    }
  }
  return result;
}

Result AudioStreamOutputOpenSLES::requestStop() {
  std::lock_guard<std::mutex> lock(mLock);
  return requestStopLocked();
}

Result AudioStreamOutputOpenSLES::requestStopLocked() {
  auto initialState = getState();
  switch (initialState) {
    case StreamState::Stopping:
    case StreamState::Stopped: {
      return Result::OK;
    }
    case StreamState::Uninitialized:
    case StreamState::Closed: {
      return Result::ErrorClosed;
    }
    default:
      break;
  }

  setState(StreamState::Stopping);

  Result result = setPlayStateLocked(SL_PLAYSTATE_STOPPED);
  if (result == Result::OK) {

    // Also clear the buffer queue so the old data won't be played if the stream
    // is restarted. Call the _l function that expects to already be under a
    // lock.
    if (requestFlushLocked() != Result::OK) {
      MINILOG_WARN("Failed to flush the stream. Error {}", convertToText(flush()));
    }

    // OpenSL ES resets its millisecond position when stopped.
    mPositionMillis.reset32();
    int64_t framesWritten = getFramesWritten();
    if (framesWritten >= 0) {
      setFramesRead(framesWritten);
    }
    setState(StreamState::Stopped);
  } else {
    setState(initialState);
  }
  return result;
}

void AudioStreamOutputOpenSLES::setFramesRead(int64_t framesRead) {
  int64_t millisWritten = framesRead * Definitions::kMillisPerSecond / getSampleRate();
  mPositionMillis.set(millisWritten);
}

void AudioStreamOutputOpenSLES::updateFramesRead() {
  if (usingFIFO()) {
    AudioStreamBuffered::updateFramesRead();
  } else {
    mFramesRead = getFramesProcessedByServer();
  }
}

Result AudioStreamOutputOpenSLES::updateServiceFrameCounter() {
  Result result = Result::OK;
  // Avoid deadlock if another thread is trying to stop or close this stream
  // and this is being called from a callback.
  if (mLock.try_lock()) {

    if (mPlayInterface == nullptr) {
      mLock.unlock();
      return Result::ErrorNull;
    }
    SLmillisecond msec = 0;
    SLresult slResult = (*mPlayInterface)->GetPosition(mPlayInterface, &msec);
    if (SL_RESULT_SUCCESS != slResult) {
      MINILOG_WARN("GetPosition() returned {}", OpenSLES::SLErrToStr(slResult));
      // set result based on SLresult
      result = Result::ErrorInternal;
    } else {
      mPositionMillis.update32((int32_t)msec);
    }
    mLock.unlock();
  }
  return result;
}

} // namespace coral
