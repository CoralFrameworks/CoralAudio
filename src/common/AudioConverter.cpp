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

#include <coral/audio/AudioConverter.h>

#include <coral/audio/AudioUtils.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace coral {

namespace {

constexpr const char* kConverterDomain = "AudioConverter";

Error* makeError(const ErrCode code, std::string reason) {
  return new Error(code, kConverterDomain, std::move(reason), std::string{});
}

bool isPCMSampleFormat(const AudioSampleFormat format) {
  switch (format) {
    case AudioSampleFormat::kFloat:
    case AudioSampleFormat::kInt16:
    case AudioSampleFormat::kInt24:
    case AudioSampleFormat::kInt32:
      return true;
    default:
      return false;
  }
}

/** One LSB of a sample format, expressed in the normalized float domain. */
float lsbOf(const AudioSampleFormat format) {
  switch (format) {
    case AudioSampleFormat::kInt16:
      return 1.0f / 32768.0f;
    case AudioSampleFormat::kInt24:
      return 1.0f / 8388608.0f;
    case AudioSampleFormat::kInt32:
      return 1.0f / 2147483648.0f;
    default:
      return 0.0f;
  }
}

/** Stores one normalized float sample in the requested PCM sample format. */
void storeSample(void* dst, const float value, const AudioSampleFormat format) {
  switch (format) {
    case AudioSampleFormat::kFloat:
      *static_cast<float*>(dst) = value;
      break;
    case AudioSampleFormat::kInt16: {
      const float clamped = std::clamp(value, -1.0f, 1.0f);
      *static_cast<int16_t*>(dst) = static_cast<int16_t>(std::lrint(clamped * 32767.0f));
      break;
    }
    case AudioSampleFormat::kInt32:
      *static_cast<int32_t*>(dst) = AudioUtils::clamp32FromFloat(value);
      break;
    case AudioSampleFormat::kInt24: {
      // Mirrors i32_from_p24(): the most significant bits live in the byte that
      // the reader shifts up by 24 on both endiannesses.
      const auto sample = static_cast<uint32_t>(clamp24_from_float(value));
      auto* bytes = static_cast<uint8_t*>(dst);
#if defined(CORAL_CPU_LENDIAN)
      bytes[0] = static_cast<uint8_t>((sample >> 8) & 0xFFu);
      bytes[1] = static_cast<uint8_t>((sample >> 16) & 0xFFu);
      bytes[2] = static_cast<uint8_t>((sample >> 24) & 0xFFu);
#else
      bytes[2] = static_cast<uint8_t>((sample >> 8) & 0xFFu);
      bytes[1] = static_cast<uint8_t>((sample >> 16) & 0xFFu);
      bytes[0] = static_cast<uint8_t>((sample >> 24) & 0xFFu);
#endif
      break;
    }
    default:
      break;
  }
}

/** Reads one PCM sample as a normalized float. */
float loadSample(const void* src, const AudioSampleFormat format) {
  switch (format) {
    case AudioSampleFormat::kFloat:
      return *static_cast<const float*>(src);
    case AudioSampleFormat::kInt16:
      return static_cast<float>(*static_cast<const int16_t*>(src)) * (1.0f / 32768.0f);
    case AudioSampleFormat::kInt32:
      return float_from_i32(*static_cast<const int32_t*>(src));
    case AudioSampleFormat::kInt24:
      return float_from_p24(static_cast<const uint8_t*>(src));
    default:
      return 0.0f;
  }
}

} // namespace anonymous

