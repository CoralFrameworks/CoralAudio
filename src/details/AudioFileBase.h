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

#ifndef CORAL_AUDIO_FILE_BASE_H
#define CORAL_AUDIO_FILE_BASE_H

#include <coral/audio/AudioBase.h>

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioTypes.h>

#include <optional>

namespace coral {

class AudioFileBase : public AudioNoncopyable {
public:
  virtual void release() = 0;

  virtual bool readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) = 0;

  virtual bool readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& pcmBuffer, uint32_t frames, Error** outError) = 0;

  virtual bool writeFromBuffer(const std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) = 0;

  virtual std::string getFileUrl() const = 0;

  virtual std::optional<AudioFormat> getFileFormat() const = 0;

  virtual std::optional<AudioFormat> getProcessingFormat() const = 0;

  virtual AudioFramePosition getLength() const = 0;

  virtual AudioFramePosition getFramePosition() const = 0;

  virtual void setFramePosition(AudioFramePosition framePosition) = 0;
};

} // namespace coral

#endif // CORAL_AUDIO_FILE_BASE_H
