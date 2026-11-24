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

#ifndef CORAL_PRIVATE_AUDIO_STREAM_FACTORY_H
#define CORAL_PRIVATE_AUDIO_STREAM_FACTORY_H

#include "private/audio/AudioStreamBuilder.h"

namespace coral {

class AudioStreamFactory {
public:
  virtual ~AudioStreamFactory() = default;

  /**
   * Creates an AudioStream with AudioStreamBuilder.
   */
  virtual std::shared_ptr<AudioStream> create(const AudioStreamBuilder& builder) = 0;
};

std::unique_ptr<AudioStreamFactory> CreateBuiltinAudioStreamFactory();

} // namespace coral

#endif //CORAL_PRIVATE_AUDIO_STREAM_FACTORY_H
