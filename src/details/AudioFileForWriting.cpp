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

#include "AudioFileForWriting.h"

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioUtils.h>

#include <media/Utilities.h>
#include <minilog/log.h>
#include <utils/Path.h>

#include <utility>

using namespace utils;

namespace coral {

std::unique_ptr<AudioFileBase>
AudioFileForWriting::CreateWithUrl(const std::string& fileUrl, const MediaFileType& fileType,
  const AudioFormat& audioFormat, Error** outError) {
  if (Path(fileUrl).exists()) {
    handleError(getName(), outError, "The file is already exist!");
    return nullptr;
  }

  if (audioFormat.getSampleFormat() == AudioSampleFormat::kInvalid ||
      audioFormat.getSampleFormat() == AudioSampleFormat::kDefault) {
    handleError(getName(), outError, "The pcm format is unsupported");
    return nullptr;
  }

  auto AudioFormatType = MediaUtils::getDefaultAudioFormatType(fileType);
  if (AudioFormatType == AudioFormatType::kAudioFormatTypeInvalid) {
    AudioFormatType = audioFormat.getFormatType();
  }

  // handle pcm data format to write
  auto sampleFormat = audioFormat.getSampleFormat();
  if (sampleFormat == AudioSampleFormat::kInvalid ||
      sampleFormat == AudioSampleFormat::kDefault) {
    sampleFormat = AudioSampleFormat::kInt16;
  }

  // if we are writing data to wav file, the audio format type must be linear pcm
  // and the data format only support 16bit and 24bit
  if (fileType == MediaFileType::kMediaFileTypeWav) {
    AudioFormatType = AudioFormatType::kAudioFormatTypeLinearPCM;
    if (sampleFormat != AudioSampleFormat::kInt24
        && sampleFormat != AudioSampleFormat::kInt16) {
      std::string str("The pcm format is unsupported for wav: ");
      str.append(convertToText(sampleFormat));
      handleError(getName(), outError, str);
      return nullptr;
    }
  }

  const auto mimeType = MediaUtils::convertToMimeType(AudioFormatType);
  const auto format =
    MediaFormat::CreateAudioFormat(mimeType, audioFormat.getSampleRate(), audioFormat.getChannelCount());
  format->setAudioFormatType(AudioFormatType);
  format->setSampleFormat(sampleFormat);

  auto fileWriter = AudioFileWriter::CreateWithUrl(fileUrl, fileType, format);
  if (!fileWriter) {
    handleError(getName(), outError, "The file is not supported for writing!");
    return nullptr;
  }

  // create and return a new AudioFileWriting instance
  auto fileWriting = std::make_unique<AudioFileForWriting>(fileUrl, audioFormat);
  fileWriting->mFileWriter = std::move(fileWriter);
  if (fileWriting->prepare(outError)) {
    return fileWriting;
  }
  return nullptr;
}

AudioFileForWriting::AudioFileForWriting(std::string outputUrl, AudioFormat processingFormat) noexcept
  : mOutputUrl(std::move(outputUrl)),
    mProcessFormat(std::move(processingFormat)) {
}

AudioFileForWriting::~AudioFileForWriting() {
  release();
}

void AudioFileForWriting::release() {
  if (mFileWriter) {
    mFileWriter->close();
  }
  mFileFormat.reset();
  mFileWriter.reset();
}

bool AudioFileForWriting::prepare(Error** outError) {
  if (!mFileWriter) {
    return false;
  }
  auto result = mFileWriter->open(outError);
  if (!result) {
    return false;
  }
  mFileFormat = AudioFormat::InitWithCommonFormat(
    mFileWriter->getSampleFormat(), mFileWriter->getSampleRate(), mFileWriter->getChannelCount(), true);
  auto bufferSize = kDefaultMaxFrameSize * mProcessFormat.getBytesPerFrame();
  if (mProcessFormat.getSampleFormat() != mFileFormat->getSampleFormat()) {
    mInputConverter = std::make_unique<SampleFormatConverter>(
      bufferSize, mProcessFormat.getSampleFormat(), mFileFormat->getSampleFormat());
  }
  mAppBuffer = std::make_unique<uint8_t[]>(bufferSize);
  return true;
}

bool AudioFileForWriting::readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) {
  handleError(getName(), outError, "Unsupported reading into buffer!");
  return false;
}

bool AudioFileForWriting::readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, uint32_t frames,
  Error** outError) {
  handleError(getName(), outError, "Unsupported reading into buffer!");
  return false;
}

bool AudioFileForWriting::writeFromBuffer(const std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError) {
  if (!formatEqual(buffer->getFormat().value(), mProcessFormat)) {
    handleError(getName(), outError,
      "the format of the AudioPCMBuffer does not match the file's "
      "processing format");
    return false;
  }

  auto numFrames = buffer->getFrameLength();
  auto channelCount = buffer->getFormat()->getChannelCount();

  void* outputBuffer = nullptr;
  if (buffer->getFormat()->isInterleaved()) {
    // checks if we need to change the audio data format
    if (mInputConverter != nullptr) {
      mInputConverter->convertToInternalOutput(numFrames * channelCount, buffer->rawData());
      outputBuffer = mInputConverter->getOutputBuffer();
    } else {
      outputBuffer = buffer->rawData();
    }
  } else {
    numFrames = AudioUtils::copyFromPCMBuffer(mAppBuffer.get(), numFrames, buffer);

    // checks if we need to change the audio data format
    if (mInputConverter != nullptr) {
      mInputConverter->convertToInternalOutput(numFrames * channelCount, mAppBuffer.get());
      outputBuffer = mInputConverter->getOutputBuffer();
    } else {
      outputBuffer = mAppBuffer.get();
    }
  }

  // write audio pcm data to the file
  auto result = mFileWriter->writeFromPCMBuffer(outputBuffer, numFrames, outError);
  if (result < 0) {
    handleError(getName(), outError, "Failed to write audio pcm data to the file");
    return false;
  }
  return true;
}

AudioFramePosition AudioFileForWriting::getLength() const {
  if (mFileWriter != nullptr) {
    return mFileWriter->getSampleLength();
  }
  return 0;
}

AudioFramePosition AudioFileForWriting::getFramePosition() const {
  if (mFileWriter != nullptr) {
    return mFileWriter->getFramePosition();
  }
  return 0;
}

void AudioFileForWriting::setFramePosition(AudioFramePosition framePosition) {
  handleError(getName(), nullptr, "Unsupported setting frame position when writing!");
}

bool AudioFileForWriting::formatEqual(const AudioFormat& format1, const AudioFormat& format2) {
  return format1.getSampleRate() == format2.getSampleRate() &&
         format1.getChannelCount() == format2.getChannelCount() &&
         format1.getSampleFormat() == format2.getSampleFormat();
}

void AudioFileForWriting::handleError(const std::string& tag, Error** outError, const std::string& errorMsg) {
  if (!outError) {
    MINILOG_ERROR("{}: {}", tag, errorMsg);
    return;
  }
  if (*outError == nullptr) {
    *outError = new Error();
  }
  (*outError)->failureReason = errorMsg;
#ifndef NDEBUG
  MINILOG_ERROR("{}: {}", tag, errorMsg);
#endif
}

} // namespace coral
