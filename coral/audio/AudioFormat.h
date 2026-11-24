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

#ifndef CORAL_AUDIO_FORMAT_H
#define CORAL_AUDIO_FORMAT_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioChannelLayout.h>
#include <coral/audio/AudioTypes.h>

#include <memory>
#include <optional>
#include <ostream>
#include <string>

namespace coral {

/**
 * A representation of an audio format.
 */
class AudioFormat final : public AudioCopyable {
public:
  /**
   * Initialize an AudioFormat with default value.
   */
  static AudioFormat DefaultFormat();

  /**
   * Initialize from an AudioBasicDescription and optional channel layout.
   *
   * @param abd    the AudioBasicDescription
   * @param layout the channel layout. Can be nil only if abd specifies 1 or 2 channels.
   *
   * @return If the format specifies more than 2 channels, this method fails (returns std::nullopt)
   *         unless layout is non-nil.
   */
  static std::optional<AudioFormat>
  InitWithStreamDescription(const AudioBasicDescription& abd, std::optional<AudioChannelLayout> layout);

  /**
   * Initialize to deinterleaved float with the specified sample rate and channel count.
   *
   * @param sampleRate the sample rate
   * @param channels the channel count
   *
   * @return If the format specifies more than 2 channels, this method fails and returns std::nullopt.
   */
  static std::optional<AudioFormat>
  InitStandardFormatWithSampleRate(Float64 sampleRate, AudioChannelCount channels);

  /**
   * Initialize to float with the specified sample rate, channel count and interleavedness.
   *
   * @param sampleFormat the audio sample format
   * @param sampleRate   the sample rate
   * @param channels     the channel count
   * @param interleaved  true if interleaved
   *
   * @return If the format specifies more than 2 channels, this method fails (returns std::nullopt).
   */
  static std::optional<AudioFormat>
  InitWithCommonFormat(AudioSampleFormat sampleFormat,
    Float64 sampleRate,
    AudioChannelCount channels,
    bool interleaved);

  /**
   * Returns the getAudioFormatType
   */
  [[nodiscard]] AudioFormatType getFormatType() const {
    return mDescription.mFormatType;
  }

  /**
   * Returns whether the format is deinterleaved native-endian float.
   */
  [[nodiscard]] bool isStandard() const {
    return getSampleFormat() == AudioSampleFormat::kFloat && !isInterleaved();
  }

  /**
   * Returns the `AudioSampleFormat` identifying the format
   */
  [[nodiscard]] AudioSampleFormat getSampleFormat() const {
    return mSampleFormat;
  }

  /**
   * Returns the number of channels of audio data.
   */
  [[nodiscard]] uint32_t getChannelCount() const {
    return mDescription.mChannelsPerFrame;
  }

  /**
   * Returns a sampling rate in Hertz.
   */
  [[nodiscard]] Float64 getSampleRate() const {
    return mDescription.mSampleRate;
  }

  /**
   * Returns the size of one frame in bytes
   */
  [[nodiscard]] uint32_t getBytesPerFrame() const {
    return mDescription.mBytesPerFrame;
  }

  /**
   * Returns the number of bit per channel
   */
  [[nodiscard]] uint32_t getBitsPerChannel() const {
    return mDescription.mBitsPerChannel;
  }

  /**
   * Returns the number of sample frames in each packet of data.
   */
  [[nodiscard]] uint32_t getFramesPerPacket() const {
    return mDescription.mFramesPerPacket;
  }

  /**
   * Returns whether the samples are interleaved.
   */
  [[nodiscard]] bool isInterleaved() const {
    return (mDescription.mFormatFlags & AudioFormatFlags::kAudioFormatFlagIsNonInterleaved) == 0;
  }

  /**
   * Indicates the AudioFormat is valid.
   */
  [[nodiscard]] bool isValid() const {
    if (mDescription.mChannelsPerFrame == 0 || mDescription.mSampleRate == 0) {
      return false;
    }
    // If we use for PCM, the AudioFormatType must be AudioFormatType::kAudioFormatTypeLinearPCM
    if (mDescription.mFormatType == AudioFormatType::kAudioFormatTypeLinearPCM) {
      return mSampleFormat != AudioSampleFormat::kDefault &&
             mSampleFormat != AudioSampleFormat::kInvalid;
    }
    // Whether is compressed format.
    return !isCompressed();
  }

  /**
   * Indicates whether the format is for compressed buffer.
   */
  [[nodiscard]] bool isCompressed() const {
    return mDescription.mFormatType != AudioFormatType::kAudioFormatTypeInvalid &&
           mDescription.mFormatType != AudioFormatType::kAudioFormatTypeLinearPCM;
  }

  /**
   * Returns the AudioBasicDescription, for use with lower-level audio API's.
   */
  [[nodiscard]] const AudioBasicDescription* getDescription() const {
    return &mDescription;
  }

  /**
   * Returns the underlying AudioChannelLayout, if any.
   *
   * Only formats with more than 2 channels are required to have channel layouts.
   */
  [[nodiscard]] std::optional<AudioChannelLayout> getChannelLayout() const {
    return mLayout;
  }

  [[nodiscard]] std::string toString() const;

  friend std::ostream& operator<<(std::ostream& os, const AudioFormat& format) {
    return os << format.toString();
  }

  friend bool operator==(const std::optional<AudioFormat>& a, const std::optional<AudioFormat>& b) {
    if (a == std::nullopt || b == std::nullopt) {
      return false;
    }
    return a.value() == b.value();
  }

  friend bool operator!=(const std::optional<AudioFormat>& a, const std::optional<AudioFormat>& b) {
    return !(a == b);
  }

  friend bool operator==(const AudioFormat& a, const AudioFormat& b) {
    return a.mSampleFormat == b.mSampleFormat && a.mDescription == b.mDescription;
  }

  friend bool operator!=(const AudioFormat& a, const AudioFormat& b) {
    return !(a == b);
  }

private:
  AudioFormat();

  explicit AudioFormat(const AudioBasicDescription& description,
    std::optional<AudioChannelLayout> layout,
    AudioSampleFormat sampleFormat) noexcept;

  static constexpr int kBitsPerByte = 8;

  static AudioFormatFlags ConvertToAudioFormatFlags(AudioSampleFormat format, bool interleaved);

  AudioBasicDescription mDescription;
  std::optional<AudioChannelLayout> mLayout;
  AudioSampleFormat mSampleFormat;
};

} // namespace coral

#endif // CORAL_AUDIO_FORMAT_H
