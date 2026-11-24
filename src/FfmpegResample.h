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

#ifndef CORAL_AUDIO_FFMPEG_RESAMPLE_H
#define CORAL_AUDIO_FFMPEG_RESAMPLE_H

#include <coral/audio/Resample.h>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

struct SwrContext;

namespace coral {

/**
 * The Resample based on Ffmpeg
 */
class FfmpegResample final : public Resample {
public:
  FfmpegResample();

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

  std::string getName() const override;

private:
  static AVSampleFormat convertToSampleFormat(AudioSampleFormat formatType);

  static constexpr int32_t kDefaultFrameSize = 1152;

  AudioConfiguration mInputConfig;
  AudioConfiguration mOutputConfig;
  SwrContext* mResampleContext;
  AVAudioFifo* pAudioFifo;
  std::unique_ptr<uint8_t[]> mAppBuffer;
  AudioFrameCount mMaxFrames = kDefaultFrameSize;
};

} // namespace coral

#endif // CORAL_AUDIO_FFMPEG_RESAMPLE_H
