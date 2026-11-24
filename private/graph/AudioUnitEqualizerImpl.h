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

#ifndef CORAL_AUDIO_GRAPH_EQUALIZER_H
#define CORAL_AUDIO_GRAPH_EQUALIZER_H

#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioUnitEqualizer.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioUnitEqualizer that implements a Multi-Band Equalizer.
 *
 * @discussion
 * Every band is a second-order biquad filter, and the bands run in series per channel, which
 * is what an AVAudioUnitEQ does. A band uses the filter type, frequency, bandwidth and gain
 * AVFoundation documents, including the per-band bypass, and the result passes through the
 * global gain of the unit.
 *
 * The client changes the bands through the node, and the render thread takes a snapshot of
 * them only when they changed, so changing a band while the graph renders does not require a
 * lock on every block.
 */
class AudioUnitEqualizerImpl final : public AudioNodeImpl {
public:
  using FilterParameters = AudioUnitEqualizer::FilterParameters;
  using FilterType = AudioUnitEqualizer::FilterType;

  explicit AudioUnitEqualizerImpl(uint32_t numberOfBands) noexcept;

  [[nodiscard]] bool isBypass() const {
    return mBypass.load(std::memory_order_relaxed);
  }

  void setBypass(bool newBypass);

  [[nodiscard]] uint32_t getNumberOfBands() const {
    return mNumberOfBands;
  }

  [[nodiscard]] std::vector<FilterParameters> getBands() const;

  void setBandParameters(uint32_t band, const FilterParameters& parameters);

  void setBands(const std::vector<FilterParameters>& bands);

  [[nodiscard]] float getGlobalGain() const {
    return mGlobalGain.load(std::memory_order_relaxed);
  }

  void setGlobalGain(float newGlobalGain);

  AudioUnitEqualizerImpl* asEqualizerImpl() override {
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
  /** The coefficients of one biquad, and the state of its channels. */
  struct BandFilter {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    bool active = false;
    //!< Transposed direct form II state, one entry per channel.
    std::vector<float> z1;
    std::vector<float> z2;
  };

  /** Clamps the values of a band to the ranges AVFoundation documents. */
  [[nodiscard]] static FilterParameters sanitize(const FilterParameters& parameters);

  /** Recomputes the coefficients of every band from the render snapshot. */
  void updateCoefficients();

  //!< The number of bands is fixed when the equalizer is created.
  uint32_t mNumberOfBands = 0;

  /** Guards the client-side band array and the version that describes it. */
  mutable std::mutex mBandsLock;
  std::vector<FilterParameters> mBands;
  std::atomic<uint64_t> mBandsVersion{ 1 };

  //!< The render thread's copy of the bands, replaced when `mBandsVersion` changes.
  std::vector<FilterParameters> mRenderBands;
  uint64_t mRenderedVersion = 0;

  //!< One filter per band; the coefficients are shared by the channels.
  std::vector<BandFilter> mFilters;

  //!< Per-block views of the planes, so the loop reads them once.
  std::vector<const float*> mInputPointers;
  std::vector<float*> mOutputPointers;
  size_t mChannels = 0;
  Float64 mSampleRate = 0.0;

  /**
   * The bypass state of the audio unit.
   *
   * If true, the audio unit bypasses audio processing.
   */
  std::atomic_bool mBypass{ false };

  /**
   * The overall gain adjustment that the audio unit applies to the signal,
   * in decibels.
   *
   * The default value is 0 dB. The valid range of values is -96 dB to 24 dB.
   */
  std::atomic<float> mGlobalGain{ 0.0f };

  //!< Smoothed global gain, so changing the property does not click.
  float mCurrentGlobalGain = 1.0f;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_EQUALIZER_H
