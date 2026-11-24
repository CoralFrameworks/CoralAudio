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

#ifndef CORAL_AUDIO_GRAPH_TIME_PITCH_H
#define CORAL_AUDIO_GRAPH_TIME_PITCH_H

#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioUnitTimePitch.h>

#include <atomic>
#include <memory>

namespace coral {

/**
 * The implementation of the AudioUnitTimeEffect that provides good quality time stretching and
 * pitch shifting.
 *
 * @discussion
 * The unit is a phase vocoder: the input is analyzed in overlapping Hann windows, the phase of
 * every bin is propagated by the rate the unit has to play at, and the windows are overlap-added
 * at the pitch-shifted rate. The `overlap` property is the number of analysis windows that cover
 * one window, i.e. the FFT hop is `windowSize / overlap`, which is what AVFoundation documents:
 * a larger overlap means fewer artifacts at a higher CPU cost. Pitch shifting is a resampler
 * after the vocoder, so rate and pitch stay independent.
 *
 * The node pulls more input than it renders when the rate is above 1.0 and less when it is below,
 * through `inputFramesForBlock`, and holds the difference in its FIFOs. All buffers are
 * allocated when the node is prepared, so `renderBlock` does not allocate.
 *
 * Internal use only.
 */
class AudioUnitTimePitchImpl final : public AudioNodeImpl {
public:
  AudioUnitTimePitchImpl();

  ~AudioUnitTimePitchImpl() override;

  /**
   * Returns the bypass state of the audio unit.
   */
  [[nodiscard]] bool isBypass() const {
    return bypass.load(std::memory_order_relaxed);
  }

  /**
   * Sets the bypass state of the audio unit.
   *
   * @param newBypass the new bypass state
   */
  void setBypass(const bool newBypass) {
    bypass.store(newBypass, std::memory_order_relaxed);
  }

  /**
   * Returns the amount by which the input signal is pitch shifted.
   *
   * One octave is equal to 1200 cents. One musical semitone is equal to 100
   * cents. The default value is 0.0. The range of values is -2400 to 2400.
   */
  [[nodiscard]] float getPitch() const {
    return pitch.load(std::memory_order_relaxed);
  }

  /**
   * Sets the amount by which the input signal is pitch shifted
   *
   * @param newPitch the amount by which the input signal is pitch shifted
   */
  void setPitch(float newPitch);

  /**
   * Returns the playback rate of the input signal.
   *
   * Range:      1/32 -> 32.0
   * Default:    1.0
   * Unit:       Generic
   */
  [[nodiscard]] float getRate() const {
    return rate.load(std::memory_order_relaxed);
  }

  /**
   * Sets the playback rate of the input signal.
   *
   * @param newRate The playback rate of the input signal. The range of the rate is 1/32 -> 32.0
   */
  void setRate(float newRate);

  /**
   * Returns the amount of overlap between segments of the input audio signal.
   *
   * @discussion
   * A higher value results in fewer artifacts in the output signal.
   * This parameter also impacts the amount of CPU used.
   *
   * Range:      3.0 -> 32.0
   * Default:    8.0
   * Unit:       Generic
   */
  [[nodiscard]] float getOverlap() const {
    return overlap.load(std::memory_order_relaxed);
  }

  /**
   * Sets the amount of overlap between segments of the input audio signal
   *
   * @param newOverlap the overlap to set. The range of overlap is [3.0, 32.0].
   */
  void setOverlap(float newOverlap);

  AudioUnitTimePitchImpl* asTimePitchImpl() override {
    return this;
  }

  void prepareBeforeRendering() override;

  /**
   * The number of input frames the unit pulls for `frames` output frames.
   *
   * @discussion A playback rate of R produces frames / R output frames from one second of input,
   * so a block of output consumes `frames * R` input frames. The extra frames the analysis window
   * needs on top are only requested while its FIFO has not been primed.
   */
  [[nodiscard]] AudioFrameCount inputFramesForBlock(AudioFrameCount frames) const override;

  /**
   * The largest number of input frames the unit may ask for.
   *
   * This is the block times the largest rate the API allows, plus one analysis window and the
   * priming margin the first block asks for. The engine sizes the upstream buffers with it.
   */
  [[nodiscard]] AudioFrameCount maxInputFramesForBlock(AudioFrameCount frames) const override;

protected:
  void onReset() override;

  void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) override;

private:
  class Impl;

  std::unique_ptr<Impl> mImpl;

  /**
   * The bypass state of the audio unit.
   *
   * If true, the audio unit bypasses audio processing.
   */
  std::atomic_bool bypass{ false };

  //!< Pitch shift, in cents.
  std::atomic<float> pitch{ 0.0f };
  //!< Playback rate.
  std::atomic<float> rate{ 1.0f };
  //!< Analysis window overlap, 3.0 to 32.0.
  std::atomic<float> overlap{ 8.0f };
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_TIME_PITCH_H