std::unique_ptr<AudioConverter>
AudioConverter::InitFromFormat(const AudioFormat& fromFormat, const AudioFormat& toFormat) {
  if (!fromFormat.isValid() || !toFormat.isValid()) {
    return nullptr;
  }
  if (!isPCMSampleFormat(fromFormat.getSampleFormat()) ||
      !isPCMSampleFormat(toFormat.getSampleFormat())) {
    // Compressed data needs a codec rather than a format converter.
    return nullptr;
  }
  if (fromFormat.getChannelCount() == 0 || toFormat.getChannelCount() == 0) {
    return nullptr;
  }

  // The resampler reduces the two rates to an integer ratio held in int32_t.
  const auto inputRate = fromFormat.getSampleRate();
  const auto outputRate = toFormat.getSampleRate();
  constexpr auto kMaxRate = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
  if (inputRate == 0 || outputRate == 0 || inputRate > kMaxRate || outputRate > kMaxRate) {
    return nullptr;
  }

  return std::unique_ptr<AudioConverter>(new AudioConverter(fromFormat, toFormat));
}

int AudioConverter::tapsForQuality(const AudioQuality quality) noexcept {
  switch (quality) {
    case AudioQuality::Fastest:
      return 2;
    case AudioQuality::Low:
      return 4;
    case AudioQuality::Medium:
      return 8;
    case AudioQuality::High:
      return 16;
    case AudioQuality::Best:
      return 32;
    default:
      return 16;
  }
}

void AudioConverter::configure() {
  mInChannels = mInputFormat.getChannelCount();
  mOutChannels = mOutputFormat.getChannelCount();
  mDitherSeed = 0x2545F491u;
  mPrimeInfo = getPrimeInfo();
}

AudioConverter::PrimeInfo AudioConverter::preparePrimeInfo() const {
  const auto taps = static_cast<AudioFrameCount>(tapsForQuality(mSampleRateConverterQuality));
  switch (mPrimeMethod) {
    case PrimeMethod::kPre:
      // A windowed-sinc filter reads both sides of the signal to prime.
      return PrimeInfo{ .leadingFrames = taps, .trailingFrames = taps };
    case PrimeMethod::kNormal:
      // Zero latency: only trailing input frames are required.
      return PrimeInfo{ .leadingFrames = 0, .trailingFrames = taps };
    case PrimeMethod::kNone:
    default:
      return PrimeInfo{ .leadingFrames = 0, .trailingFrames = 0 };
  }
}

MultiChannelResampler::Quality AudioConverter::qualityToResamplerQuality() const {
  switch (mSampleRateConverterQuality) {
    case AudioQuality::Fastest:
      return MultiChannelResampler::Quality::Fastest;
    case AudioQuality::Low:
      return MultiChannelResampler::Quality::Low;
    case AudioQuality::Medium:
      return MultiChannelResampler::Quality::Medium;
    case AudioQuality::High:
      return MultiChannelResampler::Quality::High;
    case AudioQuality::Best:
      return MultiChannelResampler::Quality::Best;
    default:
      return MultiChannelResampler::Quality::Medium;
  }
}

void AudioConverter::reset() {
  mResampler.reset();
  mResamplerRate = 0;
  mResamplerChannels = 0;
  mResamplerFrames = 0;
  mReachedEndOfStream = false;

  mPlanarInput.clear();
  mPlanarFrames = 0;
  mMapped.clear();
  mMappedFrames = 0;
  mResamplerInput.clear();
  mResampled.clear();
  mOutputFramePos = 0;
  mOutputFrameCapacity = 0;
  mDitherSeed = 0x2545F491u;
}

bool AudioConverter::ensureResampler(const int32_t channels) {
  const auto inputRate = static_cast<int32_t>(mInputFormat.getSampleRate());
  const auto outputRate = static_cast<int32_t>(mOutputFormat.getSampleRate());

  if (mResampler && mResamplerRate == inputRate && mResamplerChannels == channels) {
    return true;
  }
  if (inputRate <= 0 || outputRate <= 0 || channels <= 0) {
    return false;
  }

  auto* made = MultiChannelResampler::make(channels, inputRate, outputRate, qualityToResamplerQuality());
  if (!made) {
    return false;
  }

  mResampler.reset(made);
  mResamplerRate = inputRate;
  mResamplerChannels = channels;
  mResamplerFrames = 0;
  mReachedEndOfStream = false;
  return true;
}

