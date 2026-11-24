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

#ifndef CORAL_AUDIO_SAMPLE_FORMAT_CONVERTER_H
#define CORAL_AUDIO_SAMPLE_FORMAT_CONVERTER_H

#include <coral/audio/AudioTypes.h>

#include "flowgraph/FlowGraphNode.h"

#include <memory>

namespace coral {

/**
 * The format converter of the audio data
 */
class SampleFormatConverter final {
public:
  explicit SampleFormatConverter(int32_t maxSamples,
    AudioSampleFormat inputFormat,
    AudioSampleFormat outputFormat) noexcept;

  /**
   * Returns internal buffer used to store input data
   */
  [[nodiscard]] void* getOutputBuffer() const {
    return mOutputBuffer.get();
  }

  /**
   * Returns internal buffer used to store output data
   */
  [[nodiscard]] void* getInputBuffer() const {
    return mInputBuffer.get();
  }

  /**
   * Convert data from external buffer into internal output buffer.
   *
   * @param numSamples
   * @param inputBuffer
   * @return
   */
  int32_t convertToInternalOutput(int32_t numSamples, const void* inputBuffer);

  /**
   * Convert data formats between the specified external buffers.
   *
   * @param outputBuffer
   * @param numSamples
   * @param inputBuffer
   * @return
   */
  int32_t convert(void* outputBuffer, int32_t numSamples, const void* inputBuffer);

private:
  AudioSampleFormat mInputType;
  AudioSampleFormat mOutputType;

  int32_t mMaxSamples = 0;
  std::unique_ptr<uint8_t[]> mInputBuffer;
  std::unique_ptr<uint8_t[]> mOutputBuffer;

  std::unique_ptr<FlowGraphSourceBuffered> mSource;
  std::unique_ptr<FlowGraphSink> mSink;
};

} // namespace coral

#endif // CORAL_AUDIO_SAMPLE_FORMAT_CONVERTER_H
