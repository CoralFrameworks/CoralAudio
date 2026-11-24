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

#include <coral/audio/AudioFormat.h>
#include <media/Utilities.h>

#include <sstream>
#include <utility>

namespace coral {

AudioFormat AudioFormat::DefaultFormat() {
  const auto bytesPerFrame =
    MediaUtils::getBytesPerFrame(AudioSampleFormat::kFloat, Definitions::DEFAULT_CHANNEL_COUNT);
  auto audioFormat = AudioFormat();
  audioFormat.mSampleFormat = AudioSampleFormat::kFloat;
  audioFormat.mDescription = {
    .mSampleRate = Definitions::DEFAULT_SAMPLE_RATE,
    .mFormatType = AudioFormatType::kAudioFormatTypeLinearPCM,
    .mFormatFlags = AudioFormatFlags::kLinearPCMFormatFlagIsFloat,
    .mBytesPerPacket = bytesPerFrame,
    .mFramesPerPacket = 1,
    .mBytesPerFrame = bytesPerFrame,
    .mChannelsPerFrame = Definitions::DEFAULT_CHANNEL_COUNT,
    .mBitsPerChannel = MediaUtils::getBytesPerSample(AudioSampleFormat::kFloat) * kBitsPerByte,
  };
  return audioFormat;
}

std::optional<AudioFormat>
AudioFormat::InitWithStreamDescription(const AudioBasicDescription& abd, std::optional<AudioChannelLayout> layout) {
  if (abd.mChannelsPerFrame > 2 && layout == std::nullopt) {
    return std::nullopt;
  }
  auto sampleFormat = AudioSampleFormat::kDefault;
  if ((abd.mFormatFlags & AudioFormatFlags::kAudioFormatFlagIsFloat) != 0) {
    sampleFormat = AudioSampleFormat::kFloat;
  } else if ((abd.mFormatFlags & AudioFormatFlags::kAudioFormatFlagIsSignedInteger) != 0) {
    if (abd.mBitsPerChannel == 16) {
      sampleFormat = AudioSampleFormat::kInt16;
    } else if (abd.mBitsPerChannel == 24) {
      sampleFormat = AudioSampleFormat::kInt24;
    } else if (abd.mBitsPerChannel == 32) {
      sampleFormat = AudioSampleFormat::kInt32;
    }
  }
  return AudioFormat(abd, std::move(layout), sampleFormat);
}

std::optional<AudioFormat>
AudioFormat::InitStandardFormatWithSampleRate(const Float64 sampleRate, const AudioChannelCount channels) {
  return InitWithCommonFormat(AudioSampleFormat::kFloat, sampleRate, channels, false);
}

std::optional<AudioFormat>
AudioFormat::InitWithCommonFormat(const AudioSampleFormat sampleFormat,
  const Float64 sampleRate,
  const AudioChannelCount channels,
  const bool interleaved) {
  if (channels > 2) {
    return std::nullopt;
  }

  const auto bytesPerFrame = MediaUtils::getBytesPerFrame(sampleFormat, channels);

  auto audioFormat = AudioFormat();
  audioFormat.mSampleFormat = sampleFormat;
  audioFormat.mDescription = {
    .mSampleRate = sampleRate,
    .mFormatType = AudioFormatType::kAudioFormatTypeLinearPCM,
    .mFormatFlags = ConvertToAudioFormatFlags(sampleFormat, interleaved),
    .mBytesPerPacket = bytesPerFrame,
    .mFramesPerPacket = 1,
    .mBytesPerFrame = bytesPerFrame,
    .mChannelsPerFrame = channels,
    .mBitsPerChannel = MediaUtils::getBytesPerSample(sampleFormat) * kBitsPerByte,
  };

  return audioFormat;
}

AudioFormat::AudioFormat()
  : mDescription(),
    mLayout(std::nullopt),
    mSampleFormat(AudioSampleFormat::kInvalid) {
}

AudioFormat::AudioFormat(const AudioBasicDescription& description,
  std::optional<AudioChannelLayout> layout,
  const AudioSampleFormat sampleFormat) noexcept
  : mDescription(description),
    mLayout(std::move(layout)),
    mSampleFormat(sampleFormat) {
}

AudioFormatFlags AudioFormat::ConvertToAudioFormatFlags(const AudioSampleFormat format, const bool interleaved) {
  uint32_t result = 0;
  switch (format) {
    case AudioSampleFormat::kFloat:
      result |= AudioFormatFlags::kAudioFormatFlagIsFloat;
      break;
    case AudioSampleFormat::kInt16:
    case AudioSampleFormat::kInt32:
      result |= AudioFormatFlags::kAudioFormatFlagIsSignedInteger;
      break;
    case AudioSampleFormat::kInt24:
      result |= AudioFormatFlags::kAudioFormatFlagIsPacked;
      break;
    default:
      break;
  }
  if (!interleaved) {
    result |= AudioFormatFlags::kAudioFormatFlagIsNonInterleaved;
  }
  return static_cast<AudioFormatFlags>(result);
}

std::string AudioFormat::toString() const {
  std::stringstream out;
  out << "{codec_type: " << convertToText(getFormatType())
    << ", format: " << convertToText(getSampleFormat())
    << ", channel_count: " << getChannelCount()
    << ", sample_rate: " << getSampleRate()
    << ", interleaved: " << (isInterleaved() ? "true" : "false")
    << "}";
  return out.str();
}

} // namespace coral