bool AudioConverter::loadPlanarFloat(const AudioPCMBuffer& buffer,
  const AudioFormat& format,
  const AudioFrameCount frames,
  std::vector<float>& planar,
  AudioFrameCount& planarFrames) {
  const uint32_t channels = format.getChannelCount();
  planar.assign(static_cast<size_t>(channels) * frames, 0.0f);
  planarFrames = frames;

  const AudioBufferList* list = buffer.audioBufferList();
  if (!list || list->numberOfBuffers() == 0) {
    return false;
  }

  const auto sampleFormat = format.getSampleFormat();
  const uint32_t bytesPerSample = format.getBitsPerChannel() / 8;
  if (bytesPerSample == 0) {
    return false;
  }

  if (format.isInterleaved()) {
    const auto* buffer0 = list->getBuffer(0);
    if (!buffer0 || !buffer0->mData) {
      return false;
    }
    const uint32_t stride = buffer0->mNumberChannels == 0 ? channels : buffer0->mNumberChannels;
    const auto* base = static_cast<const uint8_t*>(buffer0->mData);
    for (uint32_t c = 0; c < channels; ++c) {
      float* dst = planar.data() + static_cast<size_t>(c) * frames;
      for (AudioFrameCount f = 0; f < frames; ++f) {
        dst[f] = loadSample(base + (static_cast<size_t>(f) * stride + c) * bytesPerSample, sampleFormat);
      }
    }
    return true;
  }

  const uint32_t available = std::min<uint32_t>(channels, static_cast<uint32_t>(list->numberOfBuffers()));
  for (uint32_t c = 0; c < available; ++c) {
    const auto* channel = list->getBuffer(c);
    if (!channel || !channel->mData) {
      return false;
    }
    float* dst = planar.data() + static_cast<size_t>(c) * frames;
    const auto* base = static_cast<const uint8_t*>(channel->mData);
    for (AudioFrameCount f = 0; f < frames; ++f) {
      dst[f] = loadSample(base + static_cast<size_t>(f) * bytesPerSample, sampleFormat);
    }
  }
  return true;
}

bool AudioConverter::prepareInput(const AudioPCMBuffer* inputBuffer) {
  if (!inputBuffer) {
    return false;
  }
  const AudioFrameCount frames = inputBuffer->getFrameLength();
  if (frames == 0) {
    mPlanarFrames = 0;
    return true;
  }
  return loadPlanarFloat(*inputBuffer, mInputFormat, frames, mPlanarInput, mPlanarFrames);
}

bool AudioConverter::loadInterleaved(const void* data,
  const AudioFrameCount frames,
  const uint32_t channels) {
  if (!data || channels == 0 || frames == 0) {
    return false;
  }
  const uint32_t bytesPerSample = mInputFormat.getBitsPerChannel() / 8;
  if (bytesPerSample == 0) {
    return false;
  }

  mPlanarInput.assign(static_cast<size_t>(channels) * frames, 0.0f);
  mPlanarFrames = frames;

  const auto sampleFormat = mInputFormat.getSampleFormat();
  const auto* base = static_cast<const uint8_t*>(data);
  for (AudioFrameCount f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < channels; ++c) {
      mPlanarInput[static_cast<size_t>(c) * frames + f] =
        loadSample(base + (static_cast<size_t>(f) * channels + c) * bytesPerSample, sampleFormat);
    }
  }
  return true;
}

