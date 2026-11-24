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

#include "private/graph/AudioGraphUtils.h"

// SampleTraits converts every supported sample format to and from the float
// [-1, 1] range that the graph works in.
#include "private/common/AudioRingBuffer.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace coral {

namespace {

using Packed24 = SampleDataPacked24;

/**
 * The number of frames that both the request and the available frames allow.
 */
AudioFrameCount framesToConvert(const AudioFrameCount numFrames, const AudioFrameCount available) {
  return std::min(numFrames, available);
}

/**
 * Converts deinterleaved float samples into an interleaved buffer.
 */
template<typename T>
void planarFloatToInterleaved(const float* const* src,
  const uint32_t srcStride,
  T* dst,
  const uint32_t channels,
  const AudioFrameCount frames) {
  for (AudioFrameCount f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < channels; ++c) {
      dst[static_cast<size_t>(f) * channels + c] =
        SampleTraits<T>::fromFloat(src[c][static_cast<size_t>(f) * srcStride]);
    }
  }
}

/**
 * Converts an interleaved buffer into deinterleaved float samples.
 */
template<typename T>
void interleavedToPlanarFloat(const T* src,
  const uint32_t channels,
  float* const* dst,
  const uint32_t dstStride,
  const AudioFrameCount frames) {
  for (AudioFrameCount f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < channels; ++c) {
      dst[c][static_cast<size_t>(f) * dstStride] =
        SampleTraits<T>::toFloat(src[static_cast<size_t>(f) * channels + c]);
    }
  }
}

/**
 * Copies deinterleaved float samples into the channels of another PCM buffer.
 */
template<typename T>
void copyPlanarFloatToChannels(const float* const* src,
  const uint32_t srcStride,
  T* const* dst,
  const uint32_t dstStride,
  const uint32_t channels,
  const AudioFrameCount srcOffset,
  const AudioFrameCount dstOffset,
  const AudioFrameCount frames) {
  for (uint32_t c = 0; c < channels; ++c) {
    const float* s = src[c] + static_cast<size_t>(srcOffset) * srcStride;
    T* d = dst[c] + static_cast<size_t>(dstOffset) * dstStride;
    for (AudioFrameCount f = 0; f < frames; ++f) {
      *d = SampleTraits<T>::fromFloat(*s);
      s += srcStride;
      d += dstStride;
    }
  }
}

/**
 * Copies deinterleaved float samples into a 24 bit buffer, which has no typed
 * channel accessor and is therefore addressed through its raw bytes.
 */
void copyPlanarFloatToPacked24(const float* const* src,
  const uint32_t srcStride,
  AudioPCMBuffer& dst,
  const uint32_t channels,
  const AudioFrameCount srcOffset,
  const AudioFrameCount dstOffset,
  const AudioFrameCount frames) {
  const auto format = dst.getFormat();
  if (!format) {
    return;
  }
  const auto dstChannels = std::min(channels, format->getChannelCount());
  if (format->isInterleaved()) {
    const auto buffer = dst.mutableAudioBufferList()->getBuffer(0);
    if (!buffer || !buffer->mData) {
      return;
    }
    auto* out = static_cast<Packed24*>(buffer->mData);
    const auto stride = format->getChannelCount();
    for (AudioFrameCount f = 0; f < frames; ++f) {
      for (uint32_t c = 0; c < dstChannels; ++c) {
        out[static_cast<size_t>(dstOffset + f) * stride + c] =
          SampleTraits<Packed24>::fromFloat(src[c][static_cast<size_t>(srcOffset + f) * srcStride]);
      }
    }
    return;
  }

  for (uint32_t c = 0; c < dstChannels; ++c) {
    const auto buffer = dst.mutableAudioBufferList()->getBuffer(c);
    if (!buffer || !buffer->mData) {
      return;
    }
    auto* out = static_cast<Packed24*>(buffer->mData);
    for (AudioFrameCount f = 0; f < frames; ++f) {
      out[dstOffset + f] = SampleTraits<Packed24>::fromFloat(src[c][static_cast<size_t>(srcOffset + f) * srcStride]);
    }
  }
}

/**
 * Converts an interleaved buffer of any PCM format into deinterleaved float.
 * Returns false when the sample format is not a supported PCM format.
 */
