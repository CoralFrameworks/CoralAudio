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

#include "private/graph/AudioUnitEqualizerImpl.h"

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace coral {

namespace {

using FilterType = AudioUnitEqualizer::FilterType;

constexpr auto kTwoPi = 6.283185307179586;
//!< The Q of a second-order Butterworth filter.
constexpr auto kButterworthQ = 0.7071067811865476;
//!< The time constant of the global gain smoothing, in seconds.
constexpr auto kSmoothSeconds = 0.005;
constexpr auto kDefaultGlobalGainDb = 0.0f;

/** The normalized coefficients of one biquad. */
struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

float dbToLinear(const float db) {
  return std::pow(10.0f, db / 20.0f);
}

/**
 * Returns the coefficients of the biquad AVFoundation's filter type describes.
 *
 * The formulae are the second-order filters of the Audio EQ Cookbook. A bandwidth in octaves
 * is converted to Q, and the shelves use a fixed slope of one except the resonant variant,
 * which takes its resonance from the bandwidth through Q.
 */
Biquad computeBiquad(const FilterType type,
  const float frequency,
  const float bandwidth,
  const float gainDb,
  const Float64 sampleRate) {
  Biquad result;
  if (sampleRate <= 0.0) {
    return result;
  }

  const auto nyquist = sampleRate * 0.5;
  const auto cutoff = std::clamp(static_cast<Float64>(frequency), 20.0, nyquist * 0.98);
  const auto octaves = std::clamp(static_cast<Float64>(bandwidth), 0.05, 5.0);
  const auto w0 = kTwoPi * cutoff / sampleRate;
  const auto cosw = std::cos(w0);
  const auto sinw = std::sin(w0);
  if (sinw <= 0.0) {
    return result;
  }
  const auto amplitude = std::pow(10.0, static_cast<Float64>(gainDb) / 40.0);
  const auto ratio = std::pow(2.0, octaves);
  const auto q = std::sqrt(ratio) / (ratio - 1.0);
  const auto alphaQ = sinw / (2.0 * q);
  // RBJ's bandwidth form: alpha = sin(w0) * sinh(ln(2)/2 * BW * w0 / sin(w0)).
  const auto alphaBandwidth = sinw * std::sinh(std::log(2.0) / 2.0 * octaves * w0 / sinw);
  const auto alphaButterworth = sinw / (2.0 * kButterworthQ);

  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a0 = 1.0;
  double a1 = 0.0;
  double a2 = 0.0;

  switch (type) {
    case FilterType::kFilterTypeParametric: {
      b0 = 1.0 + alphaBandwidth * amplitude;
      b1 = -2.0 * cosw;
      b2 = 1.0 - alphaBandwidth * amplitude;
      a0 = 1.0 + alphaBandwidth / amplitude;
      a1 = -2.0 * cosw;
      a2 = 1.0 - alphaBandwidth / amplitude;
      break;
    }
    case FilterType::kFilterTypeLowPass:
    case FilterType::kFilterTypeResonantLowPass: {
      const auto alpha = type == FilterType::kFilterTypeLowPass ? alphaButterworth : alphaBandwidth;
      b0 = (1.0 - cosw) * 0.5;
      b1 = 1.0 - cosw;
      b2 = b0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cosw;
      a2 = 1.0 - alpha;
      break;
    }
    case FilterType::kFilterTypeHighPass:
    case FilterType::kFilterTypeResonantHighPass: {
      const auto alpha = type == FilterType::kFilterTypeHighPass ? alphaButterworth : alphaBandwidth;
      b0 = (1.0 + cosw) * 0.5;
      b1 = -(1.0 + cosw);
      b2 = b0;
      a0 = 1.0 + alpha;
      a1 = -2.0 * cosw;
      a2 = 1.0 - alpha;
      break;
    }
    case FilterType::kFilterTypeBandPass: {
      b0 = alphaBandwidth;
      b1 = 0.0;
      b2 = -alphaBandwidth;
      a0 = 1.0 + alphaBandwidth;
      a1 = -2.0 * cosw;
      a2 = 1.0 - alphaBandwidth;
      break;
    }
    case FilterType::kFilterTypeBandStop: {
      b0 = 1.0;
      b1 = -2.0 * cosw;
      b2 = 1.0;
      a0 = 1.0 + alphaBandwidth;
      a1 = -2.0 * cosw;
      a2 = 1.0 - alphaBandwidth;
      break;
    }
    case FilterType::kFilterTypeLowShelf:
    case FilterType::kFilterTypeResonantLowShelf: {
      // A slope of one for the plain shelf, the bandwidth's Q for the resonant one.
      const auto alpha = type == FilterType::kFilterTypeLowShelf ? sinw * 0.5 * std::sqrt(2.0) : alphaQ;
      const auto beta = 2.0 * std::sqrt(amplitude) * alpha;
      b0 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosw + beta);
      b1 = 2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosw);
      b2 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosw - beta);
      a0 = (amplitude + 1.0) + (amplitude - 1.0) * cosw + beta;
      a1 = -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosw);
      a2 = (amplitude + 1.0) + (amplitude - 1.0) * cosw - beta;
      break;
    }
    case FilterType::kFilterTypeHighShelf:
    case FilterType::kFilterTypeResonantHighShelf: {
      const auto alpha = type == FilterType::kFilterTypeHighShelf ? sinw * 0.5 * std::sqrt(2.0) : alphaQ;
      const auto beta = 2.0 * std::sqrt(amplitude) * alpha;
      b0 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosw + beta);
      b1 = -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosw);
      b2 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosw - beta);
      a0 = (amplitude + 1.0) - (amplitude - 1.0) * cosw + beta;
      a1 = 2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosw);
      a2 = (amplitude + 1.0) - (amplitude - 1.0) * cosw - beta;
      break;
    }
  }

  if (std::abs(a0) < 1e-12) {
    return result;
  }
  result.b0 = static_cast<float>(b0 / a0);
  result.b1 = static_cast<float>(b1 / a0);
  result.b2 = static_cast<float>(b2 / a0);
  result.a1 = static_cast<float>(a1 / a0);
  result.a2 = static_cast<float>(a2 / a0);
  return result;
}

