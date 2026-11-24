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

#include "GenericResample.h"

#include "flowgraph/SinkFloat.h"
#include "flowgraph/SinkInt16.h"
#include "flowgraph/SinkInt24.h"
#include "flowgraph/SinkInt32.h"
#include "flowgraph/SourceFloat.h"
#include "flowgraph/SourceInt16.h"
#include "flowgraph/SourceInt24.h"
#include "flowgraph/SourceInt32.h"

#include <coral/audio/AudioUtils.h>

#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <vector>

namespace coral {

GenericResample::GenericResample() = default;

bool GenericResample::init(const AudioConfiguration& srcDescription, const AudioConfiguration& dstDescription) {
  mInputDescription = srcDescription;
  mOutputDescription = dstDescription;

  if (mInputDescription.isInvalid() || mOutputDescription.isInvalid()) {
    return false;
  }

  mBytesPerFrame = MediaUtils::getBytesPerFrame(mOutputDescription.sampleFormat, mOutputDescription.channelCount);
  mMaxFrames = kMaxNumberSamples;

  mCacheBuffer = std::make_unique<uint8_t[]>(mMaxFrames * mBytesPerFrame);

  // source node
  switch (mInputDescription.sampleFormat) {
    case AudioSampleFormat::kInt16: {
      mSource = std::make_unique<SourceInt16>(mInputDescription.channelCount);
      break;
    }
    case AudioSampleFormat::kInt24: {
      mSource = std::make_unique<SourceInt24>(mInputDescription.channelCount);
      break;
    }
    case AudioSampleFormat::kInt32: {
      mSource = std::make_unique<SourceInt32>(mInputDescription.channelCount);
      break;
    }
    case AudioSampleFormat::kFloat: {
      mSource = std::make_unique<SourceFloat>(mInputDescription.channelCount);
      break;
    }
    default: {
      mSource.reset();
      break;
    }
  }

  // sink node
  mSink.reset();
  switch (mOutputDescription.sampleFormat) {
    case AudioSampleFormat::kInt16: {
      mSink = std::make_unique<SinkInt16>(mOutputDescription.channelCount);
      break;
    }
    case AudioSampleFormat::kInt24: {
      mSink = std::make_unique<SinkInt24>(mOutputDescription.channelCount);
      break;
    }
    case AudioSampleFormat::kInt32: {
      mSink = std::make_unique<SinkInt32>(mOutputDescription.channelCount);
      break;
    }
    case AudioSampleFormat::kFloat: {
      mSink = std::make_unique<SinkFloat>(mOutputDescription.channelCount);
      break;
    }
    default: {
      break;
    }
  }

  // If the source node or sink node is nullptr, failed to init, return immediately.
  if (!mSink || !mSource) {
    return false;
  }

  auto* lastOutput = mSource->getOutput();

  // If we are going to reduce the number of channels then do it before the sample rate converter.
  if (mInputDescription.channelCount > mOutputDescription.channelCount) {
    if (mOutputDescription.channelCount == 1) {
      mMultiToMonoConverter = std::make_unique<MultiToMonoConverter>(mInputDescription.channelCount);
      lastOutput->connect(mMultiToMonoConverter->getInput(0));
      lastOutput = mMultiToMonoConverter->getOutput();
    } else {
      mChannelCountConverter = std::make_unique<ChannelCountConverter>(
        mInputDescription.channelCount, mOutputDescription.channelCount);
      lastOutput->connect(mChannelCountConverter->getInput(0));
      lastOutput = mChannelCountConverter->getOutput();
    }
  }

  // Sample rate conversion
  if (mInputDescription.sampleRate != mOutputDescription.sampleRate) {
    // Create a resampler to do the math.
    mResampler.reset(MultiChannelResampler::make(lastOutput->getChannelCount(),
      mInputDescription.sampleRate,
      mOutputDescription.sampleRate,
      MultiChannelResampler::Quality::Medium));
    // Make a audio graph node that uses the resampler.
    mRateConverter = std::make_unique<SampleRateConverter>(lastOutput->getChannelCount(), *mResampler.get());
    lastOutput->connect(mRateConverter->getInput(0));
    lastOutput = mRateConverter->getOutput();
  }

  // Expand the number of channels if required.
  if (mInputDescription.channelCount < mOutputDescription.channelCount) {
    if (mInputDescription.channelCount == 1) {
      mMonoToMultiConverter = std::make_unique<MonoToMultiConverter>(mOutputDescription.channelCount);
      lastOutput->connect(mMonoToMultiConverter->getInput(0));
      lastOutput = mMonoToMultiConverter->getOutput();
    } else {
      mChannelCountConverter = std::make_unique<ChannelCountConverter>(
        mInputDescription.channelCount, mOutputDescription.channelCount);
      lastOutput->connect(mChannelCountConverter->getInput(0));
      lastOutput = mChannelCountConverter->getOutput();
    }
  }

  // connect to sink node
  lastOutput->connect(mSink->getInput(0));
  mSink->pullReset();

  return true;
}

bool GenericResample::init(const AudioFormat& inputFormat,
  const AudioFormat& outputFormat) {
  const auto inputConfig =
    AudioConfiguration(inputFormat.getSampleRate(), inputFormat.getChannelCount(), inputFormat.getSampleFormat());
  const auto outputConfig =
    AudioConfiguration(outputFormat.getSampleRate(), outputFormat.getChannelCount(), outputFormat.getSampleFormat());
  return init(inputConfig, outputConfig);
}

bool GenericResample::resample(const std::shared_ptr<AVSampleBuffer>& inputBuffer,
  std::shared_ptr<AVSampleBuffer>& outputBuffer) {
  if (mInputDescription.isInvalid() || mOutputDescription.isInvalid()) {
    return false;
  }
  // we can not resample the flags buffer
  if (inputBuffer->isCodecConfig()
      || inputBuffer->data() == nullptr
      || inputBuffer->size() <= 0) {
    MINILOG_ERROR("Failed to resampler empty input buffer");
    return false;
  }

  // write into audio graph source
  auto timeUs = inputBuffer->timeUs();
  auto inputData = inputBuffer->array();
  auto inputBytesPerFrame = MediaUtils::getBytesPerFrame(
    mInputDescription.sampleFormat, mInputDescription.channelCount);
  auto inputSamples = (int) (inputBuffer->size() / inputBytesPerFrame);

  // If input samples is larger than max samples, we re-create the output buffer.
  if (inputSamples > mMaxFrames) {
    mMaxFrames = inputSamples;
    mCacheBuffer.reset();
    mCacheBuffer = std::make_unique<uint8_t[]>(mMaxFrames * mBytesPerFrame);
  }
  mSource->setData(inputData, inputSamples);

  // read from audio graph sink to internal buffer, and then copy to output
  // sample buffer.
  auto outputBytesPerFrame = MediaUtils::getBytesPerFrame(
    mOutputDescription.sampleFormat, mOutputDescription.channelCount);
  int32_t outputSamples = 0;
  while (outputSamples < mMaxFrames) {
    // Pull and read some data in app format after what was read before: the frames of a pass are
    // the ones the sink converted for the input, and they have to be kept in the order they came.
    auto* destination = mCacheBuffer.get()
                        + static_cast<size_t>(outputSamples) * static_cast<size_t>(outputBytesPerFrame);
    const auto framesRead = mSink->read(destination, mMaxFrames - outputSamples);
    if (framesRead <= 0) {
      break;
    }
    outputSamples += framesRead;
  }

  if (outputSamples <= 0) {
    MINILOG_ERROR("Failed to resample");
    return false;
  }

  const auto outputBufferSize = static_cast<int>(outputBytesPerFrame * outputSamples);
  outputBuffer->init(timeUs, outputBufferSize);
  const auto outputData = outputBuffer->data();
  outputData->position(0);
  outputData->put(mCacheBuffer.get(), outputBufferSize);
  outputData->limit(outputBufferSize);
  return true;
}

bool GenericResample::resample(const std::unique_ptr<AudioPCMBuffer>& inputBuffer,
  std::unique_ptr<AudioPCMBuffer>& outputBuffer) {
  if (mInputDescription.isInvalid() || mOutputDescription.isInvalid() || !inputBuffer || !outputBuffer) {
    return false;
  }

  // If input samples is larger than max samples, we re-create the output buffer.
  auto framesCapacity = std::max(mMaxFrames, inputBuffer->getFrameCapacity());
  framesCapacity = std::max(framesCapacity, outputBuffer->getFrameCapacity());
  if (framesCapacity > mMaxFrames) {
    mMaxFrames = framesCapacity;
    mCacheBuffer.reset();
    mCacheBuffer = std::make_unique<uint8_t[]>(mMaxFrames * mBytesPerFrame);
  }
  auto const inputSamples = inputBuffer->getFrameLength();
  const auto framesWritten = AudioUtils::copyFromPCMBuffer(mCacheBuffer.get(), framesCapacity, inputBuffer);
  if (framesWritten <= 0) {
    MINILOG_ERROR("Failed to cache input buffer");
    return false;
  }
  mSource->setData(mCacheBuffer.get(), framesWritten);

  // read from audio graph sink to internal buffer, and then copy to output sample buffer.
  const auto outputBytesPerFrame =
    MediaUtils::getBytesPerFrame(mOutputDescription.sampleFormat, mOutputDescription.channelCount);
  // The sink writes what it converted into a buffer of its own: the source still reads the input
  // that the cache above holds.
  std::vector<uint8_t> outputCache(static_cast<size_t>(mMaxFrames) * static_cast<size_t>(outputBytesPerFrame));
  int32_t outputFrames = 0;
  while (outputFrames < mMaxFrames) {
    // Pull and read some data in app format after what was read before.
    auto* destination = outputCache.data()
                        + static_cast<size_t>(outputFrames) * static_cast<size_t>(outputBytesPerFrame);
    const auto framesRead = mSink->read(destination, mMaxFrames - outputFrames);
    if (framesRead <= 0) {
      break;
    }
    outputFrames += framesRead;
  }
  if (outputFrames <= 0) {
    MINILOG_ERROR("Failed to resample");
    return false;
  }
  const auto copied =
    AudioUtils::copyIntoPCMBuffer(outputCache.data(), outputFrames, outputBuffer);
  if (copied <= 0) {
    MINILOG_ERROR("Failed to copy the resampled samples");
    return false;
  }
  outputBuffer->setFrameLength(copied);
  MINILOG_DEBUG("GenericResample resample: {}, output samples: {}, max frames: {}",
    inputSamples, outputFrames, mMaxFrames);
  return true;
}

int GenericResample::resample(const uint8_t* inputData, int inputFrames, uint8_t* outputData, int maxOutputFrames) {
  if (!inputData || !outputData) {
    return Definitions::kInvalidValue;
  }
  auto maxFrames = std::max(inputFrames, maxOutputFrames);
  mSource->setData(inputData, inputFrames);
  // read from audio graph sink to internal buffer, and then copy to output sample buffer.
  int outputFrames = 0;
  auto outputBuffer = outputData;
  auto bytesPerFrame =
    MediaUtils::getBytesPerFrame(mOutputDescription.sampleFormat, mOutputDescription.channelCount);
  while (outputFrames < maxOutputFrames) {
    // Pull and read some data in app format into a small buffer.
    const auto framesRead = mSink->read(outputBuffer, maxFrames);
    if (framesRead <= 0) {
      break;
    }
    outputFrames += framesRead;
    outputBuffer += framesRead * bytesPerFrame;
  }
  return outputFrames;
}

void GenericResample::flush() {
  if (mSink) {
    mSink->pullReset();
  }
}

void GenericResample::release() {
  mMaxFrames = 0;
  mCacheBuffer.reset();
  mSource.reset();
  mChannelCountConverter.reset();
  mMonoToMultiConverter.reset();
  mMultiToMonoConverter.reset();
  mResampler.reset();
  mRateConverter.reset();
  mSink.reset();
}

} // namespace coral
