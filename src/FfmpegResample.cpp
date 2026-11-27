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

#include "FfmpegResample.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <coral/audio/AudioUtils.h>

#include <string>
#include <media/Utilities.h>

#include <minilog/log.h>

namespace {

std::string ffErrorString(const int error) {
  char buffer[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(error, buffer, sizeof(buffer));
  return buffer;
}

} // namespace anonymous

namespace coral {

FfmpegResample::FfmpegResample()
    : mResampleContext(nullptr), pAudioFifo(nullptr) {
}

bool FfmpegResample::init(const AudioConfiguration& srcConfig, const AudioConfiguration& dstConfig) {
  if (srcConfig.sampleRate <= 0 || dstConfig.sampleRate <= 0) {
    return false;
  }
  mInputConfig = srcConfig;
  mOutputConfig = dstConfig;
  if (mResampleContext) {
    swr_free(&mResampleContext);
    mResampleContext = nullptr;
  }
  auto inSampleFormat = convertToSampleFormat(mInputConfig.sampleFormat);
  AVChannelLayout inChannelLayout{};
  av_channel_layout_default(&inChannelLayout, static_cast<int>(mInputConfig.channelCount));

  auto outSampleFormat = convertToSampleFormat(mOutputConfig.sampleFormat);
  AVChannelLayout outChannelLayout{};
  av_channel_layout_default(&outChannelLayout, static_cast<int>(mOutputConfig.channelCount));

  auto resampleContext = swr_alloc();
  av_opt_set_chlayout(resampleContext, "in_chlayout", &inChannelLayout, 0);
  av_opt_set_int(resampleContext, "in_sample_fmt", inSampleFormat, 0);
  av_opt_set_int(resampleContext, "in_sample_rate", mInputConfig.sampleRate, 0);
  av_opt_set_chlayout(resampleContext, "out_chlayout", &outChannelLayout, 0);
  av_opt_set_int(resampleContext, "out_sample_fmt", outSampleFormat, 0);
  av_opt_set_int(resampleContext, "out_sample_rate", mOutputConfig.sampleRate,
      0);

  auto result = swr_init(resampleContext);
  av_channel_layout_uninit(&inChannelLayout);
  av_channel_layout_uninit(&outChannelLayout);
  if (result < 0) {
    swr_free(&resampleContext);
    MINILOG_ERROR("Failed to call swr_init: {}", ffErrorString(result));
    return false;
  }
  mResampleContext = resampleContext;

  pAudioFifo =
      av_audio_fifo_alloc(outSampleFormat, mOutputConfig.channelCount, 1);
  auto bytePerFrames = MediaUtils::getBytesPerFrame(
      mOutputConfig.sampleFormat, mOutputConfig.channelCount);
  mAppBuffer = std::make_unique<uint8_t[]>(mMaxFrames * bytePerFrames);
  return true;
}

bool FfmpegResample::init(const AudioFormat& inputFormat, const AudioFormat& outputFormat) {
  auto inputConfig =
      AudioConfiguration(inputFormat.getSampleRate(), inputFormat.getChannelCount(),
          inputFormat.getSampleFormat());
  auto outputConfig =
      AudioConfiguration(outputFormat.getSampleRate(),
          outputFormat.getChannelCount(), outputFormat.getSampleFormat());
  return init(inputConfig, outputConfig);
}

bool FfmpegResample::resample(const std::shared_ptr<AVSampleBuffer>& inputBuffer,
    std::shared_ptr<AVSampleBuffer>& outputBuffer) {
  if (mResampleContext == nullptr) {
    return false;
  }

  auto timeUs = inputBuffer->timeUs();
  auto srcByteSize = inputBuffer->size();
  auto bytePerSample = av_get_bytes_per_sample(convertToSampleFormat(mInputConfig.sampleFormat));
  auto srcFrames = srcByteSize / (bytePerSample * mInputConfig.channelCount);
  auto dstFrames =
      av_rescale_rnd((int64_t)srcFrames, (int64_t)mOutputConfig.sampleRate,
          (int64_t)mInputConfig.sampleRate, AV_ROUND_UP);
  auto dstFormat = convertToSampleFormat(mOutputConfig.sampleFormat);

  if (dstFrames > mMaxFrames) {
    mMaxFrames = dstFrames;
    auto bytePerFrames = MediaUtils::getBytesPerFrame(
        mOutputConfig.sampleFormat, mOutputConfig.channelCount);
    mAppBuffer = std::make_unique<uint8_t[]>(mMaxFrames * bytePerFrames);
  }

  auto inputData = inputBuffer->array();
  auto resampledBuffer = mAppBuffer.get();
  auto resampledFrames = swr_convert(mResampleContext, &resampledBuffer,
      dstFrames, &inputData, srcFrames);
  if (resampledFrames < 0) {
    MINILOG_ERROR("Failed to resample: {}", ffErrorString(resampledFrames));
    return false;
  }

  auto dstByteSize = av_samples_get_buffer_size(
      nullptr, (int)mOutputConfig.channelCount, resampledFrames, dstFormat, 1);
  if (dstByteSize <= 0) {
    MINILOG_ERROR("Failed to get sample buffer size");
    return false;
  }

  // copy cache data to output buffer
  outputBuffer->init(timeUs, dstByteSize);
  auto outputData = outputBuffer->data();
  outputData->position(0);
  outputData->put(resampledBuffer, dstByteSize);
  outputData->limit(dstByteSize);
  return true;
}

bool FfmpegResample::resample(const std::unique_ptr<AudioPCMBuffer>& inputBuffer,
     std::unique_ptr<AudioPCMBuffer>& outputBuffer) {
  if (mResampleContext == nullptr) {
    return false;
  }

  auto srcFrames = inputBuffer->getFrameLength();
  auto dstFrames =
      av_rescale_rnd((int64_t)srcFrames, (int64_t)mOutputConfig.sampleRate,
          (int64_t)mInputConfig.sampleRate, AV_ROUND_UP);
  auto framesCapacity = std::max(mMaxFrames, inputBuffer->getFrameCapacity());
  framesCapacity = std::max(framesCapacity, outputBuffer->getFrameCapacity());
  if (framesCapacity > mMaxFrames) {
    mMaxFrames = framesCapacity;
    auto bytePerFrames = MediaUtils::getBytesPerFrame(mOutputConfig.sampleFormat, mOutputConfig.channelCount);
    mAppBuffer = std::make_unique<uint8_t[]>(framesCapacity * bytePerFrames);
  }

  auto inputData = (const uint8_t**)inputBuffer->rawData();
  auto resampledBuffer = (uint8_t**)outputBuffer->rawData();
  auto resampledFrames = swr_convert(mResampleContext, resampledBuffer,
      dstFrames, inputData, srcFrames);
  if (resampledFrames < 0) {
    MINILOG_ERROR("Resample failed: {}", ffErrorString(resampledFrames));
    return false;
  }
  outputBuffer->setFrameLength(resampledFrames);
  return true;
}

int FfmpegResample::resample(const uint8_t* inputData, int inputFrames,
    uint8_t* outputData, int maxOutputFrames) {
  if (!inputData || !outputData) {
    return Definitions::kInvalidValue;
  }
  auto dstFrames =
      av_rescale_rnd((int64_t)inputFrames, (int64_t)mOutputConfig.sampleRate,
          (int64_t)mInputConfig.sampleRate, AV_ROUND_UP);
  dstFrames = std::min(dstFrames, (int64_t)maxOutputFrames);
  auto resampledFrames = swr_convert(mResampleContext, &outputData, dstFrames,
      (const uint8_t**)&inputData, inputFrames);
  if (resampledFrames < 0) {
    MINILOG_ERROR("Failed to resample: {}", ffErrorString(resampledFrames));
    return Definitions::kInvalidValue;
  }
  return resampledFrames;
}

void FfmpegResample::flush() {
  if (pAudioFifo) {
    av_audio_fifo_reset(pAudioFifo);
  }
}

void FfmpegResample::release() {
  if (mResampleContext) {
    swr_free(&mResampleContext);
    mResampleContext = nullptr;
  }
  if (pAudioFifo) {
    av_audio_fifo_free(pAudioFifo);
    pAudioFifo = nullptr;
  }
  mAppBuffer.reset();
}

std::string FfmpegResample::getName() const { return "FfmpegResample"; }

AVSampleFormat
FfmpegResample::convertToSampleFormat(AudioSampleFormat formatType) {
  switch (formatType) {
    case AudioSampleFormat::kInt16: {
      return AV_SAMPLE_FMT_S16;
    }
    case AudioSampleFormat::kInt32: {
      return AV_SAMPLE_FMT_S32;
    }
    case AudioSampleFormat::kFloat: {
      return AV_SAMPLE_FMT_FLT;
    }
    default: {
      return AV_SAMPLE_FMT_NONE;
    }
  }
}

} // namespace coral
