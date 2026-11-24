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

#ifndef CORAL_AUDIO_UNIT_EQ_H
#define CORAL_AUDIO_UNIT_EQ_H

#include <coral/audio/AudioUnitEffect.h>

#include <cstdint>
#include <vector>

namespace coral {

/**
 * An AudioUnitEffect that implements a Multi-Band Equalizer.
 */
class AudioUnitEqualizer : public AudioUnitEffect {
public:
  /**
   * Creates an audio unit equalizer object with the specified number of bands.
   *
   * @param numberOfBands The number of bands that the equalizer creates.
   * @return A new AudioUnitEqualizer instance.
   */
  static std::shared_ptr<AudioUnitEqualizer> Create(uint32_t numberOfBands);

  /**
   * Filter types available to use with the filter type property.
   * @constant kFilterTypeParametric
   *     A type that represents a parametric filter that derives from a
   * Butterworth analog prototype.
   * @constant kFilterTypeLowPass
   *     A type that represents a simple Butterworth second-order low-pass
   * filter.
   * @constant kFilterTypeHighPass
   *     A type that represents a simple Butterworth second-order high-pass
   * filter.
   * @constant kFilterTypeResonantLowPass
   *     A type that represents a low-pass filter with resonance support using
   * the bandwidth parameter.
   * @constant kFilterTypeResonantHighPass
   *     A type that represents a high-pass filter with resonance support using
   * the bandwidth parameter.
   * @constant kFilterTypeBandPass
   *     A type that represents a bandpass filter.
   * @constant kFilterTypeBandStop
   *     A type that represents a band-stop filter, also known as a notch
   * filter.
   * @constant kFilterTypeLowShelf
   *     A type that represents a low-shelf filter.
   * @constant kFilterTypeHighShelf
   *     A type that represents a high-shelf filter.
   * @constant kFilterTypeResonantLowShelf
   *     A type that represents a low-shelf filter with resonance support using
   * the bandwidth parameter.
   * @constant kFilterTypeResonantHighShelf
   *     A type that represents a high-shelf filter with resonance support using
   * the bandwidth parameter.
   */
  enum class FilterType {
    kFilterTypeParametric = 0,
    kFilterTypeLowPass,
    kFilterTypeHighPass,
    kFilterTypeResonantLowPass,
    kFilterTypeResonantHighPass,
    kFilterTypeBandPass,
    kFilterTypeBandStop,
    kFilterTypeLowShelf,
    kFilterTypeHighShelf,
    kFilterTypeResonantLowShelf,
    kFilterTypeResonantHighShelf
  };

  /**
   * An object that encapsulates the parameters that the equalizer uses.
   *
   * Note:
   * Don’t create an instance of the FilterParameters class directly. Use the
   * array that returns from the bands property of AudioUnitEqualizer.
   */
  struct FilterParameters {
    /**
     * The bandwidth of the equalizer filter, in octaves.
     *
     * The value range of values is 0.05 to 5.0 octaves.
     *
     * The default value is 1.0 octave.
     */
    float bandwidth = 1.0f;
    /**
     * The bypass state of the equalizer filter band.
     *
     * true if the bypass is active; otherwise, false. A band is bypassed by
     * default, as AVFoundation defines it.
     */
    bool bypass = true;
    /**
     * The equalizer filter type.
     *
     * The default value is FilterType::kFilterTypeParametric.
     */
    FilterType filterType = FilterType::kFilterTypeParametric;
    /**
     * The frequency of the equalizer filter, in hertz.
     *
     * The valid range of values is 20 Hz through (SampleRate/2).
     *
     * The default value is 1000 Hz.
     */
    float frequency = 1000.0f;
    /**
     * The gain of the equalizer filter, in decibels.
     *
     * The default value is 0 dB. The valid range of values is -96 dB through 24
     * dB.
     */
    float gain = 0.0f;
  };

  [[nodiscard]] bool isBypass() const override;

  void setBypass(bool newBypass) override;

  /**
   * Returns the number of bands the equalizer was created with.
   */
  [[nodiscard]] uint32_t getNumberOfBands() const;

  /**
   * Returns a snapshot of the array of equalizer filter parameters.
   *
   * @discussion
   * The returned array is a copy. Change a band with `setBandParameters` or replace the
   * whole array with `setBands` so a change made while the graph renders is picked up
   * safely.
   */
  [[nodiscard]] std::vector<FilterParameters> getBands() const;

  /**
   * Replaces the parameters of one equalizer band.
   *
   * @param band the index of the band, from 0 to `getNumberOfBands() - 1`
   * @param parameters the new parameters of the band
   */
  void setBandParameters(uint32_t band, const FilterParameters& parameters);

  /**
   * Replaces the parameters of the equalizer bands.
   *
   * @param bands the new band parameters; bands beyond `getNumberOfBands()` are ignored
   */
  void setBands(const std::vector<FilterParameters>& bands);

  /**
   * Returns the overall gain adjustment applied to the signal.
   *
   * The default value is 0 db. The valid range of values is -96 db to 24 db.
   */
  [[nodiscard]] float getGlobalGain() const;
  /**
   * Sets the overall gain adjustment applied to the signal.
   *
   * @param newGlobalGain the specified overall gain
   */
  void setGlobalGain(float newGlobalGain);

protected:
  explicit AudioUnitEqualizer(uint32_t numberOfBands);

};

} // namespace coral

#endif // CORAL_AUDIO_UNIT_EQ_H
