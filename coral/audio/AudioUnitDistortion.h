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

#ifndef CORAL_AUDIO_UNIT_DISTORTION_H
#define CORAL_AUDIO_UNIT_DISTORTION_H

#include <coral/audio/AudioUnitEffect.h>

namespace coral {

/**
 * An AudioUnitEffect that implements a multi-stage distortion effect.
 */
class AudioUnitDistortion : public AudioUnitEffect {
public:
  AudioUnitDistortion();

  /**
   * Constants that represent preset audio distortions.
   *
   * The raw values match `AVAudioUnitDistortionPreset` so a preset can be passed through
   * from a platform layer unchanged.
   */
  enum class DistortionPreset : uint32_t {
    // clang-format off
    //!< A bit brush drums distortion.
    kDrumsBitBrush = 0,
    //!< A buffer beat drums distortion.
    kDrumsBufferBeats = 1,
    //!< A low fidelity drums distortion.
    kDrumsLoFi = 2,
    //!< A broken speaker distortion.
    kMultiBrokenSpeaker = 3,
    //!< A cellphone concert distortion.
    kMultiCellphoneConcert = 4,
    //!< A variant of the decimated distortion.
    kMultiDecimated1 = 5,
    //!< A variant of the decimated distortion.
    kMultiDecimated2 = 6,
    //!< A variant of the decimated distortion.
    kMultiDecimated3 = 7,
    //!< A variant of the decimated distortion.
    kMultiDecimated4 = 8,
    //!< A distorted funk distortion.
    kMultiDistortedFunk = 9,
    //!< A distorted cubed distortion.
    kMultiDistortedCubed = 10,
    //!< A distorted squared distortion.
    kMultiDistortedSquared = 11,
    //!< A variant of an echo distortion.
    kMultiEcho1 = 12,
    //!< A variant of an echo distortion.
    kMultiEcho2 = 13,
    //!< A variant of a tight echo distortion.
    kMultiEchoTight1 = 14,
    //!< A variant of a tight echo distortion.
    kMultiEchoTight2 = 15,
    //!< An everything-is-broken distortion.
    kMultiEverythingIsBroken = 16,
    //!< An alien chatter distortion.
    kSpeechAlienChatter = 17,
    //!< A cosmic interference distortion.
    kSpeechCosmicInterference = 18,
    //!< A golden pi distortion.
    kSpeechGoldenPi = 19,
    //!< A radio tower distortion.
    kSpeechRadioTower = 20,
    //!< A speech wave distortion.
    kSpeechWaves = 21,
    // clang-format on
  };

  /**
   * Configures the audio distortion unit by loading a distortion preset. The
   * default value is DistortionPreset::kDrumsBitBrush
   *
   * @param preset The distortion preset.
   */
  void loadFactoryPreset(DistortionPreset preset);

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
   * Returns the gain applied to the signal before being distorted, in decibels.
   *
   * The default value is -6 dB, and the valid range is -80 dB through 20 dB.
   */
  [[nodiscard]] float getPreGain() const;

  /**
   * Sets the gain applied to the signal before being distorted, in decibels.
   *
   * The value is clamped to the valid range of -80 dB through 20 dB.
   *
   * @param newPreGain the gain in decibels before being distorted.
   */
  void setPreGain(float newPreGain);

  /**
   * Returns the blend of the distorted and dry signals, in percent.
   *
   * The default value is 50, and the valid range is 0 through 100, where 0
   * represents all dry.
   */
  [[nodiscard]] float getWetDryMix() const;

  /**
   * Sets the blend of the distorted and dry signals, in percent.
   *
   * The value is clamped to the valid range of 0 through 100, where 0
   * represents all dry.
   *
   * @param newWetDryMix the blend of the distorted and dry signals, in percent.
   */
  void setWetDryMix(float newWetDryMix);
};

} // namespace coral

#endif // CORAL_AUDIO_UNIT_DISTORTION_H
