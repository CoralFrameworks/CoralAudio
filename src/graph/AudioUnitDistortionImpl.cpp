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

#include "private/graph/AudioUnitDistortionImpl.h"

#include "private/graph/AudioGraphUtils.h"

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace coral {

namespace {

constexpr auto kTwoPi = 6.283185307179586f;
constexpr auto kMaxEchoSeconds = 1.0f;
constexpr auto kDcBlockerPole = 0.995f;
//!< The time constant of the parameter smoothing, in seconds.
constexpr auto kSmoothSeconds = 0.005;
constexpr auto kDefaultPreGainDb = -6.0f;
constexpr auto kDefaultWetDryMixPercent = 50.0f;
//!< The highest bit depth the quantizer supports: below this it is a hard quantizer anyway.
constexpr auto kMaxBitDepth = 16;

/** The shape a preset drives its non-linearity with. */
enum class Shaper : uint8_t {
  //!< A soft saturation.
  kTanh,
  //!< A hard clip.
  kHardClip,
  //!< A cubic curve, for a harsher upper harmonic.
  kCubic,
  //!< An odd square, for a very aggressive harmonic.
  kSquared,
  //!< A wave folder.
  kFold,
};

/**
 * The stages one factory preset turns on.
 *
 * Apple does not publish the coefficients of the presets, so the values reproduce the
 * character of the preset from the stages the distortion unit documents: more drive, a
 * narrower band, a coarser decimator, a longer echo, or a ring modulator.
 */
struct PresetSettings {
  //!< Extra gain into the shaper, on top of the pre-gain property.
  float drive;
  //!< Compensation after the shaper, so the presets keep a comparable level.
  float outputGain;
  Shaper shaper;
  //!< Low pass cutoff in Hz, 0 disables the stage.
  float toneHz;
  //!< High pass cutoff in Hz, 0 disables the stage.
  float highPassHz;
  //!< Bit depth of the quantizer, 0 disables the stage.
  int bitDepth;
  //!< The number of frames the decimator holds a sample, 1 disables the stage.
  int sampleHold;
  //!< Echo time in seconds, 0 disables the stage.
  float echoSeconds;
  float echoFeedback;
  float echoMix;
  //!< Ring modulator frequency in Hz, 0 disables the stage.
  float ringHz;
  float ringDepth;
};

// clang-format off
const PresetSettings kPresetSettings[] = {
  // DrumsBitBrush: a coarse bit crusher with a short slap.
  { 6.0f, 0.55f, Shaper::kHardClip, 12000.0f,   0.0f,  5,  2, 0.035f, 0.25f, 0.30f, 0.0f, 0.0f },
  // DrumsBufferBeats: a long, feedback-heavy beat repeat.
  { 3.0f, 0.70f, Shaper::kTanh,      9000.0f,  60.0f,  0,  1, 0.330f, 0.55f, 0.50f, 0.0f, 0.0f },
  // DrumsLoFi: a narrow, quiet, low fidelity drums sound.
  { 2.5f, 0.80f, Shaper::kHardClip,  3500.0f, 150.0f,  6,  3, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  // MultiBrokenSpeaker: a torn speaker, mostly hard clipping and a coarse quantizer.
  { 8.0f, 0.45f, Shaper::kHardClip,  4500.0f, 300.0f,  4,  2, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  // MultiCellphoneConcert: a telephone band around the voice.
  { 4.0f, 0.60f, Shaper::kHardClip,  3200.0f, 450.0f,  8,  1, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  // MultiDecimated1..4: the same shaper, with a coarser and coarser decimator.
  { 2.0f, 0.85f, Shaper::kTanh,         0.0f,   0.0f,  8,  2, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  { 2.0f, 0.85f, Shaper::kTanh,         0.0f,   0.0f,  7,  4, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  { 2.0f, 0.85f, Shaper::kTanh,         0.0f,   0.0f,  6,  8, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  { 2.0f, 0.85f, Shaper::kTanh,         0.0f,   0.0f,  5, 16, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  // MultiDistortedFunk: a warm, mid-forward saturation.
  { 3.0f, 0.75f, Shaper::kTanh,      6000.0f, 100.0f,  0,  1, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  // MultiDistortedCubed: a cubic curve.
  { 4.0f, 0.80f, Shaper::kCubic,     7000.0f,   0.0f,  0,  1, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  // MultiDistortedSquared: an odd square curve.
  { 4.0f, 0.80f, Shaper::kSquared,   7000.0f,   0.0f,  0,  1, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  // MultiEcho1/2: two echo lengths.
  { 1.6f, 0.90f, Shaper::kTanh,      9000.0f,  90.0f,  0,  1, 0.140f, 0.45f, 0.55f, 0.0f, 0.0f },
  { 1.6f, 0.90f, Shaper::kTanh,      9000.0f,  90.0f,  0,  1, 0.270f, 0.50f, 0.55f, 0.0f, 0.0f },
  // MultiEchoTight1/2: the same echoes, tighter and with more feedback.
  { 1.6f, 0.90f, Shaper::kTanh,      9000.0f,  90.0f,  0,  1, 0.055f, 0.60f, 0.65f, 0.0f, 0.0f },
  { 1.6f, 0.90f, Shaper::kTanh,      9000.0f,  90.0f,  0,  1, 0.085f, 0.65f, 0.65f, 0.0f, 0.0f },
  // MultiEverythingIsBroken: every stage on, all of them rough.
  { 6.0f, 0.50f, Shaper::kHardClip,  5000.0f, 120.0f,  4,  4, 0.190f, 0.50f, 0.50f, 0.0f, 0.0f },
  // SpeechAlienChatter: a slow ring modulator over a saturated voice.
  { 2.5f, 0.80f, Shaper::kTanh,      5000.0f, 200.0f,  0,  1, 0.000f, 0.00f, 0.00f, 28.0f, 0.70f },
  // SpeechCosmicInterference: a fast ring modulator with a short echo.
  { 2.2f, 0.80f, Shaper::kTanh,      6000.0f, 150.0f,  0,  1, 0.110f, 0.45f, 0.35f, 180.0f, 0.45f },
  // SpeechGoldenPi: a quantized voice with an echo around pi.
  { 3.0f, 0.70f, Shaper::kHardClip,  4000.0f, 180.0f,  5,  3, 0.310f, 0.50f, 0.40f, 0.0f, 0.0f },
  // SpeechRadioTower: a narrow band radio voice.
  { 5.0f, 0.60f, Shaper::kHardClip,  2800.0f, 500.0f,  7,  1, 0.000f, 0.00f, 0.00f, 0.0f, 0.0f },
  // SpeechWaves: a slow tremolo with a touch of echo.
  { 1.8f, 0.90f, Shaper::kTanh,      7000.0f, 120.0f,  0,  1, 0.050f, 0.30f, 0.30f, 6.0f, 0.50f },
};
// clang-format on

static_assert(sizeof(kPresetSettings) / sizeof(kPresetSettings[0]) ==
    static_cast<size_t>(AudioUnitDistortion::DistortionPreset::kSpeechWaves) + 1,
  "one settings row per distortion preset");

const PresetSettings& settingsFor(const AudioUnitDistortion::DistortionPreset preset) {
  const auto index = static_cast<size_t>(preset);
  if (index >= sizeof(kPresetSettings) / sizeof(kPresetSettings[0])) {
    return kPresetSettings[0];
  }
  return kPresetSettings[index];
}

float clampFinite(const float value, const float low, const float high, const float fallback) {
  if (!std::isfinite(value)) {
    return fallback;
  }
  return std::clamp(value, low, high);
}

float dbToLinear(const float db) {
  return std::pow(10.0f, db / 20.0f);
}

/**
 * The coefficient of a one-pole low pass with the given cutoff, for the given sample rate.
 */
float onePoleCoefficient(const float cutoffHz, const Float64 sampleRate) {
  if (cutoffHz <= 0.0f || sampleRate <= 0.0) {
    return 0.0f;
  }
  const auto nyquist = static_cast<float>(sampleRate * 0.5);
  const auto cutoff = std::clamp(cutoffHz, 10.0f, nyquist * 0.99f);
  return 1.0f - std::exp(-kTwoPi * cutoff / static_cast<float>(sampleRate));
}

float applyShaper(const Shaper shaper, const float sample) {
  switch (shaper) {
    case Shaper::kTanh:
      return std::tanh(sample);
    case Shaper::kHardClip:
      return std::clamp(sample, -1.0f, 1.0f);
    case Shaper::kCubic:
      // A cubic saturates on its own; the clamp only catches the runaway range.
      return std::clamp(sample * sample * sample, -1.0f, 1.0f);
    case Shaper::kSquared:
      return std::clamp(sample * std::abs(sample), -1.0f, 1.0f);
    case Shaper::kFold:
      return std::sin(sample * 1.5707963267948966f);
  }
  return sample;
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
    std::copy_n(src, std::min(frames, static_cast<AudioFrameCount>(dstBuffer->mDataByteSize / sizeof(float))),
      dst);
  }
}

} // namespace anonymous

void AudioUnitDistortionImpl::setPreGain(const float newPreGain) {
  mPreGain.store(clampFinite(newPreGain, -80.0f, 20.0f, kDefaultPreGainDb), std::memory_order_relaxed);
}

void AudioUnitDistortionImpl::setWetDryMix(const float newWetDryMix) {
  mWetDryMix.store(clampFinite(newWetDryMix, 0.0f, 100.0f, kDefaultWetDryMixPercent),
    std::memory_order_relaxed);
}

void AudioUnitDistortionImpl::loadFactoryPreset(const DistortionPreset preset) {
  const auto index = static_cast<uint32_t>(preset);
  mPreset.store(index <= static_cast<uint32_t>(DistortionPreset::kSpeechWaves) ? preset
                                                                              : DistortionPreset::kDrumsBitBrush,
    std::memory_order_relaxed);
}

void AudioUnitDistortionImpl::onReset() {
  resetDspState();
}

void AudioUnitDistortionImpl::resetDspState() {
  for (auto& state: mChannelStates) {
    state.toneLowPass = 0.0f;
    state.highPassLowPass = 0.0f;
    state.dcPreviousInput = 0.0f;
    state.dcPreviousOutput = 0.0f;
    state.heldSample = 0.0f;
    state.ringPhase = 0.0f;
    std::fill(state.echo.begin(), state.echo.end(), 0.0f);
  }
  mEchoWriteIndex = 0;
  mHoldCounter = 0;
  mCurrentPreGain = dbToLinear(getPreGain());
  mCurrentWetDryMix = getWetDryMix() * 0.01f;
  mRenderedPreset = mPreset.load(std::memory_order_relaxed);
}

void AudioUnitDistortionImpl::prepareBeforeRendering() {
  const auto inputFormat = inputFormatForBus(0);
  if (!inputFormat.has_value() || !inputFormat->isValid()) {
    MINILOG_ERROR("Failed to prepare: the input bus has no valid format");
    return;
  }
  initWithInputFormat(inputFormat.value());
  AudioNodeImpl::prepareBeforeRendering();
}

void AudioUnitDistortionImpl::initWithInputFormat(const AudioFormat& format) {
  if (!format.isValid()) {
    return;
  }
  const auto sampleRate = format.getSampleRate();
  if (sampleRate <= 0.0) {
    return;
  }

  auto channels = std::max(format.getChannelCount(), 1u);
  if (const auto outputFormat = outputFormatForBus(0);
      outputFormat.has_value() && outputFormat->isValid()) {
    channels = std::max(channels, outputFormat->getChannelCount());
  }
  if (channels == 0) {
    return;
  }

  mSampleRate = sampleRate;
  mChannels = channels;

  // The lines are sized for the longest echo a preset uses, plus a frame, so that a preset
  // change never allocates on a render thread.
  mEchoSize = static_cast<size_t>(mSampleRate * static_cast<Float64>(kMaxEchoSeconds)) + 1;
  mChannelStates.assign(mChannels, ChannelState{});
  for (auto& state: mChannelStates) {
    state.echo.assign(mEchoSize, 0.0f);
  }
  mInputPointers.assign(mChannels, nullptr);
  mOutputPointers.assign(mChannels, nullptr);

  resetDspState();
}

void AudioUnitDistortionImpl::renderBlock(const AudioBufferList* const* inputs,
  const uint32_t numInputs,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& /* when */) {
  // The render buffer is silent before this call, so a block that writes nothing renders as
  // silence.
  if (outputData == nullptr || numFrames == 0) {
    return;
  }

  const auto* input = numInputs > 0 ? inputs[0] : nullptr;
  if (input == nullptr || input->numberOfBuffers() == 0) {
    return;
  }

  // A bypassed effect, and an effect that has not been prepared yet, pass the input through.
  if (isBypass() || mChannelStates.empty() || mEchoSize == 0 || mChannels == 0) {
    copyThrough(*input, *outputData, numFrames);
    return;
  }

  const auto preset = mPreset.load(std::memory_order_relaxed);
  if (preset != mRenderedPreset) {
    // A new preset starts with clean filters and a clean echo, so the previous preset's tail
    // cannot leak into it.
    resetDspState();
  }
  const auto& settings = settingsFor(preset);

  const auto inChannels = input->numberOfBuffers();
  const auto outChannels = outputData->numberOfBuffers();
  if (inChannels == 0 || outChannels == 0) {
    return;
  }

  // The graph renders every buffer in this list with the same frame count; the byte sizes are
  // still checked so a short buffer cannot be read or written past its end.
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

  // The tone and decimation coefficients only depend on the preset and the rate, so they are
  // computed once per block.
  const auto toneCoefficient = onePoleCoefficient(settings.toneHz, mSampleRate);
  const auto highPassCoefficient = onePoleCoefficient(settings.highPassHz, mSampleRate);
  const auto hasTone = toneCoefficient > 0.0f;
  const auto hasHighPass = highPassCoefficient > 0.0f;
  const auto hasEcho = settings.echoSeconds > 0.0f && mEchoSize > 1;
  const auto echoDelay = hasEcho
                           ? std::min(static_cast<size_t>(static_cast<Float64>(settings.echoSeconds) * mSampleRate),
                               mEchoSize - 1)
                           : size_t{ 0 };
  const auto echoFeedback = std::clamp(settings.echoFeedback, 0.0f, 0.9f);
  const auto echoMix = std::clamp(settings.echoMix, 0.0f, 1.0f);
  const auto hasBitDepth = settings.bitDepth > 0;
  const auto bitLevels = hasBitDepth ? std::pow(2.0f, static_cast<float>(
                                                      std::clamp(settings.bitDepth, 2, kMaxBitDepth) - 1))
                                     : 1.0f;
  const auto holdFrames = std::max(settings.sampleHold, 1);
  const auto hasRing = settings.ringHz > 0.0f;
  const auto ringIncrement = hasRing ? kTwoPi * settings.ringHz / static_cast<float>(mSampleRate) : 0.0f;
  const auto ringDepth = std::clamp(settings.ringDepth, 0.0f, 1.0f);
  const auto outputGain = settings.outputGain;

  // The live properties are smoothed towards their targets over the block, which keeps a
  // property change from clicking.
  const auto targetPreGain = dbToLinear(getPreGain());
  const auto targetWetDryMix = getWetDryMix() * 0.01f;
  const auto smoothCoefficient = 1.0f - std::exp(-1.0f / (kSmoothSeconds * static_cast<float>(mSampleRate)));

  for (AudioFrameCount f = 0; f < frames; ++f) {
    mCurrentPreGain += (targetPreGain - mCurrentPreGain) * smoothCoefficient;
    mCurrentWetDryMix += (targetWetDryMix - mCurrentWetDryMix) * smoothCoefficient;

    const auto inputGain = mCurrentPreGain * settings.drive;
    if (holdFrames <= 1 || (mHoldCounter % static_cast<uint32_t>(holdFrames)) == 0) {
      for (size_t c = 0; c < channels; ++c) {
        if (mInputPointers[c] != nullptr) {
          mChannelStates[c].heldSample = mInputPointers[c][f] * inputGain;
        }
      }
    }
    ++mHoldCounter;

    const auto mix = std::clamp(mCurrentWetDryMix, 0.0f, 1.0f);
    const auto dryMix = 1.0f - mix;

    for (size_t c = 0; c < channels; ++c) {
      const auto* source = mInputPointers[c];
      auto* output = mOutputPointers[c];
      if (source == nullptr || output == nullptr) {
        continue;
      }
      const auto dry = source[f];
      auto& state = mChannelStates[c];
      auto sample = applyShaper(settings.shaper, state.heldSample);
      sample *= outputGain;

      if (hasTone) {
        state.toneLowPass += toneCoefficient * (sample - state.toneLowPass);
        sample = state.toneLowPass;
      }
      if (hasHighPass) {
        state.highPassLowPass += highPassCoefficient * (sample - state.highPassLowPass);
        sample -= state.highPassLowPass;
      }
      if (hasBitDepth) {
        sample = std::round(sample * bitLevels) / bitLevels;
      }
      if (hasEcho) {
        const auto readIndex = (mEchoWriteIndex + mEchoSize - echoDelay) % mEchoSize;
        const auto echo = state.echo[readIndex];
        state.echo[mEchoWriteIndex] = std::clamp(sample + echo * echoFeedback, -2.0f, 2.0f);
        sample = sample * (1.0f - echoMix) + echo * echoMix;
      }
      if (hasRing) {
        state.ringPhase += ringIncrement;
        if (state.ringPhase >= kTwoPi) {
          state.ringPhase -= kTwoPi;
        }
        const auto modulation = 1.0f - ringDepth + ringDepth * std::sin(state.ringPhase);
        sample *= modulation;
      }

      // The shapers and the ring modulator can leave a DC offset behind, which a source of
      // this graph should not carry into the mix.
      const auto dcBlocked = sample - state.dcPreviousInput + kDcBlockerPole * state.dcPreviousOutput;
      state.dcPreviousInput = sample;
      state.dcPreviousOutput = dcBlocked;

      const auto wet = std::clamp(dcBlocked, -1.0f, 1.0f);
      output[f] = std::clamp(dry * dryMix + wet * mix, -1.0f, 1.0f);
    }

    if (hasEcho) {
      mEchoWriteIndex = (mEchoWriteIndex + 1) % mEchoSize;
    }
  }

  // The graph normally renders as many channels as the effect was prepared for. If the output
  // is wider than the input, the remaining channels repeat the processed first channel rather
  // than rendering the raw input or silence.
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


