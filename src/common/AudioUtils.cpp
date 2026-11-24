/*
 * Copyright (C) 2024 The Coral Project
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

#include <coral/audio/AudioUtils.h>

#include <coral/audio/AudioFormat.h>

#include <algorithm>
#include <chrono>
#include <cmath> // std::isnan
#include <cstring>
#include <thread>

namespace coral {

AudioFrameCount AudioUtils::copyFromPCMBuffer(void* outputBuffer,
  const AudioFrameCount numFrames,
  const std::unique_ptr<AudioPCMBuffer>& pcmBuffer) {
  if (outputBuffer == nullptr || pcmBuffer == nullptr) {
    return 0;
  }

  const auto& format = pcmBuffer->getFormat();
  if (!format || !format->isValid()) {
    return 0;
  }

  const AudioFrameCount srcFrames = pcmBuffer->getFrameLength();
  const AudioFrameCount framesToCopy = std::min(numFrames, srcFrames);
  if (framesToCopy == 0) {
    return 0;
  }

  const AudioBufferList* list = pcmBuffer->audioBufferList();
  if (list == nullptr || list->numberOfBuffers() == 0) {
    return 0;
  }

  const uint32_t channelCount = format->getChannelCount();
  const uint32_t bytesPerSample = format->getBitsPerChannel() / 8;
  if (bytesPerSample == 0) {
    return 0;
  }

  if (format->isInterleaved()) {
    const AudioBuffer* b0 = list->getBuffer(0);
    if (b0 == nullptr || b0->mData == nullptr) {
      return 0;
    }
    const size_t bytesToCopy = static_cast<size_t>(framesToCopy) * format->getBytesPerFrame();
    std::memcpy(outputBuffer, b0->mData, bytesToCopy);
  } else {
    // The output side is interleaved and the PCM buffer is planar: each plane is spread
    // across the frames of the output buffer, one sample of every channel after the other.
    auto* dst = static_cast<uint8_t*>(outputBuffer);
    const size_t bytesPerFrame = static_cast<size_t>(bytesPerSample) * channelCount;

    for (uint32_t ch = 0; ch < channelCount; ++ch) {
      const AudioBuffer* b = list->getBuffer(ch);
      if (b == nullptr || b->mData == nullptr) {
        return ch == 0 ? 0 : framesToCopy;
      }
      const auto* src = static_cast<const uint8_t*>(b->mData);
      for (AudioFrameCount f = 0; f < framesToCopy; ++f) {
        std::memcpy(dst + static_cast<size_t>(f) * bytesPerFrame + static_cast<size_t>(ch) * bytesPerSample,
          src + static_cast<size_t>(f) * bytesPerSample, bytesPerSample);
      }
    }
  }

  return framesToCopy;
}

AudioFrameCount AudioUtils::copyIntoPCMBuffer(void* inputBuffer,
  const AudioFrameCount numFrames,
  std::unique_ptr<AudioPCMBuffer>& pcmBuffer) {
  if (inputBuffer == nullptr || pcmBuffer == nullptr || numFrames == 0) {
    return 0;
  }

  const auto& format = pcmBuffer->getFormat();
  if (!format || !format->isValid()) {
    return 0;
  }

  const auto capacity = pcmBuffer->getFrameCapacity();
  const auto framesToCopy = std::min(numFrames, capacity);
  if (framesToCopy == 0) {
    return 0;
  }

  AudioBufferList* list = pcmBuffer->mutableAudioBufferList();
  if (list == nullptr || list->numberOfBuffers() == 0) {
    return 0;
  }

  const uint32_t channelCount = format->getChannelCount();
  const uint32_t bytesPerSample = format->getBitsPerChannel() / 8;
  if (bytesPerSample == 0) {
    return 0;
  }

  if (format->isInterleaved()) {
    const auto buffer = list->getBuffer(0);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    const size_t bytesToCopy = static_cast<size_t>(framesToCopy) * format->getBytesPerFrame();
    std::memcpy(buffer->mData, inputBuffer, bytesToCopy);
  } else {
    // The input is interleaved and the PCM buffer is planar: every channel gathers its
    // samples out of the interleaved stream into its own plane.
    const auto* src = static_cast<const uint8_t*>(inputBuffer);
    const size_t bytesPerFrame = static_cast<size_t>(bytesPerSample) * channelCount;

    for (auto channel = 0; channel < channelCount; ++channel) {
      const auto buffer = list->getBuffer(channel);
      if (buffer == nullptr || buffer->mData == nullptr) {
        pcmBuffer->setFrameLength(0);
        return 0;
      }
      auto* dst = static_cast<uint8_t*>(buffer->mData);
      const auto* sample = src + static_cast<size_t>(channel) * bytesPerSample;
      for (AudioFrameCount f = 0; f < framesToCopy; ++f) {
        std::memcpy(dst + static_cast<size_t>(f) * bytesPerSample,
          sample + static_cast<size_t>(f) * bytesPerFrame, bytesPerSample);
      }
    }
  }
  pcmBuffer->setFrameLength(framesToCopy);
  return framesToCopy;
}

namespace {

template<typename T>
struct SampleScaler;

template<>
struct SampleScaler<int16_t> {
  static int16_t scale(int16_t s, float volume) noexcept {
    float value = static_cast<float>(s) * volume;
    if (value > 32767.0f) {
      value = 32767.0f;
    }
    if (value < -32768.0f) {
      value = -32768.0f;
    }
    return static_cast<int16_t>(value);
  }
};

template<>
struct SampleScaler<float> {
  static float scale(float s, float volume) noexcept {
    float value = s * volume;
    if (value > 1.0f) {
      value = 1.0f;
    }
    if (value < -1.0f) {
      value = -1.0f;
    }
    return value;
  }
};

struct Packed24 {
  uint8_t b[3];
};

static_assert(sizeof(Packed24) == 3, "Packed24 must be exactly 3 bytes");

int32_t readPacked24(const Packed24& s) noexcept {
#if defined(CORAL_CPU_LENDIAN)
  const uint32_t u = (static_cast<uint32_t>(s.b[0]) << 8) |
                     (static_cast<uint32_t>(s.b[1]) << 16) |
                     (static_cast<uint32_t>(s.b[2]) << 24);
#else
  const uint32_t u = (static_cast<uint32_t>(s.b[2]) << 8) |
                     (static_cast<uint32_t>(s.b[1]) << 16) |
                     (static_cast<uint32_t>(s.b[0]) << 24);
#endif
  return static_cast<int32_t>(u) >> 8;
}

void writePacked24(Packed24& s, const int32_t value) noexcept {
#if defined(CORAL_CPU_LENDIAN)
  s.b[0] = static_cast<uint8_t>(value & 0xFF);
  s.b[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  s.b[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
#else
  s.b[0] = static_cast<uint8_t>((value >> 16) & 0xFF);
  s.b[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  s.b[2] = static_cast<uint8_t>(value & 0xFF);
#endif
}

template<>
struct SampleScaler<Packed24> {
  static Packed24 scale(const Packed24 s, const float volume) noexcept {
    const int32_t value = readPacked24(s);

    double scaled = static_cast<double>(value) * static_cast<double>(volume);

    // clamp to 24-bit
    if (scaled > 8388607.0) {
      scaled = 8388607.0;
    }
    if (scaled < -8388608.0) {
      scaled = -8388608.0;
    }
    const auto out = static_cast<int32_t>(std::round(scaled));

    Packed24 result{};
    writePacked24(result, out);
    return result;
  }
};

template<>
struct SampleScaler<int32_t> {
  static int32_t scale(const int32_t s, const float volume) noexcept {
    double value = static_cast<double>(s) * static_cast<double>(volume);
    if (value > 2147483647.0) {
      value = 2147483647.0;
    }
    if (value < -2147483648.0) {
      value = -2147483648.0;
    }
    return static_cast<int32_t>(value);
  }
};

template<typename T>
bool applyVolume(const uint8_t* src,
  AudioBufferList* list,
  const uint32_t channels,
  const AudioFrameCount framesToCopy,
  const bool interleaved,
  float volume) noexcept {

  if (interleaved) {
    const auto buffer = list->getBuffer(0);
    if (!buffer || !buffer->mData) {
      return false;
    }

    const auto* s = reinterpret_cast<const T*>(src);
    auto* data = static_cast<T*>(buffer->mData);
    const size_t n = static_cast<size_t>(framesToCopy) * channels;

    for (size_t i = 0; i < n; ++i) {
      data[i] = SampleScaler<T>::scale(s[i], volume);
    }
  } else {
    // The source is interleaved, so the samples of one channel are one `channels` apart.
    const auto* base = reinterpret_cast<const T*>(src);

    for (auto channel = 0; channel < channels; ++channel) {
      const auto buffer = list->getBuffer(channel);
      if (!buffer || !buffer->mData) {
        return false;
      }

      auto* dstBuffer = static_cast<T*>(buffer->mData);

      for (size_t i = 0; i < framesToCopy; ++i) {
        dstBuffer[i] = SampleScaler<T>::scale(base[i * channels + channel], volume);
      }
    }
  }
  return true;
}

template<typename T>
void applyVolumeInPlace(AudioBufferList* list,
  const uint32_t channels,
  const AudioFrameCount frameLength,
  const bool interleaved,
  float volume) noexcept {

  if (interleaved) {
    const auto buffer = list->getBuffer(0);
    if (!buffer || !buffer->mData) {
      return;
    }

    auto dstBuffer = static_cast<T*>(buffer->mData);
    const auto n = static_cast<size_t>(frameLength) * channels;

    for (auto i = 0; i < n; ++i) {
      dstBuffer[i] = SampleScaler<T>::scale(dstBuffer[i], volume);
    }
  } else {
    for (auto channel = 0; channel < channels; ++channel) {
      const auto buffer = list->getBuffer(channel);
      if (!buffer || !buffer->mData) {
        return;
      }
      auto* dstBuffer = static_cast<T*>(buffer->mData);
      for (size_t i = 0; i < frameLength; ++i) {
        dstBuffer[i] = SampleScaler<T>::scale(dstBuffer[i], volume);
      }
    }
  }
}

void zeroInPlace(AudioBufferList* list,
  const uint32_t channels,
  const AudioFrameCount frameLength,
  const uint32_t bytesPerSample,
  const bool interleaved) noexcept {

  if (interleaved) {
    const auto buffer = list->getBuffer(0);
    if (!buffer || !buffer->mData) {
      return;
    }

    const auto bytes = static_cast<size_t>(frameLength) * channels * bytesPerSample;
    std::memset(buffer->mData, 0, bytes);
  } else {
    const auto bytesPerChannel = static_cast<size_t>(frameLength) * bytesPerSample;
    for (auto channel = 0; channel < channels; ++channel) {
      const auto buffer = list->getBuffer(channel);
      if (!buffer || !buffer->mData) return;
      std::memset(buffer->mData, 0, bytesPerChannel);
    }
  }
}

} // namespace anonymous

AudioFrameCount AudioUtils::copyIntoPCMBuffer(void* inputBuffer,
  const AudioFrameCount numFrames,
  std::unique_ptr<AudioPCMBuffer>& pcmBuffer,
  float volume) {

  if (inputBuffer == nullptr || pcmBuffer == nullptr || numFrames == 0) {
    return 0;
  }

  if (std::isnan(volume) || volume < 0.0f) {
    volume = 0.0f;
  }

  if (volume == 1.0f) {
    return copyIntoPCMBuffer(inputBuffer, numFrames, pcmBuffer);
  }

  const auto& format = pcmBuffer->getFormat();
  if (!format || !format->isValid()) {
    return 0;
  }

  const AudioFrameCount capacity = pcmBuffer->getFrameCapacity();
  const AudioFrameCount framesToCopy = std::min(numFrames, capacity);
  if (framesToCopy == 0) {
    return 0;
  }

  AudioBufferList* list = pcmBuffer->mutableAudioBufferList();
  if (list == nullptr || list->numberOfBuffers() == 0) {
    return 0;
  }

  const uint32_t channelCount = format->getChannelCount();
  const uint32_t bytesPerSample = format->getBitsPerChannel() / 8;
  if (bytesPerSample == 0) {
    return 0;
  }

  const auto* src = static_cast<const uint8_t*>(inputBuffer);
  const bool interleaved = format->isInterleaved();

  bool ok = false;
  switch (format->getSampleFormat()) {
    case AudioSampleFormat::kInt16:
      ok = applyVolume<int16_t>(src, list, channelCount,
        framesToCopy, interleaved, volume);
      break;
    case AudioSampleFormat::kFloat:
      ok = applyVolume<float>(src, list, channelCount,
        framesToCopy, interleaved, volume);
      break;
    case AudioSampleFormat::kInt24:
      ok = applyVolume<Packed24>(src, list, channelCount,
        framesToCopy, interleaved, volume);
      break;
    case AudioSampleFormat::kInt32:
      ok = applyVolume<int32_t>(src, list, channelCount,
        framesToCopy, interleaved, volume);
      break;
    default:
      return 0;
  }

  if (!ok) {
    pcmBuffer->setFrameLength(0);
    return 0;
  }

  pcmBuffer->setFrameLength(framesToCopy);
  return framesToCopy;
}

void AudioUtils::changeVolume(AudioPCMBuffer* pcmBuffer, float volume) {
  if (pcmBuffer == nullptr) {
    return;
  }
  const auto& format = pcmBuffer->getFormat();
  if (!format || !format->isValid()) {
    return;
  }
  const AudioFrameCount frameLength = pcmBuffer->getFrameLength();
  if (frameLength == 0) {
    return;
  }
  if (std::isnan(volume) || volume < 0.0f) {
    volume = 0.0f;
  }
  if (volume == 1.0f) {
    return;
  }

  auto list = pcmBuffer->mutableAudioBufferList();
  if (list == nullptr || list->numberOfBuffers() == 0) {
    return;
  }

  const uint32_t channelCount = format->getChannelCount();
  const uint32_t bytesPerSample = format->getBitsPerChannel() / 8;
  if (bytesPerSample == 0) {
    return;
  }

  const bool interleaved = format->isInterleaved();

  if (volume == 0.0f) {
    zeroInPlace(list, channelCount, frameLength, bytesPerSample, interleaved);
    return;
  }

  switch (format->getSampleFormat()) {
    case AudioSampleFormat::kInt16:
      applyVolumeInPlace<int16_t>(list, channelCount, frameLength, interleaved, volume);
      break;
    case AudioSampleFormat::kFloat:
      applyVolumeInPlace<float>(list, channelCount, frameLength, interleaved, volume);
      break;
    case AudioSampleFormat::kInt24:
      applyVolumeInPlace<Packed24>(list, channelCount, frameLength, interleaved, volume);
      break;
    case AudioSampleFormat::kInt32:
      applyVolumeInPlace<int32_t>(list, channelCount, frameLength, interleaved, volume);
      break;
    default: {
      break;
    }
  }
}

int32_t AudioUtils::clamp32FromFloat(float f) {
  static const auto scale = (float) (1UL << 31);
  static const float limpos = 1.;
  static const float limneg = -1.;

  if (f <= limneg) {
    return INT32_MIN;
  } else if (f >= limpos) {
    return INT32_MAX;
  }
  f *= scale;
  /* integer conversion is through truncation (though int to float is not).
   * ensure that we round to nearest, ties away from 0.
   */
  return (int32_t) (f > 0 ? f + 0.5 : f - 0.5);
}