/** Copies the input through, duplicating the last channel when the output is wider. */
void copyThrough(const AudioBufferList& input, AudioBufferList& output, const AudioFrameCount numFrames) {
  const auto inChannels = input.numberOfBuffers();
  const auto outChannels = output.numberOfBuffers();
  if (inChannels == 0 || outChannels == 0) {
    return;
  }
  for (size_t c = 0; c < outChannels; ++c) {
    const auto* srcBuffer = input.getBuffer(std::min(c, inChannels - 1));
    auto* dstBuffer = output.getBuffer(c);
    if (srcBuffer == nullptr || dstBuffer == nullptr) {
      continue;
    }
    const auto* src = static_cast<const float*>(srcBuffer->mData);
    auto* dst = static_cast<float*>(dstBuffer->mData);
    if (src == nullptr || dst == nullptr) {
      continue;
    }
    const auto frames =
      std::min(numFrames, static_cast<AudioFrameCount>(srcBuffer->mDataByteSize / sizeof(float)));
    std::copy_n(src,
      std::min(frames, static_cast<AudioFrameCount>(dstBuffer->mDataByteSize / sizeof(float))), dst);
  }
}

} // namespace anonymous

AudioUnitEqualizerImpl::AudioUnitEqualizerImpl(const uint32_t numberOfBands) noexcept
  : mNumberOfBands(numberOfBands)
  , mBands(numberOfBands)
  , mRenderBands(numberOfBands)
  , mFilters(numberOfBands) {
}

void AudioUnitEqualizerImpl::setBypass(const bool newBypass) {
  mBypass.store(newBypass, std::memory_order_relaxed);
}

std::vector<AudioUnitEqualizerImpl::FilterParameters> AudioUnitEqualizerImpl::getBands() const {
  const std::lock_guard<std::mutex> guard(mBandsLock);
  return mBands;
}

void AudioUnitEqualizerImpl::setBandParameters(const uint32_t band, const FilterParameters& parameters) {
  if (band >= mNumberOfBands) {
    MINILOG_WARN("The equalizer has {} bands: band {} was not set", mNumberOfBands, band);
    return;
  }
  {
    const std::lock_guard<std::mutex> guard(mBandsLock);
    mBands[band] = sanitize(parameters);
  }
  mBandsVersion.fetch_add(1, std::memory_order_release);
}

