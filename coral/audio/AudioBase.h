/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_AUDIO_BASE_H
#define CORAL_AUDIO_BASE_H

namespace coral {

class AudioNoncopyable {
public:
  AudioNoncopyable(const AudioNoncopyable&) = delete;
  AudioNoncopyable& operator=(const AudioNoncopyable&) = delete;

  AudioNoncopyable(AudioNoncopyable&&) = delete;
  AudioNoncopyable& operator=(AudioNoncopyable&&) = delete;

protected:
  AudioNoncopyable() = default;
  virtual ~AudioNoncopyable() = default;
};

class AudioCopyable {
public:
  AudioCopyable(const AudioCopyable &) = default;
  AudioCopyable& operator=(const AudioCopyable &) = default;

  AudioCopyable(AudioCopyable &&) = default;
  AudioCopyable& operator=(AudioCopyable &&) = default;

protected:
  AudioCopyable() = default;
  virtual ~AudioCopyable() = default;
};

} // namespace coral

#endif //CORAL_AUDIO_BASE_H
