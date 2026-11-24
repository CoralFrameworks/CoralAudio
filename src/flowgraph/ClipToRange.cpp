/*
 * Copyright (C) 2024 The Coral Project
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

#include "ClipToRange.h"

namespace coral {

ClipToRange::ClipToRange(int32_t channelCount) noexcept
    : FlowGraphFilter(channelCount) {}

int32_t ClipToRange::onProcess(int32_t numFrames) {
  const float* inputBuffer = input->getBuffer();
  float* outputBuffer = output.getBuffer();

  int32_t numSamples = numFrames * output.getChannelCount();
  for (int32_t i = 0; i < numSamples; i++) {
    *outputBuffer++ = std::min(mMaximum, std::max(mMinimum, *inputBuffer++));
  }

  return numFrames;
}

} // namespace coral