// =============================================================
//  channels remap
// =============================================================
bool AudioConverter::remapChannels() {
  const AudioFrameCount frames = mPlanarFrames;
  const uint32_t inChannels = mInChannels;
  const uint32_t outChannels = mOutChannels;

  mMappedFrames = frames;
  if (frames == 0) {
    mMapped.clear();
    return true;
  }
  if (inChannels == 0 || outChannels == 0) {
    return false;
  }

  if (inChannels == outChannels) {
    mMapped = mPlanarInput;
    return true;
  }

  mMapped.assign(static_cast<size_t>(outChannels) * frames, 0.0f);

  if (mDownMix) {
    if (inChannels > outChannels) {
      // Fold every input channel into the output channels, spreading the inputs
      // evenly and scaling so the mix does not clip.
      const float scale = 1.0f / static_cast<float>(inChannels);
      for (uint32_t c = 0; c < outChannels; ++c) {
        float* dst = mMapped.data() + static_cast<size_t>(c) * frames;
        for (uint32_t src = c; src < inChannels; src += outChannels) {
          const float* s = mPlanarInput.data() + static_cast<size_t>(src) * frames;
          for (AudioFrameCount f = 0; f < frames; ++f) {
            dst[f] += s[f] * scale;
          }
        }
      }
    } else {
      // Upmix by repeating the input channels across the output layout.
      const float scale = static_cast<float>(outChannels) / static_cast<float>(inChannels);
      for (uint32_t c = 0; c < inChannels; ++c) {
        const float* s = mPlanarInput.data() + static_cast<size_t>(c) * frames;
        float* dst = mMapped.data() + static_cast<size_t>(c % outChannels) * frames;
        for (AudioFrameCount f = 0; f < frames; ++f) {
          dst[f] += s[f] * scale;
        }
      }
    }
    return true;
  }

  // Plain remap: line up what matches and fill the remaining outputs from the
  // closest input channel. A mono stream therefore lands on every output
  // channel instead of only on the first one.
  const uint32_t mapped = std::min(inChannels, outChannels);
  for (uint32_t c = 0; c < mapped; ++c) {
    std::memcpy(mMapped.data() + static_cast<size_t>(c) * frames,
      mPlanarInput.data() + static_cast<size_t>(c) * frames,
      static_cast<size_t>(frames) * sizeof(float));
  }
  if (outChannels > mapped) {
    const float* last = mPlanarInput.data() + static_cast<size_t>(inChannels - 1) * frames;
    for (uint32_t c = mapped; c < outChannels; ++c) {
      std::memcpy(mMapped.data() + static_cast<size_t>(c) * frames, last,
        static_cast<size_t>(frames) * sizeof(float));
    }
  }
  return true;
}

// =============================================================
//  sample rate conversion
// =============================================================
size_t AudioConverter::resampleMapped(const size_t maxOutputFrames) {
  if (!mResampler || mOutChannels == 0 || maxOutputFrames == 0) {
    return 0;
  }

  auto& resampler = *mResampler;
  const size_t inputFrames = mMappedFrames;

  // The resampler consumes and produces interleaved frames, while the graph
  // works on planes, so pack the mapped planes into an interleaved scratch.
  mResamplerInput.assign(std::max<size_t>(inputFrames, 1) * mOutChannels, 0.0f);
  for (uint32_t c = 0; c < mOutChannels; ++c) {
    const float* src = mMapped.data() + static_cast<size_t>(c) * inputFrames;
    for (size_t f = 0; f < inputFrames; ++f) {
      mResamplerInput[f * mOutChannels + c] = src[f];
    }
  }

  mResampled.assign(maxOutputFrames * mOutChannels, 0.0f);

  size_t inPos = 0;
  size_t produced = 0;
  while (produced < maxOutputFrames) {
    if (resampler.isWriteNeeded()) {
      if (inPos >= inputFrames) {
        // Out of input for this pass; the resampler keeps its phase so the next
        // call continues seamlessly.
        break;
      }
      resampler.writeNextFrame(mResamplerInput.data() + inPos * mOutChannels);
      ++inPos;
    } else {
      resampler.readNextFrame(mResampled.data() + produced * mOutChannels);
      ++produced;
    }
  }

  mResamplerFrames += produced;
  return produced;
}

