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

#include "SourceInt32.h"

namespace coral {

SourceInt32::SourceInt32(int32_t channelCount) noexcept
    : FlowGraphSourceBuffered(channelCount) {}

int32_t SourceInt32::onProcess(int32_t numFrames) {
  float* floatData = output.getBuffer();
  const int32_t channelCount = output.getChannelCount();

  const int32_t framesLeft = mSizeInFrames - mFrameIndex;
  const int32_t framesToProcess = std::min(numFrames, framesLeft);
  const int32_t numSamples = framesToProcess * channelCount;

  const auto* intBase = static_cast<const int32_t*>(mData);
  const int32_t* intData = &intBase[mFrameIndex * channelCount];

  for (int i = 0; i < numSamples; i++) {
    *floatData++ = *intData++ * kScale;
  }

  mFrameIndex += framesToProcess;
  return framesToProcess;
}

} // namespace coral
