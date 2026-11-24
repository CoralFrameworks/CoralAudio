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

#ifndef CORAL_AUDIO_FILE_FOR_WRITING_H
#define CORAL_AUDIO_FILE_FOR_WRITING_H

#include <codec/AudioFileWriter.h>

#include "AudioFileBase.h"
#include "SampleFormatConverter.h"

namespace coral {

/**
 * The implementation of the AudioFile for writing audio data to audio file.
 */
class AudioFileForWriting final : public AudioFileBase {
public:
  static std::unique_ptr<AudioFileBase>
  CreateWithUrl(const std::string& fileUrl,
    const MediaFileType& fileType,
    const AudioFormat& audioFormat,
    Error** outError = nullptr);

  explicit AudioFileForWriting(std::string outputUrl, AudioFormat processingFormat) noexcept;

  ~AudioFileForWriting() override;

  bool prepare(Error** outError);

  void release() override;

  bool readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) override;

  bool readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, uint32_t frames, Error** outError) override;

  bool writeFromBuffer(const std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) override;

  [[nodiscard]] std::string getFileUrl() const override { return mOutputUrl; }

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
  static bool formatEqual(const AudioFormat& format1, const AudioFormat& format2);

  static void handleError(const std::string& tag, Error** outError, const std::string& errorMsg);

  static const char* getName() { return "AudioFileForWriting"; }

  static constexpr int kDefaultMaxFrameSize = 4096;

  std::string mOutputUrl;
  std::unique_ptr<AudioFileWriter> mFileWriter;
  AudioFormat mProcessFormat;
  std::optional<AudioFormat> mFileFormat;
  std::unique_ptr<SampleFormatConverter> mInputConverter;
  std::unique_ptr<uint8_t[]> mAppBuffer;
};

} // namespace coral

#endif // CORAL_AUDIO_FILE_FOR_WRITING_H
