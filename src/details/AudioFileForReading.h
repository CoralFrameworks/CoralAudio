/*
 * Copyright (C) 2024 The Coral Project
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

#ifndef CORAL_AUDIO_FILE_FOR_READING_H
#define CORAL_AUDIO_FILE_FOR_READING_H

#include <codec/AudioFileReader.h>

#include "AudioFileBase.h"
#include "SampleFormatConverter.h"

#include <memory>
#include <string>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioFile for reading from audio file.
 */
class AudioFileForReading final : public AudioFileBase {
public:
  static std::unique_ptr<AudioFileBase>
  CreateWithUrl(const std::string& fileUrl, Error** outError);

  static std::unique_ptr<AudioFileBase>
  CreateWithUrl(const std::string& fileUrl, AudioSampleFormat sampleFormat, bool interleaved, Error** outError);

  explicit AudioFileForReading(std::string inputUrl, AudioSampleFormat sampleFormat, bool interleaved);

  ~AudioFileForReading() override;

  void release() override;

  bool readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) override;

  bool readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& pcmBuffer, uint32_t frames, Error** outError) override;

  bool writeFromBuffer(const std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) override;

  [[nodiscard]] std::string getFileUrl() const override {
    return mInputUrl;
  }

  [[nodiscard]] std::optional<AudioFormat> getFileFormat() const override {
    return mFileFormat;
  }

  [[nodiscard]] std::optional<AudioFormat> getProcessingFormat() const override {
    return mProcessFormat;
  }

  [[nodiscard]] AudioFramePosition getLength() const override;

  [[nodiscard]] AudioFramePosition getFramePosition() const override;

  void setFramePosition(AudioFramePosition framePosition) override;

private:
  bool prepare(Error** outError);

  static void handleError(const std::string& tag, Error** outError, std::string errorMsg);

  static const char* getName() {
    return "AudioFileForReading";
  }

  std::string mInputUrl;
  std::unique_ptr<AudioFileReader> mFileReader;
  AudioSampleFormat mOutputSampleFormat;
  bool mInterleaved;
  std::optional<AudioFormat> mFileFormat;
  std::optional<AudioFormat> mProcessFormat;
  std::vector<uint8_t> mDecodedBuffer;
  std::unique_ptr<SampleFormatConverter> mFormatConverter;
};

} // namespace coral

#endif // CORAL_AUDIO_FILE_FOR_READING_H
