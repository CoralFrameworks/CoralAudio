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

#include "private/graph/AudioUnitReverbImpl.h"

#include <minilog/log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace coral {

namespace {

using ReverbPreset = AudioUnitReverb::ReverbPreset;
using Settings = AudioUnitReverbImpl::Settings;

//!< The Freeverb tunings at 44100 Hz; they are scaled to the sample rate of the graph.
constexpr std::array<int, 8> kCombTunings = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
constexpr std::array<int, 4> kAllpassTunings = { 556, 441, 341, 225 };
//!< The delay of the right channel relative to the left, at 44100 Hz.
constexpr int kStereoSpread = 23;
//!< The Freeverb input gain, which keeps the feedback network at a usable level.
constexpr float kInputGain = 0.015f;
//!< The longest pre-delay any preset asks for, in seconds.
constexpr float kMaxPreDelaySeconds = 0.1f;
//!< The time constant of the wet/dry smoothing, in seconds.
constexpr float kSmoothSeconds = 0.005f;
constexpr float kDefaultWetDryMixPercent = 100.0f;

// clang-format off
const Settings kPresetSettings[] = {
  // Small room: short, dark and tight.
  {0.30f, 0.75f, 0.002f, 2.5f},
  // Medium room.
  {0.48f, 0.60f, 0.004f, 2.5f},
  // Large room.
  {0.66f, 0.45f, 0.008f, 2.5f},
  // Medium hall.
  {0.72f, 0.50f, 0.012f, 2.5f},
  // Large hall.
  {0.85f, 0.35f, 0.020f, 2.5f},
  // Plate: a short pre-delay and a bright, dense tail.
  {0.58f, 0.15f, 0.000f, 2.5f},
  // Medium chamber.
  {0.60f, 0.35f, 0.006f, 2.5f},
  // Large chamber.
  {0.76f, 0.28f, 0.012f, 2.5f},
  // Cathedral: the longest and darkest tail.
  {0.93f, 0.20f, 0.035f, 2.5f},
  // Large room, alternative tuning.
  {0.70f, 0.40f, 0.015f, 2.5f},
  // Medium hall, alternative tuning.
  {0.68f, 0.45f, 0.010f, 2.5f},
  // Medium hall, a brighter alternative.
  {0.74f, 0.55f, 0.014f, 2.5f},
  // Large hall, alternative tuning.
  {0.90f, 0.30f, 0.025f, 2.5f},
};
// clang-format on

static_assert(sizeof(kPresetSettings) / sizeof(kPresetSettings[0]) ==
    static_cast<size_t>(ReverbPreset::kLargeHall2) + 1,
  "one settings row per reverb preset");

const Settings& settingsFor(const ReverbPreset preset) {
  const auto index = static_cast<size_t>(preset);
  if (index >= sizeof(kPresetSettings) / sizeof(kPresetSettings[0])) {
    return kPresetSettings[static_cast<size_t>(ReverbPreset::kMediumHall)];
  }
  return kPresetSettings[index];
}

/** Scales a Freeverb tuning from 44100 Hz to the graph's rate. */
size_t scaledLength(const int tuning, const Float64 sampleRate) {
  const auto length = static_cast<size_t>(
    std::lround(static_cast<double>(tuning) * sampleRate / 44100.0));
  return std::max<size_t>(length, 4);
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

float AudioUnitReverbImpl::CombFilter::process(const float input) {
  const auto output = buffer[index];
  // The one-pole low pass inside the loop is what makes the tail lose its high end.
  filterStore = output * damp2 + filterStore * damp1;
  buffer[index] = input + filterStore * feedback;
  if (++index >= buffer.size()) {
    index = 0;
  }
  return output;
}

void AudioUnitReverbImpl::CombFilter::clear() {
  std::fill(buffer.begin(), buffer.end(), 0.0f);
  index = 0;
  filterStore = 0.0f;
}

float AudioUnitReverbImpl::AllpassFilter::process(const float input) {
  const auto buffered = buffer[index];
  const auto output = -input + buffered;
  buffer[index] = input + buffered * 0.5f;
  if (++index >= buffer.size()) {
    index = 0;
  }
  return output;
}

void AudioUnitReverbImpl::AllpassFilter::clear() {
  std::fill(buffer.begin(), buffer.end(), 0.0f);
  index = 0;
}

void AudioUnitReverbImpl::loadFactoryPreset(const ReverbPreset preset) {
  const auto index = static_cast<uint32_t>(preset);
  mPreset.store(index <= static_cast<uint32_t>(ReverbPreset::kLargeHall2) ? preset
                                                                         : ReverbPreset::kMediumHall,
    std::memory_order_relaxed);
}

void AudioUnitReverbImpl::setWetDryMix(const float wetDryMix) {
  const auto value = std::isfinite(wetDryMix) ? std::clamp(wetDryMix, 0.0f, 100.0f)
                                              : kDefaultWetDryMixPercent;
  mWetDryMix.store(value, std::memory_order_relaxed);
}

void AudioUnitReverbImpl::configureChannel(ChannelState& channel, const size_t channelIndex) {
  // The right channel uses longer delays than the left one, which decorrelates the two and
  // widens a mono input into a stereo tail.
  const auto spread = static_cast<size_t>(
    std::lround(static_cast<double>(kStereoSpread) * mSampleRate / 44100.0));
  const auto offset = (channelIndex % 2 == 0) ? size_t{ 0 } : spread;

  for (size_t i = 0; i < channel.combs.size(); ++i) {
    channel.combs[i].buffer.assign(scaledLength(kCombTunings[i], mSampleRate) + offset, 0.0f);
    channel.combs[i].index = 0;
    channel.combs[i].filterStore = 0.0f;
  }
  for (size_t i = 0; i < channel.allpasses.size(); ++i) {
    channel.allpasses[i].buffer.assign(scaledLength(kAllpassTunings[i], mSampleRate) + offset, 0.0f);
    channel.allpasses[i].index = 0;
  }

  const auto preDelayLength =
    static_cast<size_t>(static_cast<Float64>(kMaxPreDelaySeconds) * mSampleRate) + 1;
  channel.preDelay.assign(preDelayLength, 0.0f);
  channel.preDelayIndex = 0;
}

void AudioUnitReverbImpl::updateCombSettings() {
  const auto feedback = 0.70f + std::clamp(mSettings.roomSize, 0.0f, 1.0f) * 0.28f;
  const auto damp1 = std::clamp(mSettings.damping, 0.0f, 1.0f) * 0.4f;
  const auto damp2 = 1.0f - damp1;
  for (auto& channel: mChannelStates) {
    for (auto& comb: channel.combs) {
      comb.feedback = feedback;
      comb.damp1 = damp1;
      comb.damp2 = damp2;
    }
  }
}

void AudioUnitReverbImpl::clearTail() {
  for (auto& channel: mChannelStates) {
    for (auto& comb: channel.combs) {
      comb.clear();
    }
    for (auto& allpass: channel.allpasses) {
      allpass.clear();
    }
    std::fill(channel.preDelay.begin(), channel.preDelay.end(), 0.0f);
    channel.preDelayIndex = 0;
  }
}

void AudioUnitReverbImpl::initWithInputFormat(const AudioFormat& format) {
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
  mChannelStates.assign(mChannels, ChannelState{});
  for (size_t c = 0; c < mChannelStates.size(); ++c) {
    configureChannel(mChannelStates[c], c);
  }
  mInputPointers.assign(mChannels, nullptr);
  mOutputPointers.assign(mChannels, nullptr);

  mRenderedPreset = mPreset.load(std::memory_order_relaxed);
  mSettings = settingsFor(mRenderedPreset);
  updateCombSettings();
  clearTail();
  mCurrentWetDryMix = getWetDryMix() * 0.01f;
}

void AudioUnitReverbImpl::prepareBeforeRendering() {
  const auto inputFormat = inputFormatForBus(0);
  if (!inputFormat.has_value() || !inputFormat->isValid()) {
    MINILOG_ERROR("Failed to prepare: the input bus has no valid format");
    return;
  }
  initWithInputFormat(inputFormat.value());
  AudioNodeImpl::prepareBeforeRendering();
}

void AudioUnitReverbImpl::onReset() {
  clearTail();
  mCurrentWetDryMix = getWetDryMix() * 0.01f;
}

void AudioUnitReverbImpl::renderBlock(const AudioBufferList* const* inputs,
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

  if (isBypass() || mChannels == 0 || mSampleRate <= 0.0 || mChannelStates.empty()) {
    copyThrough(*input, *outputData, numFrames);
    return;
  }

  // A new preset replaces the room and clears the old tail, so the two presets cannot ring
  // into each other.
  const auto preset = mPreset.load(std::memory_order_relaxed);
  if (preset != mRenderedPreset) {
    mRenderedPreset = preset;
    mSettings = settingsFor(preset);
    updateCombSettings();
    clearTail();
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

  // A stereo output fed from a mono input renders two decorrelated tails, because each output
  // channel takes the first input channel.
  const auto channels = std::min(outChannels, mChannels);
  if (channels == 0) {
    return;
  }
  for (size_t c = 0; c < channels; ++c) {
    const auto* sourceBuffer = input->getBuffer(std::min(c, inChannels - 1));
    if (sourceBuffer == nullptr) {
      continue;
    }
    mInputPointers[c] = static_cast<const float*>(sourceBuffer->mData);
    mOutputPointers[c] = static_cast<float*>(outputData->getBuffer(c)->mData);
  }

  const auto preDelaySamples = std::min(
    mChannelStates[0].preDelay.empty() ? size_t{ 0 } : mChannelStates[0].preDelay.size() - 1,
    static_cast<size_t>(static_cast<Float64>(mSettings.preDelaySeconds) * mSampleRate));
  const auto wetGain = mSettings.wetGain;

  // The live property is smoothed towards its target so a change does not click.
  const auto targetWetDryMix = getWetDryMix() * 0.01f;
  const auto smoothCoefficient =
    1.0f - std::exp(-1.0f / (kSmoothSeconds * static_cast<float>(mSampleRate)));

  for (AudioFrameCount f = 0; f < frames; ++f) {
    mCurrentWetDryMix += (targetWetDryMix - mCurrentWetDryMix) * smoothCoefficient;
    const auto mix = std::clamp(mCurrentWetDryMix, 0.0f, 1.0f);
    const auto dryMix = 1.0f - mix;

    for (size_t c = 0; c < channels; ++c) {
      const auto* source = mInputPointers[c];
      auto* output = mOutputPointers[c];
      if (source == nullptr || output == nullptr) {
        continue;
      }
      auto& channel = mChannelStates[c];
      const auto dry = source[f];
      auto sample = dry;

      if (preDelaySamples > 0 && !channel.preDelay.empty()) {
        const auto readIndex =
          (channel.preDelayIndex + channel.preDelay.size() - preDelaySamples) % channel.preDelay.size();
        const auto delayed = channel.preDelay[readIndex];
        channel.preDelay[channel.preDelayIndex] = dry;
        channel.preDelayIndex = (channel.preDelayIndex + 1) % channel.preDelay.size();
        sample = delayed;
      }

      auto inputSample = sample * kInputGain;
      auto wet = 0.0f;
      for (auto& comb: channel.combs) {
        wet += comb.process(inputSample);
      }
      for (auto& allpass: channel.allpasses) {
        wet = allpass.process(wet);
      }
      wet *= wetGain;

      output[f] = std::clamp(dry * dryMix + wet * mix, -1.0f, 1.0f);
    }
  }

  // If the output is wider than the network, the remaining channels repeat the processed
  // first channel rather than rendering silence.
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


