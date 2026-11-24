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

#ifndef CORAL_AUDIO_UNIT_DELAY_H
#define CORAL_AUDIO_UNIT_DELAY_H

#include <coral/audio/AudioUnitEffect.h>

namespace coral {

/**
 * An AudioUnitEffect that implements a delay effect
 *
 * A delay unit delays the input signal by the specified time interval
 * and then blends it with the input signal. The amount of high frequency
 * roll-off can also be controlled in order to simulate the effect of
 * a tape delay.
 */
class AudioUnitDelay : public AudioUnitEffect {
public:
  AudioUnitDelay();
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
   * Returns the delay in seconds.
   */
  [[nodiscard]] uint64_t getDelayTime() const;

  /**
   * Sets the delay in seconds.
   *
   * @param newDelayTime the new delay seconds to set.
   */
  void setDelayTime(uint64_t newDelayTime);

  /**
   * Returns the amount of the output signal fed back into the delay line.
   */
  [[nodiscard]] float getFeedback() const;

  /**
   * Sets the amount of the output signal fed back into the delay line.
   *
   * @param newFeedback the amount of the output signal fed back into the delay line.
   */
  void setFeedback(float newFeedback);

  /**
   * Returns cutoff frequency above which high frequency content is rolled off.
   */
  [[nodiscard]] float getLowPassCutoff() const;

  /**
   * Sets cutoff frequency above which high frequency content is rolled off.
   *
   * @param newLowPassCutoff the specified cutoff frequency.
   */
  void setLowPassCutoff(float newLowPassCutoff);

  /**
   * Returns the blend of the wet and dry signals, in percent.
   */
  [[nodiscard]] float getWetDryMix() const;

  /**
   * Sets the blend of the wet and dry signals, in percent.
   *
   * The value is clamped to the valid range of 0 through 100, where 0 represents all dry.
   *
   * @param newWetDryMix the blend of the wet and dry signals, in percent.
   */
  void setWetDryMix(float newWetDryMix);

};

} // namespace coral

#endif // CORAL_AUDIO_UNIT_DELAY_H
