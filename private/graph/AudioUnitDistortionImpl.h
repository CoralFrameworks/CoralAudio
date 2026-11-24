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

#ifndef CORAL_AUDIO_GRAPH_AUDIO_DISTORTION_IMPL_H
#define CORAL_AUDIO_GRAPH_AUDIO_DISTORTION_IMPL_H

#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioUnitDistortion.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioUnitEffect that implements a multi-stage distortion effect.
 *
 * @discussion
 * The signal path follows the stages of Apple's distortion audio unit: the pre-gain, a
 * non-linear shaper, an optional bit-depth and sample-rate decimator, a tone filter, an echo,
 * and a ring modulator. `loadFactoryPreset` chooses which stages run and with which settings,
 * while `preGain` and `wetDryMix` stay live on top of the preset, and the distorted signal is
 * blended with the untouched dry signal.
 *
 * The echo lines and all of the per-channel state are allocated when the node is prepared, so
 * `renderBlock` does not allocate and can run on a render thread.
 *
 * @note
 * Apple does not document the internal settings of its factory presets. The presets here
 * reproduce their character from the documented stages, they are not bit-for-bit copies.
 */
class AudioUnitDistortionImpl final : public AudioNodeImpl {
public:
  using DistortionPreset = AudioUnitDistortion::DistortionPreset;

  /**
   * Selects the stages and settings of a factory preset.
   */
  void loadFactoryPreset(DistortionPreset preset);

  [[nodiscard]] bool isBypass() const {
    return mBypass.load(std::memory_order_relaxed);
  }

  void setBypass(const bool newBypass) {
    mBypass.store(newBypass, std::memory_order_relaxed);
  }

  /**
   * Returns the gain before distortion, in decibels.
   */
  [[nodiscard]] float getPreGain() const {
    return mPreGain.load(std::memory_order_relaxed);
  }

  /**
   * Sets the gain before distortion, in decibels, clamped to [-80, 20].
   */
  void setPreGain(float newPreGain);

  /**
   * Returns the blend of the distorted and dry signals, in percent.
   */
  [[nodiscard]] float getWetDryMix() const {
    return mWetDryMix.load(std::memory_order_relaxed);
  }

  /**
   * Sets the blend of the distorted and dry signals, in percent, clamped to [0, 100].
   */
  void setWetDryMix(float newWetDryMix);

  AudioUnitDistortionImpl* asDistortionImpl() override {
    return this;
  }

protected:
  void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) override;

  void prepareBeforeRendering() override;

  void onReset() override;

private:
  /** The state of one channel of the distortion. */
  struct ChannelState {
    //!< One-pole low pass, used for the tone stage and as the low half of the high pass.
    float toneLowPass = 0.0f;
    //!< One-pole low pass that feeds the high pass stage.
    float highPassLowPass = 0.0f;
    //!< DC blocker state.
    float dcPreviousInput = 0.0f;
    float dcPreviousOutput = 0.0f;
    //!< Last sample the decimator held.
    float heldSample = 0.0f;
    //!< Phase of the ring modulator, in radians.
    float ringPhase = 0.0f;
    //!< The channel's echo line.
    std::vector<float> echo;
  };

  /**
   * Allocates the per-channel state for the format the node renders in.
   */
  void initWithInputFormat(const AudioFormat& format);

  /**
   * Clears the filters, the echo lines and the modulators, but keeps the allocated memory.
   */
  void resetDspState();

  std::atomic<DistortionPreset> mPreset{ DistortionPreset::kDrumsBitBrush };

  /**
   * The bypass state of the audio unit.
   *
   * If true, the audio unit bypasses audio processing.
   */
  std::atomic_bool mBypass{ false };

  /**
   * The gain that the audio unit applies to the signal before distortion, in decibels.
   *
   * The default value is -6 dB. The valid range of values is -80 dB to 20 dB.
   */
  std::atomic<float> mPreGain{ -6.0f };

  /**
   * The blend of the distorted and dry signals, in percent.
   *
   * The default value is 50%. The valid range is 0% through 100%, where 0 represents all dry.
   */
  std::atomic<float> mWetDryMix{ 50.0f };

  // The render-side state below is owned by the render thread.
  //!< The preset the render side has already configured itself for.
  DistortionPreset mRenderedPreset = DistortionPreset::kDrumsBitBrush;
  //!< One state per channel of the render format.
  std::vector<ChannelState> mChannelStates;
  //!< Per-block views of the input and output planes, so the loop reads them once.
  std::vector<const float*> mInputPointers;
  std::vector<float*> mOutputPointers;
  //!< The number of channels the state was allocated for.
  size_t mChannels = 0;
  //!< The sample rate the state was allocated for.
  Float64 mSampleRate = 0.0;
  //!< The length of one echo line, and the position the channels write together.
  size_t mEchoSize = 0;
  size_t mEchoWriteIndex = 0;
  //!< The decimator's frame counter, shared by the channels to keep them aligned.
  uint32_t mHoldCounter = 0;
  //!< Smoothed copies of the live properties, to avoid zipper noise.
  float mCurrentPreGain = 0.5011872336f; //!< -6 dB, linear.
  float mCurrentWetDryMix = 0.5f;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_AUDIO_DISTORTION_IMPL_H

