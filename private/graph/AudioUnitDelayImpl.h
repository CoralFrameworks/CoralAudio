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

#ifndef CORAL_AUDIO_GRAPH_AUDIO_DELAY_IMPL_H
#define CORAL_AUDIO_GRAPH_AUDIO_DELAY_IMPL_H

#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioFormat.h>

#include <atomic>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioUnitEffect that implements a delay effect.
 *
 * @discussion
 * Every channel owns a delay line, and the write position advances once per frame for all of
 * them together, so the channels stay aligned. The output blends the delayed signal with the
 * dry signal according to the wet/dry mix, and the delayed signal is fed back through a one
 * pole low pass filter, which is what gives the unit its tape delay character.
 *
 * The delay lines are allocated for the longest delay the API allows (two seconds) when the
 * node is prepared, so changing the delay time at run time never has to allocate on a render
 * thread.
 *
 * Internal use only.
 */
class AudioUnitDelayImpl final : public AudioNodeImpl {
public:
  AudioUnitDelayImpl();

  ~AudioUnitDelayImpl() override;

  /**
   * Whether the effect is bypassed.
   *
   * @discussion `AudioUnitEffect::setBypass` stores into the node, so the processing can only
   * see the change through the flag the node pointed this implementation at.
   */
  [[nodiscard]] bool isBypass() const noexcept {
    return bypass.load(std::memory_order_relaxed);
  }

  /**
   * Points the implementation at the bypass flag of the node that owns it.
   */
  void setBypass(const bool newBypass) noexcept {
    bypass.store(newBypass, std::memory_order_relaxed);
  }

  /**
   * Returns the delay in seconds.
   */
  [[nodiscard]] uint64_t getDelayTime() const {
    return delayTime.load(std::memory_order_relaxed);
  }

  /**
   * Sets the delay in seconds.
   *
   * @param newDelayTime the new delay seconds to set, clamped to the documented range of 0 to 2.
   */
  void setDelayTime(uint64_t newDelayTime);

  /**
   * Returns the amount of the output signal fed back into the delay line.
   */
  [[nodiscard]] float getFeedback() const {
    return feedback.load(std::memory_order_relaxed);
  }

  /**
   * Sets the amount of the output signal fed back into the delay line.
   *
   * @param newFeedback the amount of the output signal fed back into the delay line.
   */
  void setFeedback(float newFeedback);

  /**
   * Returns cutoff frequency above which high frequency content is rolled off.
   */
  [[nodiscard]] float getLowPassCutoff() const {
    return lowPassCutoff.load(std::memory_order_relaxed);
  }

  /**
   * Sets cutoff frequency above which high frequency content is rolled off.
   *
   * @param newLowPassCutoff the specified cutoff frequency.
   */
  void setLowPassCutoff(float newLowPassCutoff);

  /**
   * Returns the blend of the wet and dry signals.
   */
  [[nodiscard]] float getWetDryMix() const {
    return wetDryMix.load(std::memory_order_relaxed);
  }

  /**
   * Sets the blend of the wet and dry signals.
   *
   * @param newWetDryMix the blend of the wet and dry signals.
   */
  void setWetDryMix(float newWetDryMix);

  AudioUnitDelayImpl* asDelayImpl() override {
    return this;
  }

  void prepareBeforeRendering() override;

protected:
  void onReset() override;

  void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) override;

private:
  /** Sizes the delay lines for the longest delay the API allows. */
  void initWithInputFormat(const AudioFormat& format);

  /** Recomputes the one pole coefficient when the cutoff changed. */
  void updateLowPassCutoff(Float64 sampleRate);

  //!< The longest delay the API allows, in seconds.
  static constexpr uint64_t kMaxDelaySeconds = 2;

  /**
   * The bypass state of the audio unit.
   *
   * If true, the audio unit bypasses audio processing.
   */
  std::atomic_bool bypass{ false };

  /**
   * The time for the input signal to reach the output.
   *
   * The default value is 1. The valid range of values is 0 to 2 seconds.
   */
  std::atomic<uint64_t> delayTime{ 1 };

  /**
   * The amount of the output signal that feeds back into the delay line.
   *
   * The default value is 50%. The valid range of values is -100% to 100%.
   */
  std::atomic<float> feedback{ 50.f };

  /**
   * The cutoff frequency above which high frequency content rolls off, in hertz.
   *
   * The default value is 15000 Hz. The valid range of values is 10 Hz through (sampleRate/2).
   */
  std::atomic<float> lowPassCutoff{ 15000.f };

  /**
   * The blend of the wet and dry signals, in percent.
   *
   * The default value is 100%. The valid range of values is 0% through 100%, where 0%
   * represents all dry.
   */
  std::atomic<float> wetDryMix{ 100.0f };

  //!< One delay line per channel.
  std::vector<std::vector<float>> mDelayLines;
  //!< One filter state per channel, so the channels cannot bleed into each other.
  std::vector<float> mLastFilteredSamples;
  //!< The filter state of the cutoff the coefficient was computed from.
  float mCoeffCutoff = 0.f;
  //!< The one pole low pass coefficient of the feedback path.
  float mFilterCoeff = 0.f;
  //!< The sample rate of the delay lines, 0 while the node is not prepared.
  Float64 mSampleRate = 0.0;
  //!< The position written on the next frame, shared by every channel.
  int mWriteIndex = 0;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_AUDIO_DELAY_IMPL_H
