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

#include "MonoToMultiConverter.h"

namespace coral {

MonoToMultiConverter::MonoToMultiConverter(int32_t outputChannelCount) noexcept
    : output(*this, outputChannelCount) {
  input = std::make_unique<AudioGraphPortFloatInput>(*this, 1);
}

int32_t MonoToMultiConverter::onProcess(int32_t numFrames) {
  const auto* inputBuffer = input->getBuffer();
  auto* outputBuffer = output.getBuffer();
  int32_t channelCount = output.getChannelCount();
  for (auto i = 0; i < numFrames; i++) {
    // read one, write many
    float sample = *inputBuffer++;
    for (int channel = 0; channel < channelCount; channel++) {
      *outputBuffer++ = sample;
    }
  }
  return numFrames;
}

AudioGraphPortFloatInput* MonoToMultiConverter::getInput(uint32_t bus) const {
  if (bus == 0) {
    return input.get();
  }
  return nullptr;
}

} // namespace coral