bool interleavedToPlanarFloat(const void* src,
  const AudioFormat& srcFormat,
  float* const* dst,
  const uint32_t dstStride,
  const AudioFrameCount frames) {
  if (src == nullptr) {
    return false;
  }
  const auto channels = srcFormat.getChannelCount();
  switch (srcFormat.getSampleFormat()) {
    case AudioSampleFormat::kFloat:
      interleavedToPlanarFloat(static_cast<const float*>(src), channels, dst, dstStride, frames);
      return true;
    case AudioSampleFormat::kInt16:
      interleavedToPlanarFloat(static_cast<const int16_t*>(src), channels, dst, dstStride, frames);
      return true;
    case AudioSampleFormat::kInt24:
      interleavedToPlanarFloat(reinterpret_cast<const Packed24*>(src), channels, dst, dstStride, frames);
      return true;
    case AudioSampleFormat::kInt32:
      interleavedToPlanarFloat(static_cast<const int32_t*>(src), channels, dst, dstStride, frames);
      return true;
    default:
      return false;
  }
}

/**
 * Reads one sample of any PCM format out of raw bytes.
 */
float readRawSample(const uint8_t* data, const AudioSampleFormat format) {
  switch (format) {
    case AudioSampleFormat::kFloat: {
      float value = 0.0f;
      std::memcpy(&value, data, sizeof(float));
      return value;
    }
    case AudioSampleFormat::kInt16: {
      int16_t value = 0;
      std::memcpy(&value, data, sizeof(int16_t));
      return SampleTraits<int16_t>::toFloat(value);
    }
    case AudioSampleFormat::kInt24: {
      Packed24 value{};
      std::memcpy(&value, data, sizeof(Packed24));
      return SampleTraits<Packed24>::toFloat(value);
    }
    case AudioSampleFormat::kInt32: {
      int32_t value = 0;
      std::memcpy(&value, data, sizeof(int32_t));
      return SampleTraits<int32_t>::toFloat(value);
    }
    default:
      return 0.0f;
  }
}

/**
 * The number of bytes one sample of a PCM format takes.
 */
uint32_t bytesPerSampleOf(const AudioFormat& format) {
  return format.getBitsPerChannel() / 8;
}

/**
 * The number of frames a buffer of a list holds in a PCM format, as its byte size reports it.
 */
AudioFrameCount framesOf(const AudioBuffer* buffer, const uint32_t bytesPerSample) {
  if (buffer == nullptr || buffer->mNumberChannels == 0 || bytesPerSample == 0) {
    return 0;
  }
  return static_cast<AudioFrameCount>(buffer->mDataByteSize / (bytesPerSample * buffer->mNumberChannels));
}

/**
 * The number of float frames a buffer of a list holds, as its byte size reports it.
 *
 * This is how a list that a client supplied reports how much audio it really holds. A
 * graph buffer has its byte size kept in step with its frame length, so for those the
 * answer is the number of valid frames.
 */
AudioFrameCount floatFramesOf(const AudioBuffer* buffer) {
  return framesOf(buffer, sizeof(float));
}

/**
 * The smallest number of frames the buffers of a list report.
 */
AudioFrameCount floatFramesOf(const AudioBufferList& list) {
  AudioFrameCount frames = std::numeric_limits<AudioFrameCount>::max();
  for (size_t b = 0; b < list.numberOfBuffers(); ++b) {
    frames = std::min(frames, floatFramesOf(list.getBuffer(b)));
  }
  return frames == std::numeric_limits<AudioFrameCount>::max() ? 0 : frames;
}

/**
 * Converts interleaved samples of any PCM format into the float planes of a buffer list.
 */
template<typename T>
void interleavedIntoFloatPlanes(const T* src,
  const uint32_t channels,
  const uint32_t dstChannels,
  AudioBufferList& dst,
  const AudioFrameCount frames) {
  const auto mapped = std::min(channels, dstChannels);
  for (uint32_t c = 0; c < mapped; ++c) {
    auto* plane = static_cast<float*>(dst.getBuffer(c)->mData);
    for (AudioFrameCount f = 0; f < frames; ++f) {
      plane[f] = SampleTraits<T>::toFloat(src[static_cast<size_t>(f) * channels + c]);
    }
  }
}

