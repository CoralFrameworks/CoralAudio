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

#ifndef CORAL_AUDIO_GENERIC_RESAMPLE_H
#define CORAL_AUDIO_GENERIC_RESAMPLE_H

#include <coral/audio/Resample.h>

#include "flowgraph/ChannelCountConverter.h"
#include "flowgraph/MonoToMultiConverter.h"
#include "flowgraph/MultiToMonoConverter.h"
#include "flowgraph/SampleRateConverter.h"

namespace coral {

/**
 * A Resample based on AudioGraph to convert between the various data format.
 *
 * Convert PCM channels, format and sample rate for optimal latency.
 */
class GenericResample final : public Resample {
public:
  GenericResample();

  bool init(const AudioConfiguration& srcDescription, const AudioConfiguration& dstDescription) override;

  bool init(const AudioFormat& inputFormat, const AudioFormat& outputFormat) override;

  bool resample(const std::shared_ptr<AVSampleBuffer>& inputBuffer,
      std::shared_ptr<AVSampleBuffer>& outputBuffer) override;

  bool resample(const std::unique_ptr<AudioPCMBuffer>& inputBuffer,
    std::unique_ptr<AudioPCMBuffer>& outputBuffer) override;

  int resample(const uint8_t* inputData, int inputFrames, uint8_t* outputData,
      int maxOutputFrames) override;

  void flush() override;

  void release() override;

  [[nodiscard]] std::string getName() const override {
    return "GenericResample";
  }

private:
  // The mp3 number of samples
  static constexpr int kMaxNumberSamples = 1152;

  AudioFrameCount mMaxFrames = 0;
  uint32_t mBytesPerFrame = 0;
  std::unique_ptr<uint8_t[]> mCacheBuffer;

  AudioConfiguration mInputDescription;
  AudioConfiguration mOutputDescription;

  std::unique_ptr<FlowGraphSourceBuffered> mSource;
  std::unique_ptr<ChannelCountConverter> mChannelCountConverter;
  std::unique_ptr<MonoToMultiConverter> mMonoToMultiConverter;
  std::unique_ptr<MultiToMonoConverter> mMultiToMonoConverter;
  std::unique_ptr<MultiChannelResampler> mResampler;
  std::unique_ptr<SampleRateConverter> mRateConverter;
  std::unique_ptr<FlowGraphSink> mSink;
};

} // namespace coral

#endif // CORAL_AUDIO_GENERIC_RESAMPLE_H