void AudioUnitEqualizerImpl::setBands(const std::vector<FilterParameters>& bands) {
  if (bands.size() != mNumberOfBands) {
    MINILOG_WARN("The equalizer has {} bands, {} were given: the extra bands are ignored", mNumberOfBands,
      bands.size());
  }
  {
    const std::lock_guard<std::mutex> guard(mBandsLock);
    const auto count = std::min<size_t>(bands.size(), mNumberOfBands);
    for (size_t i = 0; i < count; ++i) {
      mBands[i] = sanitize(bands[i]);
    }
  }
  mBandsVersion.fetch_add(1, std::memory_order_release);
}

void AudioUnitEqualizerImpl::setGlobalGain(const float newGlobalGain) {
  const auto value = std::isfinite(newGlobalGain) ? std::clamp(newGlobalGain, -96.0f, 24.0f)
                                                  : kDefaultGlobalGainDb;
  mGlobalGain.store(value, std::memory_order_relaxed);
}

AudioUnitEqualizerImpl::FilterParameters
AudioUnitEqualizerImpl::sanitize(const FilterParameters& parameters) {
  auto result = parameters;
  result.frequency = std::isfinite(result.frequency) ? std::clamp(result.frequency, 20.0f, 20000.0f) : 1000.0f;
  result.bandwidth = std::isfinite(result.bandwidth) ? std::clamp(result.bandwidth, 0.05f, 5.0f) : 1.0f;
  result.gain = std::isfinite(result.gain) ? std::clamp(result.gain, -96.0f, 24.0f) : 0.0f;
  const auto type = static_cast<uint32_t>(result.filterType);
  if (type > static_cast<uint32_t>(FilterType::kFilterTypeResonantHighShelf)) {
    result.filterType = FilterType::kFilterTypeParametric;
  }
  return result;
}

void AudioUnitEqualizerImpl::updateCoefficients() {
  if (mSampleRate <= 0.0) {
    return;
  }
  const auto count = std::min(mRenderBands.size(), mFilters.size());
  for (size_t i = 0; i < count; ++i) {
    const auto& parameters = mRenderBands[i];
    const auto coefficients =
      computeBiquad(parameters.filterType, parameters.frequency, parameters.bandwidth, parameters.gain,
        mSampleRate);
    auto& filter = mFilters[i];
    filter.b0 = coefficients.b0;
    filter.b1 = coefficients.b1;
    filter.b2 = coefficients.b2;
    filter.a1 = coefficients.a1;
    filter.a2 = coefficients.a2;
    filter.active = !parameters.bypass;
  }
  for (size_t i = count; i < mFilters.size(); ++i) {
    mFilters[i].active = false;
  }
}

void AudioUnitEqualizerImpl::prepareBeforeRendering() {
  const auto inputFormat = inputFormatForBus(0);
  if (!inputFormat.has_value() || !inputFormat->isValid()) {
    MINILOG_ERROR("Failed to prepare: the input bus has no valid format");
    return;
  }
  const auto sampleRate = inputFormat->getSampleRate();
  if (sampleRate <= 0.0) {
    return;
  }

  auto channels = std::max(inputFormat->getChannelCount(), 1u);
  if (const auto outputFormat = outputFormatForBus(0);
      outputFormat.has_value() && outputFormat->isValid()) {
    channels = std::max(channels, outputFormat->getChannelCount());
  }
  if (channels == 0) {
    return;
  }

  mSampleRate = sampleRate;
  mChannels = channels;
  for (auto& filter: mFilters) {
    filter.z1.assign(mChannels, 0.0f);
    filter.z2.assign(mChannels, 0.0f);
  }
  mInputPointers.assign(mChannels, nullptr);
  mOutputPointers.assign(mChannels, nullptr);

  // The next render must not pick up a stale snapshot, so take one now and compute the
  // coefficients for the new rate.
  {
    const std::lock_guard<std::mutex> guard(mBandsLock);
    mRenderBands = mBands;
  }
  mRenderedVersion = mBandsVersion.load(std::memory_order_acquire);
  mCurrentGlobalGain = dbToLinear(getGlobalGain());
  updateCoefficients();

  AudioNodeImpl::prepareBeforeRendering();
}

