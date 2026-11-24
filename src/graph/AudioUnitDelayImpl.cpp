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

#include "private/graph/AudioUnitDelayImpl.h"

#include "private/graph/AudioGraphUtils.h"

#include <minilog/log.h>

#include <algorithm>
#include <cmath> // std::isfinite

namespace coral {

AudioUnitDelayImpl::AudioUnitDelayImpl() = default;

AudioUnitDelayImpl::~AudioUnitDelayImpl() = default;

void AudioUnitDelayImpl::setDelayTime(const uint64_t newDelayTime) {
  delayTime.store(std::clamp(newDelayTime, static_cast<uint64_t>(0), kMaxDelaySeconds), std::memory_order_relaxed);
}

void AudioUnitDelayImpl::setFeedback(const float newFeedback) {
  feedback.store(std::clamp(newFeedback, -100.f, 100.f), std::memory_order_relaxed);
}

void AudioUnitDelayImpl::setLowPassCutoff(const float newLowPassCutoff) {
  auto cutoff = newLowPassCutoff;
  if (mSampleRate != 0) {
    cutoff = std::clamp(newLowPassCutoff, 10.f, static_cast<float>(mSampleRate * 0.5));
  }
  lowPassCutoff.store(cutoff, std::memory_order_relaxed);
}

void AudioUnitDelayImpl::setWetDryMix(const float newWetDryMix) {
  // The property is a percentage, as AVFoundation defines it.
  const auto value = std::isfinite(newWetDryMix) ? std::clamp(newWetDryMix, 0.f, 100.f) : 100.f;
  wetDryMix.store(value, std::memory_order_relaxed);
}

void AudioUnitDelayImpl::onReset() {
  // Clearing the delay lines is what silences the tail of the effect, which is what a reset is
  // for.
  for (auto& line: mDelayLines) {
    std::fill(line.begin(), line.end(), 0.0f);
  }
  std::fill(mLastFilteredSamples.begin(), mLastFilteredSamples.end(), 0.0f);
  mWriteIndex = 0;
}

void AudioUnitDelayImpl::prepareBeforeRendering() {
  const auto format = inputFormatForBus(0);
  if (!format.has_value() || !format->isValid()) {
    MINILOG_ERROR("Failed to prepare: the input bus has no valid format");
    return;
  }

  initWithInputFormat(format.value());
  AudioNodeImpl::prepareBeforeRendering();
}

void AudioUnitDelayImpl::initWithInputFormat(const AudioFormat& format) {
  if (!format.isValid()) {
    return;
  }
  const auto channels = std::max(format.getChannelCount(), 1u);
  mSampleRate = format.getSampleRate();
  if (mSampleRate <= 0) {
    return;
  }

  // The lines are sized for the longest delay the API allows, so that a delay time set while
  // the graph is running never allocates on a render thread.
  const auto maxDelaySamples = static_cast<size_t>(mSampleRate * static_cast<Float64>(kMaxDelaySeconds));
  mDelayLines.assign(channels, std::vector<float>(maxDelaySamples, 0.0f));
  mLastFilteredSamples.assign(channels, 0.0f);
  mWriteIndex = 0;
  mCoeffCutoff = 0.f;
  updateLowPassCutoff(mSampleRate);
}

void AudioUnitDelayImpl::updateLowPassCutoff(const Float64 sampleRate) {
  if (sampleRate <= 0) {
    return;
  }
  const auto cutoff = getLowPassCutoff();
  if (cutoff == mCoeffCutoff) {
    return;
  }
  mCoeffCutoff = cutoff;

  // A one pole low pass whose coefficient is the usual approximation of the cutoff, clamped to
  // the stable range.
  const auto nyquist = static_cast<float>(0.5 * sampleRate);
  const auto clampedFreq = std::clamp(cutoff, 10.0f, nyquist);
  const auto coefficient = static_cast<float>(2.0 * std::sin(M_PI * clampedFreq / sampleRate));
  mFilterCoeff = std::clamp(coefficient, 0.0f, 1.0f);
}

void AudioUnitDelayImpl::renderBlock(const AudioBufferList* const* inputs,
  const uint32_t numInputs,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& /* when */) {
  // The render buffer is silent and holds `numFrames` valid frames before this call, so a
  // block that writes nothing renders as silence.
  if (outputData == nullptr || numFrames == 0) {
    return;
  }

  const auto* input = numInputs > 0 ? inputs[0] : nullptr;
  if (input == nullptr || input->numberOfBuffers() == 0) {
    return;
  }

  const auto channels =
    static_cast<uint32_t>(std::min(input->numberOfBuffers(), outputData->numberOfBuffers()));
  if (channels == 0) {
    return;
  }

  // A bypassed effect, and an effect that has no delay line yet, pass the input through.
  if (isBypass() || mDelayLines.empty()) {
    (void) AudioGraphUtils::copyFloatBufferList(*input, *outputData, numFrames);
    return;
  }

  updateLowPassCutoff(mSampleRate);

  const auto size = static_cast<int>(mDelayLines[0].size());
  if (size <= 0) {
    return;
  }
  // The delay is derived from the property on every block, so changing it while the graph runs
  // takes effect at the next block without touching the delay lines.
  const auto delaySamples =
    std::clamp(static_cast<int>(mSampleRate * static_cast<Float64>(getDelayTime())), 0, size - 1);
  const auto mix = getWetDryMix() * 0.01f;
  const auto feedbackAmount = getFeedback() * 0.01f;
  const auto coefficient = mFilterCoeff;

  // Every channel shares one write position, so the position is advanced once per frame, after
  // all of the channels have written their frame. The buffers of a list are addressed in
  // constant time, which keeps this allocation free on a render thread.
  for (AudioFrameCount f = 0; f < numFrames; ++f) {
    const auto readIndex = (mWriteIndex - delaySamples + size) % size;
    for (uint32_t c = 0; c < channels; ++c) {
      const auto* source = static_cast<const float*>(input->getBuffer(c)->mData);
      auto* output = static_cast<float*>(outputData->getBuffer(c)->mData);
      if (source == nullptr || output == nullptr) {
        continue;
      }
      auto& line = mDelayLines[c];
      auto& lastFiltered = mLastFilteredSamples[c];
      const auto sample = source[f];
      // A delay of zero has nothing to read yet, so the delayed signal is the input itself.
      const auto delayed = delaySamples == 0 ? sample : line[readIndex];

      output[f] = sample * (1.0f - mix) + delayed * mix;

      // The feedback runs through the filter, which is what gives the effect its tape
      // character, and the line is clamped so a large feedback cannot blow up.
      const auto feedbackSignal = delayed * feedbackAmount;
      lastFiltered += coefficient * (feedbackSignal - lastFiltered);
      line[mWriteIndex] = std::clamp(sample + lastFiltered, -1.0f, 1.0f);
    }
    mWriteIndex = (mWriteIndex + 1) % size;
  }
}

} // namespace coral
