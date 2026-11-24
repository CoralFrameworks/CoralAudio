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

#include "MultiToMonoConverter.h"

namespace coral {

MultiToMonoConverter::MultiToMonoConverter(int32_t inputChannelCount) noexcept
    : output(*this, 1) {
  input = std::make_unique<AudioGraphPortFloatInput>(*this, inputChannelCount);
}

int32_t MultiToMonoConverter::onProcess(int32_t numFrames) {
  auto channelCount = input->getChannelCount();
  const auto* inputBuffer = input->getBuffer();
  auto* outputBuffer = output.getBuffer();
  for (int i = 0; i < numFrames; i++) {
    // read first channel of multi stream, write many
    *outputBuffer++ = *inputBuffer;
    inputBuffer += channelCount;
  }
  return numFrames;
}

AudioGraphPortFloatInput* MultiToMonoConverter::getInput(uint32_t bus) const {
  if (bus == 0) {
    return input.get();
  }
  return nullptr;
}

} // namespace coral
