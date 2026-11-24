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

#ifndef CORAL_AUDIO_UNIT_TIME_PITCH_H
#define CORAL_AUDIO_UNIT_TIME_PITCH_H

#include <coral/audio/AudioUnitTimeEffect.h>

#include <atomic>
#include <memory>

namespace coral {

/**
 * An AudioUnitTimeEffect that provides good quality time stretching and pitch shifting
 *
 * @discussion
 * In this time effect, the playback rate and pitch parameters function independently of each other
 */
class AudioUnitTimePitch : public AudioUnitTimeEffect {
public:
  AudioUnitTimePitch();
  /**
   * Returns the bypass state of the audio unit.
   */
  [[nodiscard]] bool isBypass() const override;

  /**
   * Sets the bypass state of the audio unit.
   *
   * @param newBypass the new bypass state
   */
  void setBypass(bool newBypass) override;

  /**
   * Returns the amount by which the input signal is pitch shifted.
   *
   * One octave is equal to 1200 cents. One musical semitone is equal to 100
   * cents. The default value is 0.0. The range of values is -2400 to 2400.
   */
  [[nodiscard]] float getPitch() const;

  /**
   * Sets the amount by which the input signal is pitch shifted
   *
   * @param newPitch the amount by which the input signal is pitch shifted
   * The default value is 0.0. The range of values is -2400 to 2400.
   */
  void setPitch(float newPitch);

  /**
   * Returns the playback rate of the input signal.
   *
   * Range:      1/32 -> 32.0
   * Default:    1.0
   * Unit:       Generic
   */
  [[nodiscard]] float getRate() const;

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
  [[nodiscard]] float getOverlap() const;

  /**
   * Sets the amount of overlap between segments of the input audio signal
   *
   * @discussion
   * This property may be changed while the graph is rendering. Because it changes the
   * overlap-add grid, the unit fades its output out, rebuilds the grid, and fades back in
   * instead of splicing the waveform. Rate and pitch changes are smoothed in the same way.
   * The unit has the processing latency of its analysis window.
   *
   * @param newOverlap the overlap to set. The range of overlap is [3.0, 32.0].
   */
  void setOverlap(float newOverlap);
};

} // namespace coral

#endif // CORAL_AUDIO_UNIT_TIME_PITCH_H