/**
 * Converts one plane of any PCM format into the float planes of a buffer list.
 */
template<typename T>
void planarIntoFloatPlanes(const AudioBufferList& src,
  const uint32_t channels,
  AudioBufferList& dst,
  const AudioFrameCount frames) {
  for (uint32_t c = 0; c < channels; ++c) {
    const auto* plane = static_cast<const T*>(src.getBuffer(c)->mData);
    auto* out = static_cast<float*>(dst.getBuffer(c)->mData);
    for (AudioFrameCount f = 0; f < frames; ++f) {
      out[f] = SampleTraits<T>::toFloat(plane[f]);
    }
  }
}

} // namespace anonymous

AudioFrameCount AudioGraphUtils::copyPCMBuffer(const AudioPCMBuffer& src,
  const AudioFrameCount srcOffset,
  AudioPCMBuffer& dst,
  const AudioFrameCount dstOffset,
  const AudioFrameCount numFrames) {
  const auto srcFormat = src.getFormat();
  const auto dstFormat = dst.getFormat();
  if (!srcFormat || !dstFormat || !srcFormat->isValid() || !dstFormat->isValid()) {
    return 0;
  }
  // The graph renders float, so a source in any other format is a programming error.
  if (srcFormat->getSampleFormat() != AudioSampleFormat::kFloat) {
    return 0;
  }
  if (srcOffset >= src.getFrameLength() || dstOffset >= dst.getFrameCapacity()) {
    return 0;
  }

  const auto channels = std::min(srcFormat->getChannelCount(), dstFormat->getChannelCount());
  const auto frames = std::min({ numFrames, src.getFrameLength() - srcOffset, dst.getFrameCapacity() - dstOffset });
  if (channels == 0 || frames == 0) {
    return 0;
  }

  auto* const* srcChannels = src.floatChannelData();
  if (srcChannels == nullptr) {
    return 0;
  }
  const auto srcStride = src.getStride();

  switch (dstFormat->getSampleFormat()) {
    case AudioSampleFormat::kFloat: {
      auto* const* dstChannels = dst.floatChannelData();
      if (dstChannels == nullptr) {
        return 0;
      }
      copyPlanarFloatToChannels(srcChannels, srcStride, dstChannels, dst.getStride(), channels, srcOffset, dstOffset,
        frames);
      return frames;
    }
    case AudioSampleFormat::kInt16: {
      auto* const* dstChannels = dst.int16ChannelData();
      if (dstChannels == nullptr) {
        return 0;
      }
      copyPlanarFloatToChannels(srcChannels, srcStride, dstChannels, dst.getStride(), channels, srcOffset, dstOffset,
        frames);
      return frames;
    }
    case AudioSampleFormat::kInt32: {
      auto* const* dstChannels = dst.int32ChannelData();
      if (dstChannels == nullptr) {
        return 0;
      }
      copyPlanarFloatToChannels(srcChannels, srcStride, dstChannels, dst.getStride(), channels, srcOffset, dstOffset,
        frames);
      return frames;
    }
    case AudioSampleFormat::kInt24: {
      copyPlanarFloatToPacked24(srcChannels, srcStride, dst, channels, srcOffset, dstOffset, frames);
      return frames;
    }
    default:
      return 0;
  }
}

