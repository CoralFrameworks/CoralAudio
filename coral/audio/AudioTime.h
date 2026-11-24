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

#ifndef CORAL_AUDIO_TIME_H
#define CORAL_AUDIO_TIME_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioTypes.h>

#include <ostream>
#include <sstream>

namespace coral {

/**
 * Represent a moment in time.
 *
 * AudioTime is used in AudioEngine to represent time. Instances are immutable.
 *
 * A single moment in time may be represented in two different ways:
 * 1. the system's basic clock. Commonly referred to as "host time."
 * 2. audio samples at a particular sample rate
 *
 * A single AudioTime instance may contain either or both representations; it
 * might represent only a sample time, only a host time, or both.
 */
class AudioTime final : public AudioCopyable {
public:
  explicit AudioTime(const AudioFramePosition sampleTime, const Float64 sampleRate, uint64_t hostTime) noexcept
    : mSampleTime(sampleTime),
      mSampleRate(sampleRate),
      mHostTime(hostTime) {
  }

  /**
   * Returns whether the sampleTime and sampleRate properties are valid.
   */
  [[nodiscard]] bool isSampleTimeValid() const {
    return mSampleTime >= 0 && mSampleRate > 0;
  }

  /**
   * Returns the time as a number of audio samples, as tracked by the current
   * audio device.
   */
  [[nodiscard]] AudioFramePosition getSampleTime() const {
    return mSampleTime;
  }

  /**
   * Returns the sample rate at which sampleTime is being expressed.
   */
  [[nodiscard]] Float64 getSampleRate() const {
    return mSampleRate;
  }

  /**
   * Returns the host time
   */
  [[nodiscard]] uint64_t getHostTime() const { return mHostTime; }

  /**
   * Indicates whether the host time is valid.
   */
  [[nodiscard]] bool isHostTimeValid() const { return mHostTime != 0; }

  [[nodiscard]] std::string toString() const noexcept {
    std::stringstream out;
    out << "{" << getSampleTime() << ", " << getSampleRate() << "}";
    return out.str();
  }

  friend std::ostream& operator<<(std::ostream& os, const AudioTime& time) {
    return os << "{" << time.getSampleTime() << ", " << time.getSampleRate() << "}";
  }

  friend std::ostream& operator<<(std::ostream& os, const std::optional<AudioTime>& time) {
    if (!time.has_value()) {
      return os << "{}";
    }
    return os << "{" << time->getSampleTime() << ", " << time->getSampleRate() << "}";
  }

private:
  AudioFramePosition mSampleTime;
  Float64 mSampleRate;
  uint64_t mHostTime;
};

} // namespace coral

#endif // CORAL_AUDIO_TIME_H
