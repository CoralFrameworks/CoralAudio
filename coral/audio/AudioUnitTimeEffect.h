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

#ifndef CORAL_AUDIO_UNIT_TIME_EFFECT_H
#define CORAL_AUDIO_UNIT_TIME_EFFECT_H

#include <coral/audio/AudioUnit.h>

namespace coral {

/**
 * An AudioUnit that processes audio in non real-time
 */
class AudioUnitTimeEffect : public AudioUnit {
public:
  /**
   * Returns the bypass state of the audio unit.
   */
  [[nodiscard]] virtual bool isBypass() const = 0;

  /**
   * Sets the bypass state of the audio unit.
   *
   * @param newBypass the new bypass state
   */
  virtual void setBypass(bool newBypass) = 0;

protected:
  using AudioUnit::AudioUnit;
};

} // namespace coral

#endif // CORAL_AUDIO_UNIT_TIME_EFFECT_H