AudioFrameCount AudioGraphUtils::copySameFormatPCMBuffer(const AudioPCMBuffer& src,
  const AudioFrameCount srcOffset,
  AudioPCMBuffer& dst,
  const AudioFrameCount dstOffset,
  const AudioFrameCount numFrames) {
  const auto srcFormat = src.getFormat();
  const auto dstFormat = dst.getFormat();
  if (!srcFormat || !dstFormat || *srcFormat != *dstFormat) {
    return 0;
  }
  if (srcOffset >= src.getFrameLength() || dstOffset >= dst.getFrameCapacity()) {
    return 0;
  }

  const auto frames = std::min({ numFrames, src.getFrameLength() - srcOffset, dst.getFrameCapacity() - dstOffset });
  const auto bytesPerSample = bytesPerSampleOf(*srcFormat);
  if (frames == 0 || bytesPerSample == 0) {
    return 0;
  }

  const auto* srcList = src.audioBufferList();
  auto* dstList = dst.mutableAudioBufferList();
  if (srcFormat->isInterleaved()) {
    const auto srcBuffer = srcList->getBuffer(0);
    const auto dstBuffer = dstList->getBuffer(0);
    if (srcBuffer == nullptr || dstBuffer == nullptr || srcBuffer->mData == nullptr || dstBuffer->mData == nullptr) {
      return 0;
    }
    const auto bytesPerFrame = static_cast<size_t>(bytesPerSample) * srcFormat->getChannelCount();
    std::memcpy(static_cast<uint8_t*>(dstBuffer->mData) + static_cast<size_t>(dstOffset) * bytesPerFrame,
      static_cast<const uint8_t*>(srcBuffer->mData) + static_cast<size_t>(srcOffset) * bytesPerFrame,
      static_cast<size_t>(frames) * bytesPerFrame);
    return frames;
  }

  const auto channels = srcFormat->getChannelCount();
  for (uint32_t c = 0; c < channels; ++c) {
    const auto srcBuffer = srcList->getBuffer(c);
    const auto dstBuffer = dstList->getBuffer(c);
    if (srcBuffer == nullptr || dstBuffer == nullptr || srcBuffer->mData == nullptr || dstBuffer->mData == nullptr) {
      return 0;
    }
    std::memcpy(static_cast<uint8_t*>(dstBuffer->mData) + static_cast<size_t>(dstOffset) * bytesPerSample,
      static_cast<const uint8_t*>(srcBuffer->mData) + static_cast<size_t>(srcOffset) * bytesPerSample,
      static_cast<size_t>(frames) * bytesPerSample);
  }
  return frames;
}

AudioFrameCount AudioGraphUtils::copyBufferListIntoPCMBuffer(const AudioBufferList& src,
  const AudioFormat& format,
  AudioPCMBuffer& dst,
  const AudioFrameCount numFrames) {
  const auto dstFormat = dst.getFormat();
  if (!dstFormat.has_value() || *dstFormat != format || !format.isValid()) {
    return 0;
  }

  const auto channels = format.getChannelCount();
  const auto bytesPerSample = bytesPerSampleOf(format);
  if (channels == 0 || bytesPerSample == 0) {
    return 0;
  }

  const auto srcInterleaved = format.isInterleaved();
  const auto srcBytesPerFrame = srcInterleaved ? bytesPerSample * channels : bytesPerSample;
  auto frames = framesToConvert(numFrames, dst.getFrameCapacity());

  // The source list decides how much audio is really there.
  if (srcInterleaved) {
    const auto* buffer = src.getBuffer(0);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    frames = std::min(frames, static_cast<AudioFrameCount>(buffer->mDataByteSize / srcBytesPerFrame));
  } else {
    for (uint32_t c = 0; c < channels; ++c) {
      const auto* buffer = src.getBuffer(c);
      if (buffer == nullptr || buffer->mData == nullptr) {
        return 0;
      }
      frames = std::min(frames, static_cast<AudioFrameCount>(buffer->mDataByteSize / bytesPerSample));
    }
  }
  if (frames == 0) {
    return 0;
  }

  auto* dstList = dst.mutableAudioBufferList();
  const auto dstInterleaved = dstFormat->isInterleaved();
  if (srcInterleaved && dstInterleaved) {
    const auto* srcBuffer = src.getBuffer(0);
    const auto* dstBuffer = dstList->getBuffer(0);
    std::memcpy(dstBuffer->mData, srcBuffer->mData, static_cast<size_t>(frames) * srcBytesPerFrame);
    return frames;
  }
  if (srcInterleaved) {
    // Interleaved source, one destination plane per channel.
    const auto* srcBase = static_cast<const uint8_t*>(src.getBuffer(0)->mData);
    for (uint32_t c = 0; c < channels; ++c) {
      auto* dstChannel = static_cast<uint8_t*>(dstList->getBuffer(c)->mData);
      for (AudioFrameCount f = 0; f < frames; ++f) {
        std::memcpy(dstChannel + static_cast<size_t>(f) * bytesPerSample,
          srcBase + (static_cast<size_t>(f) * channels + c) * bytesPerSample, bytesPerSample);
      }
    }
    return frames;
  }
  if (dstInterleaved) {
    // One source plane per channel, interleaved destination.
    auto* dstBase = static_cast<uint8_t*>(dstList->getBuffer(0)->mData);
    for (uint32_t c = 0; c < channels; ++c) {
      const auto* srcChannel = static_cast<const uint8_t*>(src.getBuffer(c)->mData);
      for (AudioFrameCount f = 0; f < frames; ++f) {
        std::memcpy(dstBase + (static_cast<size_t>(f) * channels + c) * bytesPerSample,
          srcChannel + static_cast<size_t>(f) * bytesPerSample, bytesPerSample);
      }
    }
    return frames;
  }
  for (uint32_t c = 0; c < channels; ++c) {
    std::memcpy(dstList->getBuffer(c)->mData, src.getBuffer(c)->mData,
      static_cast<size_t>(frames) * bytesPerSample);
  }
  return frames;
}

