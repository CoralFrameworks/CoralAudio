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

#include "AudioStreamInputOpenSLES.h"
#include "OpenSLESUtilities.h"

#include <minilog/log.h>
#include <utils/api_level.h>

using namespace utils;

namespace coral {

static SLuint32 OpenSLES_convertInputPreset(InputPreset preset) {
  SLuint32 openslPreset = SL_ANDROID_RECORDING_PRESET_NONE;
  switch (preset) {
    case InputPreset::Generic: {
      openslPreset = SL_ANDROID_RECORDING_PRESET_GENERIC;
      break;
    }
    case InputPreset::Camcorder: {
      openslPreset = SL_ANDROID_RECORDING_PRESET_CAMCORDER;
      break;
    }
    case InputPreset::VoiceRecognition:
    case InputPreset::VoicePerformance: {
      openslPreset = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
      break;
    }
    case InputPreset::VoiceCommunication: {
      openslPreset = SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION;
      break;
    }
    case InputPreset::Unprocessed: {
      openslPreset = SL_ANDROID_RECORDING_PRESET_UNPROCESSED;
      break;
    }
    default: {
      break;
    }
  }
  return openslPreset;
}

AudioStreamInputOpenSLES::AudioStreamInputOpenSLES(
    const AudioStreamBuilder &builder) noexcept
    : AudioStreamOpenSLES(builder) {
}

SLuint32
AudioStreamInputOpenSLES::channelCountToChannelMask(int channelCount) const {
  // Derived from internal sles_channel_in_mask_from_count(chanCount);
  // in "frameworks/wilhelm/src/android/channels.cpp".
  // Yes, it seems strange to use SPEAKER constants to describe inputs.
  // But that is how OpenSL ES does it internally.
  switch (channelCount) {
    case 1:return SL_SPEAKER_FRONT_LEFT;
    case 2:return SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
    default:return channelCountToChannelMaskDefault(channelCount);
  }
}

Result AudioStreamInputOpenSLES::open() {
  logUnsupportedAttributes();

  SLAndroidConfigurationItf configItf = nullptr;

  if (api_level() < __ANDROID_API_M__ &&
      mFormat == AudioSampleFormat::kFloat) {
    // TODO: Allow floating point format on API <23 using float->int16 converter
    return Result::ErrorInvalidFormat;
  }

  // If audio format is unspecified then choose a suitable default.
  // API 23+: FLOAT
  // API <23: INT16
  if (mFormat == AudioSampleFormat::kDefault) {
    mFormat = (api_level() < __ANDROID_API_M__)
        ? AudioSampleFormat::kInt16
        : AudioSampleFormat::kFloat;
  }

  Result opResult = AudioStreamOpenSLES::open();
  if (Result::OK != opResult) {
    return opResult;
  }

  auto bitsPerSample = static_cast<SLuint32>(getBytesPerSample() * kBitsPerByte);

  // configure audio sink
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

  SLDataSink audioSink = {&loc_bufq, &format_pcm};

  /**
   * API 23 (Marshmallow) introduced support for floating-point data
   * representation and an extended data format type: SLAndroidDataFormat_PCM_EX
   * for recording streams (playback streams got this in API 21). If running on
   * API 23+ use this newer format type, creating it from our original format.
   */
  SLAndroidDataFormat_PCM_EX format_pcm_ex;
  if (api_level() >= __ANDROID_API_M__) {
    SLuint32 representation =
        OpenSLES::ConvertFormatToRepresentation(getFormat());
    // Fill in the format structure.
    format_pcm_ex = OpenSLES::CreateExtendedFormat(format_pcm, representation);
    // Use in place of the previous format.
    audioSink.pFormat = &format_pcm_ex;
  }

  // configure audio source
  SLDataLocator_IODevice loc_dev = {
      SL_DATALOCATOR_IODEVICE,
      SL_IODEVICE_AUDIOINPUT,
      SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
  SLDataSource audioSrc = {&loc_dev, nullptr};

  SLresult result = EngineOpenSLES::getInstance().createAudioRecorder(
      &mObjectInterface, &audioSrc, &audioSink);

  if (SL_RESULT_SUCCESS != result) {
    MINILOG_ERROR("CreateAudioRecorder() result: {}", OpenSLES::SLErrToStr(result));
    goto error;
  }

  // Configure the stream.
  result = (*mObjectInterface)
      ->GetInterface(mObjectInterface, SL_IID_ANDROIDCONFIGURATION,
          &configItf);

  if (SL_RESULT_SUCCESS != result) {
    MINILOG_WARN("GetInterface(SL_IID_ANDROIDCONFIGURATION) failed with {}", OpenSLES::SLErrToStr(result));
  } else {
    if (getInputPreset() == InputPreset::VoicePerformance) {
      MINILOG_WARN("OpenSL ES does not support InputPreset::VoicePerformance. Use VoiceRecognition.");
      mInputPreset = InputPreset::VoiceRecognition;
    }
    SLuint32 presetValue = OpenSLES_convertInputPreset(getInputPreset());
    result = (*configItf)->SetConfiguration(configItf, SL_ANDROID_KEY_RECORDING_PRESET,
        &presetValue, sizeof(SLuint32));
    if (SL_RESULT_SUCCESS != result &&
        presetValue != SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION) {
      presetValue = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
      MINILOG_WARN("Setting InputPreset: {} failed. Using VoiceRecognition instead.",
          convertToText(getInputPreset()));
      mInputPreset = InputPreset::VoiceRecognition;
      (*configItf)->SetConfiguration(configItf, SL_ANDROID_KEY_RECORDING_PRESET,
          &presetValue, sizeof(SLuint32));
    }

    result = configurePerformanceMode(configItf);
    if (SL_RESULT_SUCCESS != result) {
      goto error;
    }
  }

  result = (*mObjectInterface)->Realize(mObjectInterface, SL_BOOLEAN_FALSE);
  if (SL_RESULT_SUCCESS != result) {
    MINILOG_ERROR("Realize recorder object result: {}", OpenSLES::SLErrToStr(result));
    goto error;
  }

  result =
      (*mObjectInterface)
          ->GetInterface(mObjectInterface, SL_IID_RECORD, &mRecordInterface);
  if (SL_RESULT_SUCCESS != result) {
    MINILOG_ERROR("GetInterface RECORD result: {}", OpenSLES::SLErrToStr(result));
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

Result AudioStreamInputOpenSLES::close() {
  std::lock_guard<std::mutex> lock(mLock);
  Result result = Result::OK;
  if (getState() == StreamState::Closed) {
    result = Result::ErrorClosed;
  } else {
    (void)requestStopLocked();
    sleepBeforeClose();
    // invalidate any interfaces
    mRecordInterface = nullptr;
    result = AudioStreamOpenSLES::closeLocked();
  }
  return result;
}

Result AudioStreamInputOpenSLES::setRecordStateLocked(SLuint32 newState) {
  Result result = Result::OK;

  if (mRecordInterface == nullptr) {
    MINILOG_WARN("mRecordInterface is null");
    return Result::ErrorInvalidState;
  }
  SLresult slResult =
      (*mRecordInterface)->SetRecordState(mRecordInterface, newState);
  // LOGD("AudioInputStreamOpenSLES::%s(%u) returned %u", __func__, newState,
  // slResult);
  if (SL_RESULT_SUCCESS != slResult) {
    MINILOG_ERROR("{} returned error {}", newState, OpenSLES::SLErrToStr(slResult));
    result = Result::ErrorInternal;
  }
  return result;
}

Result AudioStreamInputOpenSLES::requestStart() {
  std::lock_guard<std::mutex> lock(mLock);
  StreamState initialState = getState();
  switch (initialState) {
    case StreamState::Starting:
    case StreamState::Started:return Result::OK;
    case StreamState::Closed:return Result::ErrorClosed;
    default:break;
  }

  // We use a callback if the user requests one
  // OR if we have an internal callback to fill the blocking IO buffer.
  setDataCallbackEnabled(true);

  setState(StreamState::Starting);

  if (getBufferDepth(mSimpleBufferQueueInterface) == 0) {
    // Enqueue the first buffer to start the streaming.
    // This does not call the callback function.
    enqueueCallbackBuffer(mSimpleBufferQueueInterface);
  }

  Result result = setRecordStateLocked(SL_RECORDSTATE_RECORDING);
  if (result == Result::OK) {
    setState(StreamState::Started);
  } else {
    setState(initialState);
  }
  return result;
}

Result AudioStreamInputOpenSLES::requestPause() {
  MINILOG_ERROR("Intentionally not implemented for input streams");
  // Matches AAudio behavior.
  return Result::ErrorUnimplemented;
}

Result AudioStreamInputOpenSLES::requestFlush() {
  MINILOG_ERROR("Intentionally not implemented for input streams");
  // Matches AAudio behavior.
  return Result::ErrorUnimplemented;
}

Result AudioStreamInputOpenSLES::requestStop() {
  std::lock_guard<std::mutex> lock(mLock);
  return requestStopLocked();
}

Result AudioStreamInputOpenSLES::requestStopLocked() {
  StreamState initialState = getState();
  switch (initialState) {
    case StreamState::Stopping:
    case StreamState::Stopped:return Result::OK;
    case StreamState::Uninitialized:
    case StreamState::Closed:return Result::ErrorClosed;
    default:break;
  }

  setState(StreamState::Stopping);

  Result result = setRecordStateLocked(SL_RECORDSTATE_STOPPED);
  if (result == Result::OK) {
    mPositionMillis
        .reset32(); // OpenSL ES resets its millisecond position when stopped.
    setState(StreamState::Stopped);
  } else {
    setState(initialState);
  }
  return result;
}

void AudioStreamInputOpenSLES::updateFramesWritten() {
  if (usingFIFO()) {
    AudioStreamBuffered::updateFramesWritten();
  } else {
    mFramesWritten = getFramesProcessedByServer();
  }
}

Result AudioStreamInputOpenSLES::updateServiceFrameCounter() {
  Result result = Result::OK;
  // Avoid deadlock if another thread is trying to stop or close this stream
  // and this is being called from a callback.
  if (mLock.try_lock()) {

    if (mRecordInterface == nullptr) {
      mLock.unlock();
      return Result::ErrorNull;
    }
    SLmillisecond msec = 0;
    SLresult slResult =
        (*mRecordInterface)->GetPosition(mRecordInterface, &msec);
    if (SL_RESULT_SUCCESS != slResult) {
      MINILOG_ERROR("GetPosition() returned {}", OpenSLES::SLErrToStr(slResult));
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
