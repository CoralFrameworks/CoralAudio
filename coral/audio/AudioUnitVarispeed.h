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

#ifndef CORAL_AUDIO_UNIT_VARI_SPEED_H
#define CORAL_AUDIO_UNIT_VARI_SPEED_H

#include <coral/audio/AudioUnitTimeEffect.h>

namespace coral {

/**
 * An AudioUnitTimeEffect that can be used to control the playback rate
 */
class AudioUnitVarispeed : public AudioUnitTimeEffect {
public:
  AudioUnitVarispeed();
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
   * Returns the playback rate of the audio signal.
   *
   * The default value is 1.0. The range of values is 0.25 to 4.0.
   */
  [[nodiscard]] float getRate() const;

  /**
   * Set the playback rate of the audio signal
   *
   * @param newRate the playback rate
   *
   * @discussion
   * Since this unit resamples the input signal, changing the playback rate also changes the pitch.
   *
   * i.e. changing the rate to 2.0 results in the output audio playing one octave higher.
   * Similarly changing the rate to 0.5, results in the output audio playing one octave lower.
   * 
   * Range:      0.25 -> 4.0
   * Default:    1.0
   * Unit:       Generic
   */
  void setRate(float newRate);
};

} // namespace coral

#endif // CORAL_AUDIO_UNIT_VARI_SPEED_H
