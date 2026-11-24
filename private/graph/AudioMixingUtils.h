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

#ifndef CORAL_AUDIO_MIXING_UTILS_H
#define CORAL_AUDIO_MIXING_UTILS_H

#include <coral/audio/AudioMixing.h>

#include <memory>

namespace coral {

class AudioEngine;
class AudioNode;

class AudioMixingUtils {
public:
  static std::shared_ptr<AudioMixingDestination>
  destinationForMixer(const std::shared_ptr<AudioEngine>& engine,
    const std::shared_ptr<AudioNode>& sourceNode,
    const std::shared_ptr<AudioNode>& mixerNode,
    AudioNodeBus mixerInputBus);

private:
  AudioMixingUtils() = default;
  ~AudioMixingUtils() = default;
};

} // namespace coral

#endif //CORAL_AUDIO_MIXING_UTILS_H