AudioFrameCount
AudioGraphUtils::toInterleavedFloat(const AudioPCMBuffer& src, float* dst, const AudioFrameCount numFrames) {
  const auto format = src.getFormat();
  if (dst == nullptr || !format.has_value() || !format->isValid()) {
    return 0;
  }

  const auto frames = framesToConvert(numFrames, src.getFrameLength());
  const auto channels = format->getChannelCount();
  const auto bytesPerSample = bytesPerSampleOf(format.value());
  if (frames == 0 || channels == 0 || bytesPerSample == 0) {
    return 0;
  }

  const auto* list = src.audioBufferList();
  const auto sampleFormat = format->getSampleFormat();
  if (format->isInterleaved()) {
    const auto buffer = list->getBuffer(0);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    const auto* base = static_cast<const uint8_t*>(buffer->mData);
    for (AudioFrameCount f = 0; f < frames; ++f) {
      for (uint32_t c = 0; c < channels; ++c) {
        dst[static_cast<size_t>(f) * channels + c] =
          readRawSample(base + (static_cast<size_t>(f) * channels + c) * bytesPerSample, sampleFormat);
      }
    }
    return frames;
  }

  for (uint32_t c = 0; c < channels; ++c) {
    const auto buffer = list->getBuffer(c);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    const auto* base = static_cast<const uint8_t*>(buffer->mData);
    for (AudioFrameCount f = 0; f < frames; ++f) {
      dst[static_cast<size_t>(f) * channels + c] =
        readRawSample(base + static_cast<size_t>(f) * bytesPerSample, sampleFormat);
    }
  }
  return frames;
}

AudioFrameCount
AudioGraphUtils::interleavedFloatIntoBufferList(const float* src, AudioBufferList& dst, const AudioFrameCount numFrames) {
  const auto channels = static_cast<uint32_t>(dst.numberOfBuffers());
  if (src == nullptr || channels == 0 || numFrames == 0) {
    return 0;
  }

  for (uint32_t c = 0; c < channels; ++c) {
    const auto buffer = dst.getBuffer(c);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    auto* plane = static_cast<float*>(buffer->mData);
    for (AudioFrameCount f = 0; f < numFrames; ++f) {
      plane[f] = src[static_cast<size_t>(f) * channels + c];
    }
  }
  return numFrames;
}

