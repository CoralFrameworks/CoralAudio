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

#include "SampleFormatConverter.h"

namespace coral {

SampleFormatConverter::SampleFormatConverter(const int32_t maxSamples,
  const AudioSampleFormat inputFormat,
  const AudioSampleFormat outputFormat) noexcept {
  mInputType = inputFormat;
  mOutputType = outputFormat;

  mMaxSamples = maxSamples;
  mInputBuffer = std::make_unique<uint8_t[]>(maxSamples * sizeof(int32_t));
  mOutputBuffer = std::make_unique<uint8_t[]>(maxSamples * sizeof(int32_t));

  switch (mInputType) {
    case AudioSampleFormat::kInt16:
      mSource = FlowGraphSourceBuffered::make(1, FlowGraphSourceBuffered::SourceType::kSourceInt16);
      break;
    case AudioSampleFormat::kInt24:
      mSource = FlowGraphSourceBuffered::make(1, FlowGraphSourceBuffered::SourceType::kSourceInt24);
      break;
    case AudioSampleFormat::kInt32:
      mSource = FlowGraphSourceBuffered::make(1, FlowGraphSourceBuffered::SourceType::kSourceInt32);
      break;
    case AudioSampleFormat::kFloat:
    case AudioSampleFormat::kDefault:
    case AudioSampleFormat::kInvalid:
      mSource = FlowGraphSourceBuffered::make(1, FlowGraphSourceBuffered::SourceType::kSourceFloat);
      break;
  }

  switch (mOutputType) {
    case AudioSampleFormat::kInt16:
      mSink = FlowGraphSink::make(1, FlowGraphSink::SinkType::kSinkInt16);
      break;
    case AudioSampleFormat::kInt24:
      mSink = FlowGraphSink::make(1, FlowGraphSink::SinkType::kSinkInt24);
      break;
    case AudioSampleFormat::kInt32:
      mSink = FlowGraphSink::make(1, FlowGraphSink::SinkType::kSinkInt32);
      break;
    case AudioSampleFormat::kFloat:
    case AudioSampleFormat::kDefault:
    case AudioSampleFormat::kInvalid:
      mSink = FlowGraphSink::make(1, FlowGraphSink::SinkType::kSinkFloat);
      break;
  }

  if (mSource && mSink) {
    mSource->getOutput()->connect(mSink->getInput(0));
    mSink->pullReset();
  }
}

int32_t SampleFormatConverter::convertToInternalOutput(const int32_t numSamples, const void* inputBuffer) {
  assert(numSamples <= mMaxSamples);
  return convert(getOutputBuffer(), numSamples, inputBuffer);
}

int32_t SampleFormatConverter::convert(void* outputBuffer, const int32_t numSamples, const void* inputBuffer) {
  mSource->setData(inputBuffer, numSamples);
  return mSink->read(outputBuffer, numSamples);
}

} // namespace coral
