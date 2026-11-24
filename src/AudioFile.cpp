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

#include <coral/audio/AudioFile.h>
#include <coral/audio/AudioBuffer.h>

#include <media/Utilities.h>

#include "details/AudioFileForReading.h"
#include "details/AudioFileForWriting.h"

namespace coral {

std::shared_ptr<AudioFile>
AudioFile::InitForReading(const std::string& fileUrl, Error** outError) {
  auto file = AudioFileForReading::CreateWithUrl(fileUrl, outError);
  if (!file) {
    return nullptr;
  }
  return std::make_shared<AudioFile>(std::move(file));
}

std::shared_ptr<AudioFile>
AudioFile::InitForReading(const std::string& fileUrl,
  const AudioSampleFormat sampleFormat,
  const bool interleaved,
  Error** outError) {
  auto file = AudioFileForReading::CreateWithUrl(fileUrl, sampleFormat, interleaved, outError);
  if (!file) {
    return nullptr;
  }
  return std::make_shared<AudioFile>(std::move(file));
}

std::shared_ptr<AudioFile>
AudioFile::initForWriting(const std::string& fileUrl,
  const MediaFileType& fileType,
  const AudioFormat& format,
  Error** outError) {
  auto file = AudioFileForWriting::CreateWithUrl(fileUrl, fileType, format, outError);
  if (!file) {
    return nullptr;
  }
  return std::make_shared<AudioFile>(std::move(file));
}

AudioFile::AudioFile(std::unique_ptr<AudioFileBase> audioFile) noexcept
  : impl(audioFile.release()) {
}

AudioFile::~AudioFile() {
  delete impl;
}

bool AudioFile::readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) {
  return impl->readIntoBuffer(buffer, outError);
}

bool AudioFile::readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, const uint32_t frames, Error** outError) {
  return impl->readIntoBuffer(buffer, frames, outError);
}

bool AudioFile::writeFromBuffer(const std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) {
  return impl->writeFromBuffer(buffer, outError);
}

std::string AudioFile::getFileUrl() const {
  return impl->getFileUrl();
}

std::optional<AudioFormat> AudioFile::getFileFormat() const {
  return impl->getFileFormat();
}

std::optional<AudioFormat> AudioFile::getProcessingFormat() const {
  return impl->getProcessingFormat();
}

AudioFramePosition AudioFile::getLength() const {
  return impl->getLength();
}

AudioFramePosition AudioFile::getFramePosition() const {
  return impl->getFramePosition();
}

void AudioFile::setFramePosition(const AudioFramePosition framePosition) {
  impl->setFramePosition(framePosition);
}

void AudioFile::release() {
  impl->release();
}

} // namespace coral