// =============================================================
//  write sample to output
// =============================================================
float AudioConverter::ditherNoise() {
  if (!mDither) {
    return 0.0f;
  }
  // Triangular PDF dither: the difference of two independent uniform values.
  mDitherSeed = mDitherSeed * 1664525u + 1013904223u;
  const float a = static_cast<float>((mDitherSeed >> 8) & 0xFFFFu) * (1.0f / 65536.0f);
  mDitherSeed = mDitherSeed * 1664525u + 1013904223u;
  const float b = static_cast<float>((mDitherSeed >> 8) & 0xFFFFu) * (1.0f / 65536.0f);
  return (a - b) * lsbOf(mOutputFormat.getSampleFormat());
}

bool AudioConverter::writeSample(AudioBuffer& out, const size_t byteOffset, float value) {
  const uint32_t bytesPerSample = mOutputFormat.getBitsPerChannel() / 8;
  if (bytesPerSample == 0 || !out.mData) {
    return false;
  }
  if (out.mDataByteSize != 0 && byteOffset + bytesPerSample > out.mDataByteSize) {
    return false; // Would write past the end of the buffer.
  }

  const auto sampleFormat = mOutputFormat.getSampleFormat();
  if (mDither && sampleFormat != AudioSampleFormat::kFloat) {
    value += ditherNoise();
  }

  storeSample(static_cast<uint8_t*>(out.mData) + byteOffset, value, sampleFormat);
  return true;
}

bool AudioConverter::writeFrames(AudioBuffer& out,
  const AudioFrameCount outOffset,
  const AudioFrameCount frames,
  const float* planar,
  const size_t planarStride) {
  if (frames == 0) {
    return true;
  }
  if (!planar || mOutChannels == 0) {
    return false;
  }

  // A raw AudioBuffer carries every channel interleaved inside it.
  const size_t bytesPerSample = mOutputFormat.getBitsPerChannel() / 8;
  for (AudioFrameCount f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < mOutChannels; ++c) {
      const size_t offset =
        (static_cast<size_t>(outOffset + f) * mOutChannels + c) * bytesPerSample;
      if (!writeSample(out, offset, planar[static_cast<size_t>(c) * planarStride + f])) {
        return false;
      }
    }
  }
  return true;
}

bool AudioConverter::writePlanarFrames(AudioPCMBuffer& out,
  const AudioFrameCount outOffset,
  const AudioFrameCount frames,
  const float* planar,
  const size_t planarStride) {
  if (frames == 0) {
    return true;
  }
  if (!planar || mOutChannels == 0) {
    return false;
  }

  auto* list = out.mutableAudioBufferList();
  if (!list || list->numberOfBuffers() == 0) {
    return false;
  }

  if (mOutputFormat.isInterleaved()) {
    auto* buffer0 = list->getBuffer(0);
    if (!buffer0) {
      return false;
    }
    return writeFrames(*buffer0, outOffset, frames, planar, planarStride);
  }

  // Planar: each channel owns one AudioBuffer holding consecutive frames.
  const size_t bytesPerSample = mOutputFormat.getBitsPerChannel() / 8;
  for (uint32_t c = 0; c < mOutChannels; ++c) {
    auto* buffer = list->getBuffer(c);
    if (!buffer) {
      return false;
    }
    const float* src = planar + static_cast<size_t>(c) * planarStride;
    for (AudioFrameCount f = 0; f < frames; ++f) {
      const size_t offset = static_cast<size_t>(outOffset + f) * bytesPerSample;
      if (!writeSample(*buffer, offset, src[f])) {
        return false;
      }
    }
  }
  return true;
}

