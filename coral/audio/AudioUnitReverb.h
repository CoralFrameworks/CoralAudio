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

#ifndef CORAL_AUDIO_UNIT_REVERB_H
#define CORAL_AUDIO_UNIT_REVERB_H

#include <coral/audio/AudioUnitEffect.h>

#include <cstdint>

namespace coral {

/**
 * An AudioUnitEffect that implements a reverb
 *
 * A reverb simulates the acoustic characteristics of a particular environment.
 * Use the different presets to simulate a particular space and blend it in with
 * the original signal using the wetDryMix parameter.
 */
class AudioUnitReverb : public AudioUnitEffect {
public:
  AudioUnitReverb();

  /**
   * Constants that represent preset reverbs.
   *
   * The raw values match `AVAudioUnitReverbPreset`.
   */
  enum class ReverbPreset : uint32_t {
    // clang-format off
    //!< A small-sized room environment.
    kSmallRoom = 0,
    //!< A medium-sized room environment.
    kMediumRoom = 1,
    //!< A large-sized room environment.
    kLargeRoom = 2,
    //!< A medium-sized hall environment.
    kMediumHall = 3,
    //!< A large-sized hall environment.
    kLargeHall = 4,
    //!< A plate environment.
    kPlate = 5,
    //!< A medium-sized chamber environment.
    kMediumChamber = 6,
    //!< A large-sized chamber environment.
    kLargeChamber = 7,
    //!< A cathedral environment.
    kCathedral = 8,
    //!< An alternative large-sized room environment.
    kLargeRoom2 = 9,
    //!< An alternative medium-sized hall environment.
    kMediumHall2 = 10,
    //!< An alternative medium-sized hall environment.
    kMediumHall3 = 11,
    //!< An alternative large-sized hall environment.
    kLargeHall2 = 12,
    // clang-format on
  };

  /**
   * Loads a reverb preset. Default value is ReverbPreset::kMediumHall
   *
   * @param preset the preset to load
   */
  void loadFactoryPreset(ReverbPreset preset);
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
   * Returns the blend of the wet and dry signals, in percent.
   *
   * The default value is 100, and the valid range is 0 through 100, where 0
   * represents all dry.
   */
  [[nodiscard]] float getWetDryMix() const;

  /**
   * Sets the blend of the wet and dry signals, in percent.
   *
   * The value is clamped to the valid range of 0 through 100, where 0
   * represents all dry.
   *
   * @param wetDryMix the blend of the wet and dry signals, in percent.
   */
  void setWetDryMix(float wetDryMix);

};

} // namespace coral

#endif // CORAL_AUDIO_UNIT_REVERB_H
