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

#include "MultiToManyConverter.h"

namespace coral {

MultiToManyConverter::MultiToManyConverter(int32_t channelCount) noexcept
    : outputs(channelCount) {
  input = std::make_unique<AudioGraphPortFloatInput>(*this, channelCount);
  for (auto i = 0; i < channelCount; i++) {
    outputs[i] = std::make_unique<AudioGraphPortFloatOutput>(*this, 1);
  }
}

int32_t MultiToManyConverter::onProcess(int32_t numFrames) {
  int32_t channelCount = input->getChannelCount();

  for (auto ch = 0; ch < channelCount; ch++) {
    const float* inputBuffer = input->getBuffer() + ch;
    float* outputBuffer = outputs[ch]->getBuffer();

    for (auto i = 0; i < numFrames; i++) {
      *outputBuffer++ = *inputBuffer;
      inputBuffer += channelCount;
    }
  }

  return numFrames;
}

AudioGraphPortFloatInput* MultiToManyConverter::getInput(uint32_t bus) const {
  if (bus == 0) {
    return input.get();
  }
  return nullptr;
}

} // namespace coral
