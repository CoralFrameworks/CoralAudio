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

#include "SourceInt16.h"

namespace coral {

SourceInt16::SourceInt16(int32_t channelCount) noexcept
    : FlowGraphSourceBuffered(channelCount) {}

int32_t SourceInt16::onProcess(int32_t numFrames) {
  auto* floatData = output.getBuffer();
  int32_t channelCount = output.getChannelCount();

  int32_t framesLeft = mSizeInFrames - mFrameIndex;
  int32_t framesToProcess = std::min(numFrames, framesLeft);
  int32_t numSamples = framesToProcess * channelCount;

  const auto* shortBase = static_cast<const int16_t*>(mData);
  const int16_t* shortData = &shortBase[mFrameIndex * channelCount];

  for (int i = 0; i < numSamples; i++) {
    *floatData++ = *shortData++ * (1.0f / 32768);
  }

  mFrameIndex += framesToProcess;
  return framesToProcess;
}

} // namespace coral
