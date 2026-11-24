/*
 * Copyright (C) 2026 The Coral Project
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

/**
 * @file AudioStreamBuilder.cpp
 *
 * The macOS implementation of the two platform hooks of `AudioStreamBuilder`.
 *
 * The common `openStream()` in `src/common/AudioStreamBuilder.cpp` validates and owns the shared
 * pointer; these two functions only decide which concrete stream is created and whether it opened.
 * That split is the same one the Windows, Linux and Android builders use.
 */

#include "private/audio/AudioStreamBuilder.h"

#include "AudioStreamCoreAudio.h"

#include <minilog/log.h>

namespace coral {

AudioStream* AudioStreamBuilder::build() const {
  return new AudioStreamCoreAudio(*this);
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

  AudioStream* stream = build();
  if (stream == nullptr) {
    return Result::ErrorNull;
  }

  const Result result = stream->open();
  if (result == Result::OK) {
    *streamPP = stream;
  } else {
    // The stream owns the audio unit and the control thread once open() has started; its destructor
    // closes whichever of them was created before the failure.
    delete stream;
  }
  return result;
}

} // namespace coral
