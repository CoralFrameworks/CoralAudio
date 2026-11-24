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

#include "private/audio/AudioStreamBuilder.h"

#include "AudioStreamWasapi.h"

#include <minilog/log.h>

namespace coral {

AudioStream* AudioStreamBuilder::build() const {
  return new AudioStreamWasapi(*this);
}

Result AudioStreamBuilder::openStreamInternal(AudioStream** streamPP) {
  const Result config = isValidConfig();
  if (config != Result::OK) {
    MINILOG_WARN("Invalid config. Error: {}", convertToText(config));
    return config;
  }

  if (streamPP == nullptr) {
    return Result::ErrorNull;
  }
  *streamPP = nullptr;

  AudioStream* streamP = build();
  if (streamP == nullptr) {
    return Result::ErrorNull;
  }

  const Result result = streamP->open();
  if (result == Result::OK) {
    *streamPP = streamP;
  } else {
    // The stream owns the device and the event threads, so it must be destroyed through
    // its own destructor rather than leaked when the open failed.
    delete streamP;
  }
  return result;
}

} // namespace coral