std::optional<Error>
AudioConverter::convertToBuffer(const std::unique_ptr<AudioPCMBuffer>& inputBuffer,
  std::unique_ptr<AudioPCMBuffer>& outputBuffer) {
  if (!inputBuffer) {
    return Error(ErrCode::kInvalidArgument, kConverterDomain, "convertToBuffer: input buffer is null", "");
  }
  if (!outputBuffer) {
    return Error(ErrCode::kInvalidArgument, kConverterDomain, "convertToBuffer: output buffer is null", "");
  }

  const auto inputExtra = inputBuffer->getFormat();
  if (!inputExtra || !inputExtra->isValid()) {
    return Error(ErrCode::kInvalidArgument, kConverterDomain,
      "convertToBuffer: input buffer has no valid format", "");
  }
  const auto outputExtra = outputBuffer->getFormat();
  if (!outputExtra || !outputExtra->isValid()) {
    return Error(ErrCode::kInvalidArgument, kConverterDomain,
      "convertToBuffer: output buffer has no valid format", "");
  }

  const AudioFormat& srcFormat = *inputExtra;
  const AudioFormat& dstFormat = *outputExtra;

  if (srcFormat.getChannelCount() != mInputFormat.getChannelCount() ||
      dstFormat.getChannelCount() != mOutputFormat.getChannelCount()) {
    return Error(ErrCode::kInvalidArgument, kConverterDomain,
      "convertToBuffer: buffer channel layout does not match the converter", "");
  }

  const AudioFrameCount inputFrames = inputBuffer->getFrameLength();
  if (inputFrames == 0) {
    outputBuffer->setFrameLength(0);
    return std::nullopt;
  }

  const bool needsResample = srcFormat.getSampleRate() != dstFormat.getSampleRate();
  if (!needsResample && outputBuffer->getFrameCapacity() < inputFrames) {
    outputBuffer->setFrameLength(0);
    return Error(ErrCode::kInvalidArgument, kConverterDomain,
      "convertToBuffer: output capacity is smaller than the input frame length", "");
  }

  // A PCM buffer reports the byte size of its current frame length, while this call is allowed
  // to fill it up to its capacity, so the output is opened up first and trimmed to what was
  // produced on the way out. Without this, converting twice into the same buffer fails as soon
  // as the second block is longer than the first one left behind.
  outputBuffer->setFrameLength(outputBuffer->getFrameCapacity());

  if (!prepareInput(inputBuffer.get()) || !remapChannels()) {
    outputBuffer->setFrameLength(0);
    return Error(ErrCode::kUnknown, kConverterDomain, "convertToBuffer: failed to read the input buffer", "");
  }

  AudioFrameCount produced = 0;
  if (!needsResample) {
    produced = std::min(mMappedFrames, outputBuffer->getFrameCapacity());
    if (!writePlanarFrames(*outputBuffer, 0, produced, mMapped.data(), mMappedFrames)) {
      outputBuffer->setFrameLength(0);
      return Error(ErrCode::kUnknown, kConverterDomain, "convertToBuffer: failed to write the output buffer", "");
    }
  } else {
    if (!ensureResampler(static_cast<int32_t>(dstFormat.getChannelCount()))) {
      outputBuffer->setFrameLength(0);
      return Error(ErrCode::kUnknown, kConverterDomain,
        "convertToBuffer: failed to create the sample rate converter", "");
    }
    const size_t wanted = outputBuffer->getFrameCapacity();
    produced = static_cast<AudioFrameCount>(resampleMapped(wanted));
    if (produced == 0) {
      outputBuffer->setFrameLength(0);
      return Error(ErrCode::kUnknown, kConverterDomain,
        "convertToBuffer: the sample rate converter produced no output", "");
    }
    // The resampler works on interleaved frames while the writer expects one plane per channel,
    // so the result is deinterleaved into the scratch the resampler has just released.
    mResamplerInput.assign(static_cast<size_t>(produced) * mOutChannels, 0.0f);
    for (uint32_t c = 0; c < mOutChannels; ++c) {
      for (AudioFrameCount f = 0; f < produced; ++f) {
        mResamplerInput[static_cast<size_t>(c) * produced + f] =
          mResampled[static_cast<size_t>(f) * mOutChannels + c];
      }
    }
    if (!writePlanarFrames(*outputBuffer, 0, produced, mResamplerInput.data(), produced)) {
      outputBuffer->setFrameLength(0);
      return Error(ErrCode::kUnknown, kConverterDomain, "convertToBuffer: failed to write the output buffer", "");
    }
  }

  outputBuffer->setFrameLength(produced);
  return std::nullopt;
}

