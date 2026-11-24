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

#include "AudioFileForReading.h"

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioUtils.h>

#include <media/Utilities.h>
#include <minilog/log.h>
#include <utils/Path.h>

#if defined(CORAL_SUPPORTS_FFMPEG)
#include <ffcodec/FfmpegAudioFileReader.h>
#endif

using namespace utils;

namespace coral {

std::unique_ptr<AudioFileBase>
AudioFileForReading::CreateWithUrl(const std::string &fileUrl, Error **outError) {
  return CreateWithUrl(fileUrl, AudioSampleFormat::kFloat, false, outError);
}

std::unique_ptr<AudioFileBase>
AudioFileForReading::CreateWithUrl(const std::string &fileUrl,
    AudioSampleFormat sampleFormat,
    bool interleaved,
    Error **outError) {
  if (!Path(fileUrl).exists()) {
    handleError(getName(), outError, "The file is not exist!");
    return nullptr;
  }

  auto fileReader = AudioFileReader::CreateWithUrl(fileUrl);

#if defined(CORAL_SUPPORTS_FFMPEG)
  if (!fileReader) {
    fileReader = FfmpegAudioFileReader::sniff(fileUrl);
  }
#endif

  if (!fileReader) {
    handleError(getName(), outError, "The file is not supported for reading!");
    return nullptr;
  }

  auto reader = std::make_unique<AudioFileForReading>(fileUrl, sampleFormat, interleaved);
  reader->mFileReader = std::move(fileReader);
  if (reader->prepare(outError)) {
    return reader;
  }
  return nullptr;
}

AudioFileForReading::AudioFileForReading(std::string inputUrl,
  const AudioSampleFormat sampleFormat,
  const bool interleaved)
    : mInputUrl(std::move(inputUrl)),
      mOutputSampleFormat(sampleFormat),
      mInterleaved(interleaved) {
}

AudioFileForReading::~AudioFileForReading() {
  release();
}

void AudioFileForReading::release() {
  mDecodedBuffer.clear();
  if (mFileReader) {
    mFileReader->close();
    mFileReader.reset();
  }
}

bool AudioFileForReading::prepare(Error **outError) {
  if (!mFileReader) {
    return false;
  }
  auto result = mFileReader->open(outError);
  if (!result) {
    return false;
  }
  // The audio file decoder only supports interleaved format
  mFileFormat = AudioFormat::InitWithCommonFormat(mFileReader->getSampleFormat(),
      mFileReader->getSampleRate(),
      mFileReader->getChannelCount(),
      true);
  if (mOutputSampleFormat == AudioSampleFormat::kDefault) {
    mOutputSampleFormat = mFileReader->getSampleFormat();
  }
  mProcessFormat = AudioFormat::InitWithCommonFormat(mOutputSampleFormat,
      mFileReader->getSampleRate(),
      mFileReader->getChannelCount(),
      mInterleaved).value();
  MINILOG_DEBUG("file format: {}, output format: {}, sample rate: {}, channel count: {}, interleaved: {}",
      convertToText(mFileReader->getSampleFormat()),
      convertToText(mOutputSampleFormat),
      mFileReader->getSampleRate(),
      mFileReader->getChannelCount(),
      mInterleaved);
  return true;
}

AudioFramePosition AudioFileForReading::getLength() const {
  if (mFileReader) {
    return mFileReader->getSampleLength();
  }
  return Definitions::kInvalidValue;
}

AudioFramePosition AudioFileForReading::getFramePosition() const {
  if (mFileReader) {
    return mFileReader->getFramePosition();
  }
  return Definitions::kInvalidValue;
}

void AudioFileForReading::setFramePosition(AudioFramePosition framePosition) {
  if (mFileReader) {
    mFileReader->seekTo(framePosition);
  }
}

bool AudioFileForReading::readIntoBuffer(std::unique_ptr<AudioPCMBuffer> &buffer, Error **outError) {
  if (!buffer) {
    return false;
  }
  return readIntoBuffer(buffer, buffer->getFrameCapacity(), outError);
}

bool AudioFileForReading::readIntoBuffer(std::unique_ptr<AudioPCMBuffer> &pcmBuffer,
  const uint32_t frames,
  Error **outError) {
  if (!mFileReader || !pcmBuffer) {
    handleError(getName(), outError, "Not implementation of the AudioFileDecoder or the AudioPCMBuffer!");
    return false;
  }

  if (pcmBuffer->getFormat() != mProcessFormat) {
    handleError(getName(), outError, "The format of the AudioPCMBuffer must be same with the process format!");
    return false;
  }

  if (mOutputSampleFormat != AudioSampleFormat::kFloat &&
      mOutputSampleFormat != AudioSampleFormat::kInt16 &&
      mOutputSampleFormat != AudioSampleFormat::kInt32) {
    handleError(getName(), outError, "Unsupported the pcm format of the AudioPCMBuffer!");
    return false;
  }

  // The reader fills a byte buffer whose frames are the ones of the file's own sample
  // format, which is not necessarily the frame size of the processing format: a 16 bit file
  // that is read as float, for example, has half as many bytes per frame. Sizing the decode
  // buffer with the processing format would therefore ask the reader for more frames than
  // the buffer can hold.
  const auto readerChannels = static_cast<size_t>(std::max(mFileReader->getChannelCount(), 0));
  const auto readerBytesPerFrame =
    static_cast<size_t>(MediaUtils::getBytesPerSample(mFileReader->getSampleFormat())) * readerChannels;
  if (readerBytesPerFrame == 0) {
    handleError(getName(), outError, "The sample format of the file is unknown!");
    return false;
  }

  const auto framesToRead = std::min<int64_t>(frames, pcmBuffer->getFrameCapacity());
  mDecodedBuffer.resize(static_cast<size_t>(framesToRead) * readerBytesPerFrame);
  auto numberFrames = mFileReader->readIntoBuffer(mDecodedBuffer.data(),
    static_cast<int64_t>(mDecodedBuffer.size()), outError);
  if (numberFrames <= 0) {
    handleError(getName(), outError, "Failed to read into buffer");
    return false;
  }

  // checks whether we should convert to output pcm format
  void *cacheBuffer = mDecodedBuffer.data();
  if (mOutputSampleFormat != mFileReader->getSampleFormat()) {
    auto channelCount = mProcessFormat->getChannelCount();
    if (!mFormatConverter) {
      auto bufferSize = pcmBuffer->getFrameCapacity() * channelCount;
      mFormatConverter =
          std::make_unique<SampleFormatConverter>(bufferSize, mFileReader->getSampleFormat(), mOutputSampleFormat);
    }
    // The converter is driven with interleaved samples and answers with the number of
    // samples it wrote, so the frame count has to be recovered from it before the frames
    // are deinterleaved below.
    const auto samples = mFormatConverter->convertToInternalOutput(numberFrames * channelCount, mDecodedBuffer.data());
    if (samples <= 0 || channelCount == 0) {
      handleError(getName(), outError, "Failed to convert the samples of the file!");
      return false;
    }
    numberFrames = samples / channelCount;
    cacheBuffer = mFormatConverter->getOutputBuffer();
  }

  auto framesCopy = AudioUtils::copyIntoPCMBuffer(cacheBuffer, numberFrames, pcmBuffer);
  return framesCopy > 0;
}

bool AudioFileForReading::writeFromBuffer(const std::unique_ptr<AudioPCMBuffer> &buffer, Error **outError) {
  handleError(getName(), outError, "Unsupported writing from buffer!");
  return false;
}

void AudioFileForReading::handleError(const std::string &tag, Error **outError, std::string errorMsg) {
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