AudioFrameCount AudioGraphUtils::copyFloatBufferList(const AudioBufferList& src,
  AudioBufferList& dst,
  const AudioFrameCount numFrames) {
  const auto dstBuffers = dst.numberOfBuffers();
  // Both sides bound the copy: a source that holds fewer frames than the block asks for
  // copies what it has, and a destination that is smaller than the source is filled up to
  // what it can hold instead of being overrun.
  const auto frames = std::min({ numFrames, floatFramesOf(src), floatFramesOf(dst) });
  if (frames == 0 || dstBuffers == 0) {
    return 0;
  }

  // The destination is the layout of the graph, one float plane per channel. A source with
  // the same number of buffers is that layout too, whatever its byte sizes say about its
  // channels, so those planes are moved as they are.
  if (src.numberOfBuffers() == dstBuffers) {
    for (size_t c = 0; c < dstBuffers; ++c) {
      const auto* srcBuffer = src.getBuffer(c);
      const auto* dstBuffer = dst.getBuffer(c);
      if (srcBuffer == nullptr || dstBuffer == nullptr ||
          srcBuffer->mData == nullptr || dstBuffer->mData == nullptr ||
          srcBuffer->mNumberChannels != dstBuffer->mNumberChannels) {
        return 0;
      }
      std::memcpy(dstBuffer->mData, srcBuffer->mData,
        static_cast<size_t>(frames) * sizeof(float) * srcBuffer->mNumberChannels);
    }
    return frames;
  }

  // Otherwise the source is interleaved and fills one destination plane per channel.
  const auto* srcBuffer = src.getBuffer(0);
  if (src.numberOfBuffers() != 1 || srcBuffer == nullptr || srcBuffer->mData == nullptr ||
      srcBuffer->mNumberChannels == 0) {
    return 0;
  }
  const auto* base = static_cast<const float*>(srcBuffer->mData);
  const auto channels = srcBuffer->mNumberChannels;
  const auto mapped = std::min<size_t>(channels, dstBuffers);
  for (size_t c = 0; c < mapped; ++c) {
    const auto* dstBuffer = dst.getBuffer(c);
    if (dstBuffer == nullptr || dstBuffer->mData == nullptr) {
      return 0;
    }
    auto* plane = static_cast<float*>(dstBuffer->mData);
    for (AudioFrameCount f = 0; f < frames; ++f) {
      plane[f] = base[static_cast<size_t>(f) * channels + c];
    }
  }
  return frames;
}

AudioFrameCount AudioGraphUtils::interleaveBufferListToFloat(const AudioBufferList& src,
  float* dst,
  const AudioFrameCount numFrames) {
  const auto channels = static_cast<uint32_t>(src.numberOfBuffers());
  if (dst == nullptr || channels == 0) {
    return 0;
  }

  const auto frames = std::min(numFrames, floatFramesOf(src));
  if (frames == 0) {
    return 0;
  }
  for (uint32_t c = 0; c < channels; ++c) {
    const auto buffer = src.getBuffer(c);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    const auto* plane = static_cast<const float*>(buffer->mData);
    for (AudioFrameCount f = 0; f < frames; ++f) {
      dst[static_cast<size_t>(f) * channels + c] = plane[f];
    }
  }
  return frames;
}

AudioFrameCount AudioGraphUtils::convertFromPCMBuffer(const AudioPCMBuffer& src,
  void* dst,
  const AudioFormat& dstFormat,
  const AudioFrameCount numFrames) {
  const auto srcFormat = src.getFormat();
  if (dst == nullptr || !srcFormat || !srcFormat->isValid() || !dstFormat.isValid()) {
    return 0;
  }
  if (srcFormat->getSampleFormat() != AudioSampleFormat::kFloat) {
    return 0;
  }
  auto* const* srcChannels = src.floatChannelData();
  if (srcChannels == nullptr) {
    return 0;
  }

  const auto frames = framesToConvert(numFrames, src.getFrameLength());
  const auto channels = std::min(srcFormat->getChannelCount(), dstFormat.getChannelCount());
  if (channels == 0 || frames == 0) {
    return 0;
  }
  const auto stride = src.getStride();

  switch (dstFormat.getSampleFormat()) {
    case AudioSampleFormat::kFloat:
      planarFloatToInterleaved(srcChannels, stride, static_cast<float*>(dst), channels, frames);
      return frames;
    case AudioSampleFormat::kInt16:
      planarFloatToInterleaved(srcChannels, stride, static_cast<int16_t*>(dst), channels, frames);
      return frames;
    case AudioSampleFormat::kInt24:
      planarFloatToInterleaved(srcChannels, stride, static_cast<Packed24*>(dst), channels, frames);
      return frames;
    case AudioSampleFormat::kInt32:
      planarFloatToInterleaved(srcChannels, stride, static_cast<int32_t*>(dst), channels, frames);
      return frames;
    default:
      return 0;
  }
}

