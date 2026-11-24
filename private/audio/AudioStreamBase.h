/*
 * Copyright (C) 2026 The Coral Project
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

#ifndef CORAL_PRIVATE_AUDIO_STREAM_BASE_H
#define CORAL_PRIVATE_AUDIO_STREAM_BASE_H

#include "private/audio/AudioStreamCallback.h"
#include "private/audio/AudioStreamTypes.h"

#include <coral/audio/AudioTypes.h>

#include <memory>

namespace coral {

class AudioStreamBase {
public:
  AudioStreamBase() = default;
  virtual ~AudioStreamBase() = default;
  AudioStreamBase(const AudioStreamBase&) = default;
  AudioStreamBase(AudioStreamBase&&) = default;
  AudioStreamBase& operator=(const AudioStreamBase&) = default;
  AudioStreamBase& operator=(AudioStreamBase&&) = default;

  /**
   * Represents any attribute, property or value which hasn't been specified.
   */
  static constexpr uint32_t kUnspecified = 0;

  /**
 * Returns the direction of the stream
 */
  [[nodiscard]] Direction getDirection() const {
    return mDirection;
  }

  /**
   * Returns the number of channels, for example 2 for stereo, or 0
   */
  [[nodiscard]] uint32_t getChannelCount() const {
    return mChannelCount;
  }

  /**
   * Returns the sample rate for the stream, or 0
   */
  [[nodiscard]] uint64_t getSampleRate() const {
    return mSampleRate;
  }

  /**
   * Returns the audio sample format
   */
  [[nodiscard]] AudioSampleFormat getFormat() const {
    return mFormat;
  }

  /**
   * Returns the number of frames in each data callback or 0.
   */
  [[nodiscard]] uint32_t getFramesPerDataCallback() const {
    return mFramesPerCallback;
  }

  /**
   * Query the maximum number of frames that can be filled without blocking.
   * If the stream has been closed the last known value will be returned.
   *
   * @return buffer size
   */
  virtual uint32_t getBufferSizeInFrames() {
    return mBufferSizeInFrames;
  }

  /**
   * Returns the capacity in frames or kUnspecified.
   */
  [[nodiscard]] virtual uint32_t getBufferCapacityInFrames() const {
    return mBufferCapacityInFrames;
  }

  /**
   * Returns true if a data callback was set for this stream
   */
  [[nodiscard]] bool isDataCallbackSpecified() const {
    return mDataCallback != nullptr;
  }

  /**
   * Note that if the app does not set an error callback then a
   * default one may be provided.
   * @return true if an error callback was set for this stream
   */
  [[nodiscard]] bool isErrorCallbackSpecified() const {
    return mErrorCallback != nullptr;
  }

  /**
   * Returns the performance mode of the stream.
   */
  [[nodiscard]] PerformanceMode getPerformanceMode() const {
    return mPerformanceMode;
  }

  /***
   * Returns the sharing mode of the stream
   */
  [[nodiscard]] SharingMode getSharingMode() const {
    return mSharingMode;
  }

  /**
 * Returns the stream's input preset.
 */
  [[nodiscard]] InputPreset getInputPreset() const {
    return mInputPreset;
  }

  /**
   * Returns whether if can convert channel counts to achieve optimal results.
   */
  [[nodiscard]] bool isChannelConversionAllowed() const {
    return mChannelConversionAllowed;
  }

  /**
   * Returns whether if can convert data formats to achieve optimal results.
   */
  [[nodiscard]] bool isFormatConversionAllowed() const {
    return mFormatConversionAllowed;
  }

  /**
   * Returns the quality for sample rate conversion.
   */
  [[nodiscard]] AudioQuality getSampleRateConversionQuality() const {
    return mSampleRateConversionQuality;
  }

protected:
  //!< Stream direction
  Direction mDirection = Direction::Render;
  //!< Stream channel count
  AudioChannelCount mChannelCount = kUnspecified;
  //!< Stream sample rate
  uint64_t mSampleRate = kUnspecified;
  //!< Format of audio frames
  AudioSampleFormat mFormat = AudioSampleFormat::kDefault;
  //!< Number of audio frames which will be requested in each callback
  AudioFrameCount mFramesPerCallback = kUnspecified;
  //!< Stream buffer capacity specified as a number of audio frames
  AudioFrameCount mBufferCapacityInFrames = kUnspecified;
  //!< Stream buffer size specified as a number of audio frames
  AudioFrameCount mBufferSizeInFrames = kUnspecified;
  //!< Stream performance mode
  PerformanceMode mPerformanceMode = PerformanceMode::None;
  //!< Stream sharing mode
  SharingMode mSharingMode = SharingMode::Exclusive;

  //!< Stream input preset. For Android, only active on API 28+
  InputPreset mInputPreset = InputPreset::VoiceRecognition;

  //!< Control whether can convert channel counts to achieve optimal results.
  bool mChannelConversionAllowed = false;
  //!< Control whether can convert data formats to achieve optimal results.
  bool mFormatConversionAllowed = false;
  //!< Control whether and how can convert sample rates to achieve optimal results.
  AudioQuality mSampleRateConversionQuality = AudioQuality::Fastest;

  std::shared_ptr<AudioStreamDataCallback> mDataCallback = nullptr;
  std::shared_ptr<AudioStreamErrorCallback> mErrorCallback = nullptr;

  /** Validate stream parameters that might not be checked in lower layers */
  [[nodiscard]] Result isValidConfig() const {
    switch (mFormat) {
      case AudioSampleFormat::kDefault:
      case AudioSampleFormat::kInt16:
      case AudioSampleFormat::kFloat:
      case AudioSampleFormat::kInt24:
      case AudioSampleFormat::kInt32:
        break;

      default:
        return Result::ErrorInvalidFormat;
    }

    switch (mSampleRateConversionQuality) {
      case AudioQuality::Fastest:
      case AudioQuality::Low:
      case AudioQuality::Medium:
      case AudioQuality::High:
      case AudioQuality::Best:
        return Result::OK;
      default:
        return Result::ErrorIllegalArgument;
    }
  }
};

} // namespace coral

#endif //CORAL_PRIVATE_AUDIO_STREAM_BASE_H