void AudioUnitEqualizerImpl::onReset() {
  for (auto& filter: mFilters) {
    std::fill(filter.z1.begin(), filter.z1.end(), 0.0f);
    std::fill(filter.z2.begin(), filter.z2.end(), 0.0f);
  }
  mCurrentGlobalGain = dbToLinear(getGlobalGain());
}

void AudioUnitEqualizerImpl::renderBlock(const AudioBufferList* const* inputs,
  const uint32_t numInputs,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& /* when */) {
  if (outputData == nullptr || numFrames == 0) {
    return;
  }

  const auto* input = numInputs > 0 ? inputs[0] : nullptr;
  if (input == nullptr || input->numberOfBuffers() == 0) {
    return;
  }

  if (isBypass() || mChannels == 0 || mSampleRate <= 0.0) {
    copyThrough(*input, *outputData, numFrames);
    return;
  }

  // The bands only have to be copied when they changed, so the steady state has no lock.
  const auto version = mBandsVersion.load(std::memory_order_acquire);
  if (version != mRenderedVersion) {
    {
      const std::lock_guard<std::mutex> guard(mBandsLock);
      mRenderBands = mBands;
    }
    mRenderedVersion = version;
    updateCoefficients();
  }

  const auto inChannels = input->numberOfBuffers();
  const auto outChannels = outputData->numberOfBuffers();
  if (inChannels == 0 || outChannels == 0) {
    return;
  }

  auto frames = numFrames;
  for (size_t c = 0; c < inChannels; ++c) {
    if (const auto* buffer = input->getBuffer(c); buffer != nullptr) {
      frames = std::min(frames, static_cast<AudioFrameCount>(buffer->mDataByteSize / sizeof(float)));
    }
  }
  for (size_t c = 0; c < outChannels; ++c) {
    if (const auto* buffer = outputData->getBuffer(c); buffer != nullptr) {
      frames = std::min(frames, static_cast<AudioFrameCount>(buffer->mDataByteSize / sizeof(float)));
    }
  }
  if (frames == 0) {
    return;
  }

  const auto channels = std::min({ inChannels, outChannels, mChannels });
  if (channels == 0) {
    return;
  }

  for (size_t c = 0; c < channels; ++c) {
    mInputPointers[c] = static_cast<const float*>(input->getBuffer(c)->mData);
    mOutputPointers[c] = static_cast<float*>(outputData->getBuffer(c)->mData);
  }

  // The global gain is smoothed towards its target so a property change does not click.
  const auto targetGlobalGain = dbToLinear(getGlobalGain());
  const auto smoothCoefficient =
    1.0f - std::exp(-1.0f / (kSmoothSeconds * static_cast<float>(mSampleRate)));

  for (AudioFrameCount f = 0; f < frames; ++f) {
    mCurrentGlobalGain += (targetGlobalGain - mCurrentGlobalGain) * smoothCoefficient;
    for (size_t c = 0; c < channels; ++c) {
      const auto* source = mInputPointers[c];
      auto* output = mOutputPointers[c];
      if (source == nullptr || output == nullptr) {
        continue;
      }
      auto sample = source[f];
      for (auto& filter: mFilters) {
        if (!filter.active) {
          continue;
        }
        const auto filtered = filter.b0 * sample + filter.z1[c];
        filter.z1[c] = filter.b1 * sample - filter.a1 * filtered + filter.z2[c];
        filter.z2[c] = filter.b2 * sample - filter.a2 * filtered;
        sample = filtered;
      }
      output[f] = sample * mCurrentGlobalGain;
    }
  }

  // If the output is wider than the input, the remaining channels repeat the processed first
  // channel rather than rendering the raw input or silence.
  for (size_t c = channels; c < outChannels; ++c) {
    auto* outputBuffer = outputData->getBuffer(c);
    if (outputBuffer == nullptr || outputBuffer->mData == nullptr) {
      continue;
    }
    auto* output = static_cast<float*>(outputBuffer->mData);
    if (mOutputPointers[0] != nullptr) {
      std::copy_n(mOutputPointers[0], frames, output);
      continue;
    }
    const auto* sourceBuffer = input->getBuffer(std::min(c, inChannels - 1));
    if (sourceBuffer == nullptr || sourceBuffer->mData == nullptr) {
      continue;
    }
    std::copy_n(static_cast<const float*>(sourceBuffer->mData), frames, output);
  }
}

} // namespace coral