AudioFrameCount AudioGraphUtils::convertIntoPCMBuffer(const void* src,
  const AudioFormat& srcFormat,
  AudioPCMBuffer& dst,
  const AudioFrameCount numFrames) {
  const auto dstFormat = dst.getFormat();
  if (!dstFormat || !dstFormat->isValid() || !srcFormat.isValid()) {
    return 0;
  }
  if (dstFormat->getSampleFormat() != AudioSampleFormat::kFloat) {
    return 0;
  }
  auto* const* dstChannels = dst.floatChannelData();
  if (dstChannels == nullptr) {
    return 0;
  }

  const auto frames = framesToConvert(numFrames, dst.getFrameCapacity());
  if (frames == 0) {
    return 0;
  }
  if (!interleavedToPlanarFloat(src, srcFormat, dstChannels, dst.getStride(), frames)) {
    return 0;
  }
  return frames;
}

AudioFrameCount AudioGraphUtils::convertBufferListIntoFloatBufferList(const AudioBufferList& src,
  const AudioFormat& srcFormat,
  AudioBufferList& dst,
  const AudioFrameCount numFrames) {
  if (!srcFormat.isValid() || dst.numberOfBuffers() == 0) {
    return 0;
  }
  const auto bytesPerSample = bytesPerSampleOf(srcFormat);
  const auto srcChannels = srcFormat.getChannelCount();
  if (bytesPerSample == 0 || srcChannels == 0) {
    return 0;
  }

  // A float source needs no conversion, only a possible layout change.
  if (srcFormat.getSampleFormat() == AudioSampleFormat::kFloat) {
    return copyFloatBufferList(src, dst, numFrames);
  }

  const auto dstChannels = static_cast<uint32_t>(dst.numberOfBuffers());
  if (srcFormat.isInterleaved()) {
    const auto* buffer = src.getBuffer(0);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    const auto frames = std::min(numFrames, framesOf(buffer, bytesPerSample));
    if (frames == 0) {
      return 0;
    }
    for (uint32_t c = 0; c < std::min(srcChannels, dstChannels); ++c) {
      const auto* out = dst.getBuffer(c);
      if (out == nullptr || out->mData == nullptr) {
        return 0;
      }
    }
    switch (srcFormat.getSampleFormat()) {
      case AudioSampleFormat::kInt16:
        interleavedIntoFloatPlanes(static_cast<const int16_t*>(buffer->mData), srcChannels, dstChannels, dst, frames);
        break;
      case AudioSampleFormat::kInt24:
        interleavedIntoFloatPlanes(reinterpret_cast<const Packed24*>(buffer->mData), srcChannels, dstChannels, dst,
          frames);
        break;
      case AudioSampleFormat::kInt32:
        interleavedIntoFloatPlanes(static_cast<const int32_t*>(buffer->mData), srcChannels, dstChannels, dst, frames);
        break;
      default:
        return 0;
    }
    return frames;
  }

  // A deinterleaved source has one buffer per channel.
  const auto channels = std::min(srcChannels, dstChannels);
  auto frames = numFrames;
  for (uint32_t c = 0; c < channels; ++c) {
    const auto* buffer = src.getBuffer(c);
    const auto* out = dst.getBuffer(c);
    if (buffer == nullptr || buffer->mData == nullptr || out == nullptr || out->mData == nullptr) {
      return 0;
    }
    frames = std::min(frames, framesOf(buffer, bytesPerSample));
  }
  if (frames == 0) {
    return 0;
  }
  switch (srcFormat.getSampleFormat()) {
    case AudioSampleFormat::kInt16:
      planarIntoFloatPlanes<int16_t>(src, channels, dst, frames);
      break;
    case AudioSampleFormat::kInt24:
      planarIntoFloatPlanes<Packed24>(src, channels, dst, frames);
      break;
    case AudioSampleFormat::kInt32:
      planarIntoFloatPlanes<int32_t>(src, channels, dst, frames);
      break;
    default:
      return 0;
  }
  return frames;
}