AudioConverter::OutputStatus
AudioConverter::convertToBuffer(const std::unique_ptr<AudioBuffer>& outputBuffer,
  Error** outError,
  const InputBlock& inputBlock) {
  if (outError) {
    *outError = nullptr;
  }

  if (!outputBuffer) {
    if (outError) {
      *outError = makeError(ErrCode::kInvalidArgument, "convertToBuffer: output buffer is null");
    }
    return OutputStatus::Error;
  }
  if (!inputBlock) {
    if (outError) {
      *outError = makeError(ErrCode::kInvalidArgument, "convertToBuffer: input block is null");
    }
    return OutputStatus::Error;
  }
  if (!mInputFormat.isValid() || !mOutputFormat.isValid()) {
    if (outError) {
      *outError = makeError(ErrCode::kNotInit, "convertToBuffer: the converter has no usable format");
    }
    return OutputStatus::Error;
  }

  const uint32_t inputBytesPerFrame = mInputFormat.getBytesPerFrame();
  const uint32_t outputBytesPerFrame = mOutputFormat.getBytesPerFrame();
  if (inputBytesPerFrame == 0 || outputBytesPerFrame == 0) {
    if (outError) {
      *outError = makeError(ErrCode::kNotInit, "convertToBuffer: a format has zero bytes per frame");
    }
    return OutputStatus::Error;
  }

  // The output buffer is described by its byte size; derive the frame capacity.
  const AudioFrameCount capacity = outputBuffer->mDataByteSize / outputBytesPerFrame;
  outputBuffer->mDataByteSize = 0;
  if (capacity == 0) {
    if (outError) {
      *outError = makeError(ErrCode::kInvalidArgument, "convertToBuffer: output buffer has no capacity");
    }
    return OutputStatus::Error;
  }

  const bool needsResample = mInputFormat.getSampleRate() != mOutputFormat.getSampleRate();

  // A raw AudioBuffer is a single buffer whose samples are interleaved across
  // mNumberChannels, so every write goes through that one buffer.
  if (outputBuffer->mNumberChannels == 0) {
    outputBuffer->mNumberChannels = mOutChannels;
  }

  // ---- No sample rate conversion: the input block drives the loop. ----
  if (!needsResample) {
    // Ask for roughly a millisecond of audio at a time.
    const auto framesPerRequest = static_cast<AudioPacketCount>(
      std::max<uint64_t>(1, mInputFormat.getSampleRate() / 1000u));

    AudioFrameCount produced = 0;
    bool reachedEndOfStream = false;

    while (produced < capacity) {
      InputStatus status = InputStatus::HaveData;
      auto block = inputBlock(framesPerRequest, &status);

      if (!block || status != InputStatus::HaveData) {
        reachedEndOfStream = status == InputStatus::EndOfStream;
        break;
      }
      if (!block->mData || block->mDataByteSize == 0) {
        continue; // Nothing usable; ask again.
      }

      const auto framesAvailable = static_cast<AudioFrameCount>(block->mDataByteSize / inputBytesPerFrame);
      const AudioFrameCount wanted = std::min(framesAvailable, capacity - produced);
      if (wanted == 0) {
        break;
      }

      if (!loadInterleaved(block->mData, wanted, mInChannels) || !remapChannels()) {
        if (outError) {
          *outError = makeError(ErrCode::kUnknown, "convertToBuffer: failed to read the input block");
        }
        return OutputStatus::Error;
      }
      if (!writeFrames(*outputBuffer, produced, wanted, mMapped.data(), mMappedFrames)) {
        if (outError) {
          *outError = makeError(ErrCode::kUnknown, "convertToBuffer: failed to write the output buffer");
        }
        return OutputStatus::Error;
      }
      produced += wanted;
    }

    outputBuffer->mDataByteSize = produced * outputBytesPerFrame;
    if (reachedEndOfStream && produced == 0) {
      return OutputStatus::EndOfStream;
    }
    return produced == capacity ? OutputStatus::HaveData : OutputStatus::InputRanDry;
  }

  // ---- Sample rate conversion: write input on demand, read output to fill. ----
  if (!ensureResampler(static_cast<int32_t>(mOutChannels))) {
    if (outError) {
      *outError = makeError(ErrCode::kUnknown, "convertToBuffer: failed to create the sample rate converter");
    }
    return OutputStatus::Error;
  }

  auto& resampler = *mResampler;
  const auto framesPerRequest = static_cast<AudioPacketCount>(
    std::max<uint64_t>(1, mInputFormat.getSampleRate() / 1000u));

  // Feed the resampler from interleaved frames that come back from the resampler
  // itself, so keep separate scratch for its input and output.
  size_t inPos = 0;
  size_t inFrames = 0;
  AudioFrameCount produced = 0;
  bool reachedEndOfStream = false;

  while (produced < capacity) {
    if (resampler.isWriteNeeded()) {
      if (inPos >= inFrames) {
        InputStatus status = InputStatus::HaveData;
        auto block = inputBlock(framesPerRequest, &status);
        if (!block || status != InputStatus::HaveData || !block->mData || block->mDataByteSize == 0) {
          reachedEndOfStream = status == InputStatus::EndOfStream;
          break;
        }

        const auto framesAvailable = static_cast<AudioFrameCount>(block->mDataByteSize / inputBytesPerFrame);
        if (framesAvailable == 0) {
          break;
        }
        if (!loadInterleaved(block->mData, framesAvailable, mInChannels) || !remapChannels()) {
          if (outError) {
            *outError = makeError(ErrCode::kUnknown, "convertToBuffer: failed to read the input block");
          }
          return OutputStatus::Error;
        }

        // Pack the mapped planes into the interleaved layout the resampler wants.
        mResamplerInput.assign(static_cast<size_t>(mMappedFrames) * mOutChannels, 0.0f);
        for (uint32_t c = 0; c < mOutChannels; ++c) {
          const float* src = mMapped.data() + static_cast<size_t>(c) * mMappedFrames;
          for (AudioFrameCount f = 0; f < mMappedFrames; ++f) {
            mResamplerInput[static_cast<size_t>(f) * mOutChannels + c] = src[f];
          }
        }
        inFrames = mMappedFrames;
        inPos = 0;
      }

      resampler.writeNextFrame(mResamplerInput.data() + inPos * mOutChannels);
      ++inPos;
      continue;
    }

    // Read one interleaved frame and scatter it into the output buffer.
    float frame[16]{};
    if (mOutChannels > 16) {
      if (outError) {
        *outError = makeError(ErrCode::kUnknown, "convertToBuffer: too many output channels");
      }
      return OutputStatus::Error;
    }
    resampler.readNextFrame(frame);
    const size_t bytesPerSample = mOutputFormat.getBitsPerChannel() / 8;
    for (uint32_t c = 0; c < mOutChannels; ++c) {
      const size_t offset = (static_cast<size_t>(produced) * mOutChannels + c) * bytesPerSample;
      if (!writeSample(*outputBuffer, offset, frame[c])) {
        if (outError) {
          *outError = makeError(ErrCode::kUnknown, "convertToBuffer: failed to write the output buffer");
        }
        return OutputStatus::Error;
      }
    }
    ++produced;
    mResamplerFrames++;
  }

  mReachedEndOfStream = reachedEndOfStream;
  outputBuffer->mDataByteSize = produced * outputBytesPerFrame;
  if (reachedEndOfStream && produced == 0) {
    return OutputStatus::EndOfStream;
  }
  return produced == capacity ? OutputStatus::HaveData : OutputStatus::InputRanDry;
}

} // namespace coral
