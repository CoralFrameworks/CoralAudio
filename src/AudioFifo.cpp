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

#include <coral/audio/AudioFifo.h>

#include <media/Utilities.h>

namespace coral {

std::unique_ptr<AudioFifo>
AudioFifo::Create(const AudioSampleFormat sampleFormat,
  const AudioChannelCount channels,
  const AudioFrameCount capacityInFrames) {
  if (sampleFormat == AudioSampleFormat::kDefault || sampleFormat == AudioSampleFormat::kInvalid) {
    return nullptr;
  }
  if (channels == 0) {
    return nullptr;
  }
  return std::unique_ptr<AudioFifo>(new AudioFifo(sampleFormat, channels, capacityInFrames));
}

AudioFifo::AudioFifo(const AudioSampleFormat sampleFormat,
  const AudioChannelCount channels,
  const AudioFrameCount capacityInFrames) noexcept
  : mSampleFormat(sampleFormat) {
  // Create() rejects a zero channel count, so this cannot fail in practice; the
  // return value is ignored rather than leaving an unusable FIFO half-built.
  (void) mRingBuffer.init(capacityInFrames, channels);
}

AudioFifo::~AudioFifo() {
  mRingBuffer.reset();
}

OpResult<AudioFrameCount>
AudioFifo::write(const void* data, const AudioFrameCount numSamples) {
  auto result = false;
  if (mRingBuffer.space() < numSamples) {
    return Result::ErrorNoMemory;
  }
  switch (mSampleFormat) {
    case AudioSampleFormat::kFloat: {
      result = mRingBuffer.write(static_cast<const float*>(data), numSamples);
      break;
    }
    case AudioSampleFormat::kInt16: {
      result = mRingBuffer.writeInt16(static_cast<const int16_t*>(data), numSamples);
      break;
    }
    case AudioSampleFormat::kInt24: {
      result = mRingBuffer.writeInt24(static_cast<const uint8_t*>(data), numSamples);
      break;
    }
    case AudioSampleFormat::kInt32: {
      result = mRingBuffer.writeInt32(static_cast<const int32_t*>(data), numSamples);
      break;
    }
    default: {
      return Result::ErrorInvalidFormat;
    }
  }
  if (!result) {
    return Result::ErrorNoMemory;
  }
  return OpResult(numSamples);
}

OpResult<AudioFrameCount> AudioFifo::read(void* data, const AudioFrameCount numSamples) {
  AudioFrameCount framesRead = 0;
  if (size() == 0) {
    return Result::ErrorNoMemory;
  }
  switch (mSampleFormat) {
    case AudioSampleFormat::kFloat: {
      framesRead = mRingBuffer.read(static_cast<float*>(data), numSamples);
      break;
    }
    case AudioSampleFormat::kInt16: {
      framesRead = mRingBuffer.readInt16(static_cast<int16_t*>(data), numSamples);
      break;
    }
    case AudioSampleFormat::kInt24: {
      framesRead = mRingBuffer.readInt24(static_cast<uint8_t*>(data), numSamples);
      break;
    }
    case AudioSampleFormat::kInt32: {
      framesRead = mRingBuffer.readInt32(static_cast<int32_t*>(data), numSamples);
      break;
    }
    default: {
      return Result::ErrorInvalidFormat;
    }
  }
  return OpResult(framesRead);
}

Result AudioFifo::drain(const AudioFrameCount numSamples) {
  mRingBuffer.discard(numSamples);
  return Result::OK;
}

void AudioFifo::reset() {
  mRingBuffer.reset();
}

AudioFrameCount AudioFifo::size() const {
  return mRingBuffer.available();
}

AudioFrameCount AudioFifo::space() const {
  return mRingBuffer.space();
}

} // namespace coral
