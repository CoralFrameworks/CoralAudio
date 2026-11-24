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

#ifndef CORAL_AUDIO_UNIT_H
#define CORAL_AUDIO_UNIT_H

#include <coral/audio/AudioNode.h>

#include "private/graph/AudioNodeImpl.h"

#include <string>

namespace coral {

/**
 * An AudioUnit is an AudioNode implemented by an audio unit. Depending on the
 * type of the audio unit, audio is processed either in real-time or non
 * real-time.
 */
class AudioUnit : public AudioNode {
public:
  /**
   * Returns the manufacturer name of the audio unit.
   */
  [[nodiscard]] std::string getManufacturerName() const {
    return manufacturerName;
  }

  /**
   * Returns the version number of the audio unit.
   */
  [[nodiscard]] int32_t getVersion() const {
    return version;
  }

protected:
  explicit AudioUnit(const std::string& name, std::unique_ptr<AudioNodeImpl> impl) noexcept;

  std::string manufacturerName = "Coral.Audio";
  int32_t version = 1;
};

} // namespace coral

#endif // CORAL_AUDIO_UNIT_H