void AudioGraphUtils::silenceFloatBufferList(AudioBufferList& buffer, const AudioFrameCount numFrames) {
  if (numFrames == 0) {
    return;
  }
  for (size_t b = 0; b < buffer.numberOfBuffers(); ++b) {
    const auto audioBuffer = buffer.getBuffer(b);
    if (audioBuffer != nullptr && audioBuffer->mData != nullptr) {
      std::memset(audioBuffer->mData, 0,
        static_cast<size_t>(numFrames) * audioBuffer->mNumberChannels * sizeof(float));
    }
  }
}

void AudioGraphUtils::silenceBuffer(void* dst, const AudioFormat& format, const AudioFrameCount numFrames) {
  if (dst == nullptr || !format.isValid()) {
    return;
  }
  const auto bytesPerSample = format.getBitsPerChannel() / 8;
  if (bytesPerSample == 0) {
    return;
  }
  const auto bytes = static_cast<size_t>(numFrames) * format.getChannelCount() * bytesPerSample;
  std::memset(dst, 0, bytes);
}

void AudioGraphUtils::silencePCMBuffer(AudioPCMBuffer& buffer, const AudioFrameCount numFrames) {
  silencePCMBuffer(buffer, 0, numFrames);
}

void AudioGraphUtils::silencePCMBuffer(AudioPCMBuffer& buffer, const AudioFrameCount startFrame,
    const AudioFrameCount numFrames) {
  const auto format = buffer.getFormat();
  if (!format.has_value() || !format->isValid()) {
    return;
  }
  const auto capacity = buffer.getFrameCapacity();
  if (startFrame >= capacity) {
    return;
  }
  const auto frames = framesToConvert(numFrames, capacity - startFrame);
  if (frames == 0) {
    return;
  }

  const auto bytesPerSample = format->getBitsPerChannel() / 8;
  const auto bytesPerFrame = static_cast<size_t>(bytesPerSample) * format->getChannelCount();
  auto* list = buffer.mutableAudioBufferList();
  if (format->isInterleaved()) {
    const auto audioBuffer = list->getBuffer(0);
    if (audioBuffer != nullptr && audioBuffer->mData != nullptr) {
      auto* const start = static_cast<uint8_t*>(audioBuffer->mData) +
          static_cast<size_t>(startFrame) * bytesPerFrame;
      std::memset(start, 0, frames * bytesPerFrame);
    }
    return;
  }
  for (size_t channel = 0; channel < list->numberOfBuffers(); ++channel) {
    const auto audioBuffer = list->getBuffer(channel);
    if (audioBuffer != nullptr && audioBuffer->mData != nullptr) {
      auto* const start = static_cast<uint8_t*>(audioBuffer->mData) +
          static_cast<size_t>(startFrame) * bytesPerSample;
      std::memset(start, 0, static_cast<size_t>(frames) * bytesPerSample);
    }
  }
}

std::optional<AudioFormat> AudioGraphUtils::standardFormat(const AudioFormat& format) {
  if (!format.isValid()) {
    return std::nullopt;
  }
  if (const auto standard = AudioFormat::InitStandardFormatWithSampleRate(format.getSampleRate(),
        format.getChannelCount());
    standard.has_value()) {
    return standard;
  }
  return AudioFormat::InitWithCommonFormat(AudioSampleFormat::kFloat, format.getSampleRate(), format.getChannelCount(),
    false);
}

AudioFormat AudioGraphUtils::interleavedFormatForStream(const AudioStream& stream) {
  auto sampleFormat = stream.getFormat();
  if (sampleFormat == AudioSampleFormat::kDefault || sampleFormat == AudioSampleFormat::kInvalid) {
    sampleFormat = AudioSampleFormat::kFloat;
  }

  const auto defaultFormat = AudioFormat::DefaultFormat();
  const auto sampleRate = stream.getSampleRate() > 0
                            ? static_cast<Float64>(stream.getSampleRate())
                            : defaultFormat.getSampleRate();
  const auto channels = stream.getChannelCount() > 0 ? stream.getChannelCount() : defaultFormat.getChannelCount();

  // A stream's data callback delivers the frames in the device layout, which is
  // interleaved for every platform implementation in this module.
  if (const auto format = AudioFormat::InitWithCommonFormat(sampleFormat, sampleRate, channels, true);
    format.has_value()) {
    return format.value();
  }
  return defaultFormat;
}

} // namespace coral