void AudioUtils::convertFloatToPcm16(const float* source, int16_t* destination, int32_t numSamples) {
  for (int i = 0; i < numSamples; i++) {
    float fval = source[i];
    fval += 1.0; // to avoid discontinuity at 0.0 caused by truncation
    fval *= 32768.0f;
    auto sample = static_cast<int32_t>(fval);
    // clip to 16-bit range
    if (sample < 0) {
      sample = 0;
    } else if (sample > 0x0FFFF) {
      sample = 0x0FFFF;
    }
    sample -= 32768; // center at zero
    destination[i] = static_cast<int16_t>(sample);
  }
}

void AudioUtils::convertPcm16ToFloat(const int16_t* source, float* destination, int32_t numSamples) {
  for (int i = 0; i < numSamples; i++) {
    destination[i] = (float) source[i] * kScaleI16ToFloat;
  }
}

void AudioUtils::convertFloatToPcm24(const float* source, uint8_t* destination, int32_t numSamples) {
  while (numSamples--) {
    const int32_t ival = clamp24_from_float(*source++);
    // The packed samples are stored in the native byte order, the same one i32_from_p24()
    // reads, so the bytes are assembled the way the CPU stores a 24 bit sample.
#if defined(CORAL_CPU_LENDIAN)
    *destination++ = static_cast<uint8_t>(ival & 0xFF);
    *destination++ = static_cast<uint8_t>((ival >> 8) & 0xFF);
    *destination++ = static_cast<uint8_t>((ival >> 16) & 0xFF);
#else
    *destination++ = static_cast<uint8_t>((ival >> 16) & 0xFF);
    *destination++ = static_cast<uint8_t>((ival >> 8) & 0xFF);
    *destination++ = static_cast<uint8_t>(ival & 0xFF);
#endif
  }
}

void AudioUtils::convertPcm24ToFloat(const uint8_t* source, float* destination, int32_t numSamples) {
  while (numSamples--) {
    *destination++ = float_from_p24(source);
    source += 3;
  }
}

int64_t AudioUtils::getNanoseconds() {
  const auto now = std::chrono::steady_clock::now();
  const auto duration = now.time_since_epoch();
  const auto sec = std::chrono::duration_cast<std::chrono::seconds>(duration);
  const auto nanosecond = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - sec);
  return sec.count() * Definitions::kNanosPerSecond + nanosecond.count();
}

void AudioUtils::sleepForNanos(const int64_t nanoseconds) {
  const auto now = std::chrono::steady_clock::now();
  std::this_thread::sleep_until(now + std::chrono::nanoseconds(nanoseconds));
}

} // namespace coral
