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

#ifndef CORAL_AUDIO_GRAPH_REVERB_H
#define CORAL_AUDIO_GRAPH_REVERB_H

#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioUnitReverb.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioUnitReverb that implements a reverb
 *
 * @discussion
 * The network is a Freeverb-style reverb: eight parallel comb filters with a damped feedback
 * per channel, followed by four series allpass filters, plus a short pre-delay. The two
 * channels use slightly different delay lengths, which is what turns a mono input into a
 * stereo tail. A factory preset selects the room size (the comb feedback), the damping, and
 * the pre-delay, and `wetDryMix` blends the tail with the dry signal.
 *
 * Apple does not publish the parameters of its reverb presets, so the presets reproduce the
 * size and brightness their names describe, they are not copies of Apple's algorithm.
 *
 * All the delay lines are allocated when the node is prepared, so `renderBlock` never
 * allocates and can run on a render thread.
 */
class AudioUnitReverbImpl final : public AudioNodeImpl {
public:
  using ReverbPreset = AudioUnitReverb::ReverbPreset;

  /** The tunings of a preset. */
  struct Settings {
    float roomSize = 0.72f;
    float damping = 0.5f;
    float preDelaySeconds = 0.012f;
    float wetGain = 2.5f;
  };

  /**
   * Selects the room size, damping and pre-delay of a factory preset.
   */
  void loadFactoryPreset(ReverbPreset preset);

  [[nodiscard]] bool isBypass() const {
    return mBypass.load(std::memory_order_relaxed);
  }

  void setBypass(const bool newBypass) {
    mBypass.store(newBypass, std::memory_order_relaxed);
  }

  /**
   * Returns the blend of the wet and dry signals, in percent.
   */
  [[nodiscard]] float getWetDryMix() const {
    return mWetDryMix.load(std::memory_order_relaxed);
  }

  /**
   * Sets the blend of the wet and dry signals, in percent, clamped to [0, 100].
   */
  void setWetDryMix(float wetDryMix);

  AudioUnitReverbImpl* asReverbImpl() override {
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
  /** A damped parallel comb filter. */
  struct CombFilter {
    std::vector<float> buffer;
    size_t index = 0;
    float filterStore = 0.0f;
    float feedback = 0.84f;
    float damp1 = 0.2f;
    float damp2 = 0.8f;

    [[nodiscard]] float process(float input);
    void clear();
  };

  /** A series allpass diffuser. */
  struct AllpassFilter {
    std::vector<float> buffer;
    size_t index = 0;

    [[nodiscard]] float process(float input);
    void clear();
  };

  /** The delay lines and filters of one output channel. */
  struct ChannelState {
    std::array<CombFilter, 8> combs;
    std::array<AllpassFilter, 4> allpasses;
    std::vector<float> preDelay;
    size_t preDelayIndex = 0;
  };

  /**
   * Sizes the delay lines of the format the node renders in.
   */
  void initWithInputFormat(const AudioFormat& format);

  /** Applies the delay tunings of `channel` to its filters. */
  void configureChannel(ChannelState& channel, size_t channelIndex);

  /** Applies the room size and damping of `mSettings` to every comb. */
  void updateCombSettings();

  /** Clears the tail of every channel, e.g. when the preset changed. */
  void clearTail();

  /**
   * The bypass state of the audio unit.
   *
   * If true, the audio unit bypasses audio processing.
   */
  std::atomic_bool mBypass{ false };

  /**
   * The blend of the wet and dry signals, in percent.
   *
   * The default value is 100%. The valid range is 0% through 100%, where 0% represents all
   * dry.
   */
  std::atomic<float> mWetDryMix{ 100.0f };

  //!< The preset the client selected, and the one the render side configured itself for.
  std::atomic<ReverbPreset> mPreset{ ReverbPreset::kMediumHall };
  ReverbPreset mRenderedPreset = ReverbPreset::kMediumHall;
  Settings mSettings;

  //!< The render-side state below is owned by the render thread.
  std::vector<ChannelState> mChannelStates;
  std::vector<const float*> mInputPointers;
  std::vector<float*> mOutputPointers;
  size_t mChannels = 0;
  Float64 mSampleRate = 0.0;
  //!< Smoothed wet/dry mix, so a property change does not click.
  float mCurrentWetDryMix = 1.0f;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_REVERB_H

