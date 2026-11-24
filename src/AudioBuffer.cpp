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

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioUtils.h>

#include <media/Types.h>

#include <algorithm>

namespace coral {

std::unique_ptr<AudioPCMBuffer>
AudioPCMBuffer::InitWithPCMFormat(const AudioFormat& format, const uint32_t frameCapacity) {
  if (!format.isValid() || format.getBytesPerFrame() == 0) {
    return nullptr;
  }
  return std::unique_ptr<AudioPCMBuffer>(new AudioPCMBuffer(format, frameCapacity));
}

AudioPCMBuffer::AudioPCMBuffer(const AudioFormat& format, const uint32_t frameCapacity) noexcept
  : AudioBufferBase(format),
    mFrameCapacity(frameCapacity),
    mFrameLength(0) {

  const uint32_t channelCount = format.getChannelCount();
  const uint32_t bytesPerFrame = format.getBytesPerFrame();
  const uint32_t bytesPerChannel = format.getBitsPerChannel() / 8;
  const bool interleaved = format.isInterleaved();

  const uint32_t totalBytes = interleaved
                                ? frameCapacity * bytesPerFrame
                                : frameCapacity * bytesPerChannel * channelCount;

  mStorage.assign(totalBytes, 0);

  if (interleaved) {
    mBufferList.resize(1);
    auto* buf = mBufferList.getBuffer(0);
    buf->mNumberChannels = channelCount;
    buf->mDataByteSize = 0; // The frame length starts at zero.
    buf->mData = mStorage.data();
  } else {
    mBufferList.resize(channelCount);
    const uint32_t perChannel = frameCapacity * bytesPerChannel;
    for (auto channel = 0; channel < channelCount; ++channel) {
      auto* buffer = mBufferList.getBuffer(channel);
      buffer->mNumberChannels = 1;
      buffer->mDataByteSize = 0;
      buffer->mData = mStorage.data() + channel * perChannel;
    }
  }
}

void AudioPCMBuffer::setFrameLength(const uint32_t frameLength) {
  mFrameLength = std::min(mFrameCapacity, frameLength);
  const uint32_t bytesPerChannel = mFormat->getBitsPerChannel() / 8;
  if (mFormat->isInterleaved()) {
    mBufferList.getBuffer(0)->mDataByteSize = mFrameLength * mFormat->getBytesPerFrame();
  } else {
    const uint32_t perChannel = mFrameLength * bytesPerChannel;
    for (size_t channel = 0; channel < mBufferList.numberOfBuffers(); ++channel) {
      mBufferList.getBuffer(channel)->mDataByteSize = perChannel;
    }
  }
}

uint32_t AudioPCMBuffer::getStride() const {
  // if the format is deinterleaved, the stride is 1, otherwise, stride is the
  // number of interleaved channels.
  if (!mFormat->isInterleaved()) {
    return 1;
  }
  return mFormat->getChannelCount();
}

template<typename T>
T** AudioPCMBuffer::buildPointerArray(std::vector<T*>& cache) const {
  const uint32_t channelCount = mFormat->getChannelCount();
  if (cache.size() != channelCount) cache.resize(channelCount);

  if (mFormat->isInterleaved()) {
    // Interleaved: All pointers point to different offsets within the same memory block
    // Accessed by the user as [frame * stride + channel], where stride = channelCount
    auto* base = static_cast<T*>(mBufferList.getBuffer(0)->mData);
    for (uint32_t channel = 0; channel < channelCount; ++channel) {
      cache[channel] = base + channel;
    }
  } else {
    // Non-interleaved: Each pointer points to the starting address of the corresponding channel.
    for (uint32_t channel = 0; channel < channelCount; ++channel) {
      cache[channel] = static_cast<T*>(mBufferList.getBuffer(channel)->mData);
    }
  }
  return cache.data();
}

float** AudioPCMBuffer::floatChannelData() const {
  if (mFormat->getSampleFormat() != AudioSampleFormat::kFloat) {
    return nullptr;
  }
  return buildPointerArray(mFloatPtrs);
}

int16_t** AudioPCMBuffer::int16ChannelData() const {
  if (mFormat->getSampleFormat() != AudioSampleFormat::kInt16) {
    return nullptr;
  }
  return buildPointerArray(mInt16Ptrs);
}

int32_t** AudioPCMBuffer::int32ChannelData() const {
  if (mFormat->getSampleFormat() != AudioSampleFormat::kInt32) {
    return nullptr;
  }
  return buildPointerArray(mInt32Ptrs);
}

namespace {
template<typename T>
void convertInterleaved(const T* src, const uint32_t frames, const uint32_t channels,
  std::vector<std::vector<float>>& out, const float scale) {
  for (uint32_t channel = 0; channel < channels; ++channel) {
    for (uint32_t f = 0; f < frames; ++f) {
      out[channel][f] = static_cast<float>(src[f * channels + channel]) * scale;
    }
  }
}

template<typename T>
void convertPlanar(const AudioBufferList& list, const uint32_t frames,
  std::vector<std::vector<float>>& out, const float scale) {
  for (size_t channel = 0; channel < out.size(); ++channel) {
    const auto* src = static_cast<const T*>(list.getBuffer(channel)->mData);
    for (uint32_t f = 0; f < frames; ++f) {
      out[channel][f] = static_cast<float>(src[f]) * scale;
    }
  }
}
} // namespace anonymous

std::vector<std::vector<float>> AudioPCMBuffer::toFloatChannelData() const {
  const uint32_t channels = mFormat->getChannelCount();
  const uint32_t frames = mFrameLength;
  std::vector<std::vector<float>> result(channels, std::vector<float>(frames, 0.0f));
  if (frames == 0) {
    return result;
  }

  const bool interleaved = mFormat->isInterleaved();
  const auto* buffer = mBufferList.getBuffer(0);

  switch (mFormat->getSampleFormat()) {
    case AudioSampleFormat::kFloat: {
      constexpr float kScale = 1.0f;
      if (interleaved) {
        convertInterleaved(static_cast<const float*>(buffer->mData), frames, channels, result, kScale);
      } else {
        convertPlanar<float>(mBufferList, frames, result, kScale);
      }
      break;
    }
    case AudioSampleFormat::kInt16: {
      constexpr float kScale = 1.0f / 32768.0f;
      if (interleaved) {
        convertInterleaved(static_cast<const int16_t*>(buffer->mData), frames, channels, result, kScale);
      } else {
        convertPlanar<int16_t>(mBufferList, frames, result, kScale);
      }
      break;
    }
    case AudioSampleFormat::kInt24: {
      if (interleaved) {
        const auto* src = static_cast<const uint8_t*>(buffer->mData);
        for (uint32_t channel = 0; channel < channels; ++channel) {
          for (uint32_t f = 0; f < frames; ++f) {
            const uint8_t* sample = src + (f * channels + channel) * 3;
            result[channel][f] = float_from_p24(sample);
          }
        }
      } else {
        for (uint32_t channel = 0; channel < channels; ++channel) {
          const auto* src = static_cast<const uint8_t*>(mBufferList.getBuffer(channel)->mData);
          for (uint32_t f = 0; f < frames; ++f) {
            const uint8_t* sample = src + f * 3;
            result[channel][f] = float_from_p24(sample);
          }
        }
      }
      break;
    }
    case AudioSampleFormat::kInt32: {
      constexpr float kScale = 1.0f / 2147483648.0f;
      if (interleaved) {
        convertInterleaved(static_cast<const int32_t*>(buffer->mData), frames, channels, result, kScale);
      } else {
        convertPlanar<int32_t>(mBufferList, frames, result, kScale);
      }
      break;
    }
    default: {
      break;
    }
  }
  return result;
}

//----------------------------------------------------------------------------------------------------------------------

namespace {
bool isPCMFormat(const AudioFormat& format) {
  switch (format.getSampleFormat()) {
    case AudioSampleFormat::kInt16:
    case AudioSampleFormat::kFloat:
    case AudioSampleFormat::kInt24:
    case AudioSampleFormat::kInt32:
      return true;
    default:
      return false;
  }
}
} // namespace anonymous

std::unique_ptr<AudioCompressedBuffer>
AudioCompressedBuffer::InitWithFormat(const AudioFormat& format, const AudioPacketCount packetCapacity) {
  if (isPCMFormat(format) || format.getFormatType() == AudioFormatType::kAudioFormatTypeLinearPCM) {
    return nullptr;
  }
  if (format.getBytesPerFrame() == 0) {
    return nullptr;
  }
  return std::unique_ptr<AudioCompressedBuffer>(new AudioCompressedBuffer(format, packetCapacity,
    format.getBytesPerFrame()));
}

std::unique_ptr<AudioCompressedBuffer>
AudioCompressedBuffer::InitWithFormat(const AudioFormat& format,
  const AudioPacketCount packetCapacity,
  const uint32_t maximumPacketSize) {
  if (isPCMFormat(format) || format.getFormatType() == AudioFormatType::kAudioFormatTypeLinearPCM) {
    return nullptr;
  }
  return std::unique_ptr<AudioCompressedBuffer>(new AudioCompressedBuffer(format, packetCapacity, maximumPacketSize));
}

AudioCompressedBuffer::AudioCompressedBuffer(const AudioFormat& format,
  const AudioPacketCount capacity,
  const uint32_t maxPacketSize)
  : AudioBufferBase(format),
    mPacketCount(1),
    mMaximumPacketSize(maxPacketSize),
    mPacketCapacity(capacity),
    mByteCapacity(capacity * maxPacketSize),
    mByteLength(0) {

  mStorage.assign(mByteCapacity, 0);

  auto* buffer = mBufferList.getBuffer(0);
  buffer->mNumberChannels = format.getChannelCount();
  buffer->mDataByteSize = 0;
  buffer->mData = mStorage.data();

  if (format.getBytesPerFrame() == 0 || format.getBitsPerChannel() == 0) {
    mPacketDescriptions.resize(capacity);
  }
}

void AudioCompressedBuffer::setPacketCount(const AudioPacketCount packetCount) {
  mPacketCount = std::min(packetCount, mPacketCapacity);
}

void AudioCompressedBuffer::setByteLength(const uint32_t newByteLength) {
  mByteLength = std::min(newByteLength, mByteCapacity);
  mBufferList.getBuffer(0)->mDataByteSize = mByteLength;
}

void AudioCompressedBuffer::reset() {
  mByteLength = 0;
  mPacketCount = 0;
  mBufferList.getBuffer(0)->mDataByteSize = 0;
  std::ranges::fill(mStorage, 0);
  std::ranges::fill(mPacketDescriptions, AudioPacketDescription{});
}

} // namespace coral
