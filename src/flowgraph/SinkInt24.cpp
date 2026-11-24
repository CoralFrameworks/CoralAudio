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

#include "SinkInt24.h"

namespace coral {

SinkInt24::SinkInt24(int32_t channelCount) noexcept
    : FlowGraphSink(channelCount) {}

int32_t SinkInt24::read(void* data, int32_t numFrames) {
  auto* byteData = (uint8_t*)data;
  const int32_t channelCount = input->getChannelCount();

  int32_t framesLeft = numFrames;
  while (framesLeft > 0) {
    // Pull data through the input port.
    int32_t framesRead = pullData(framesLeft);
    if (framesRead <= 0) {
      break;
    }
    const float* floatData = input->getBuffer();
    int32_t numSamples = framesRead * channelCount;
    const int32_t kI24PackedMax = 0x007FFFFF;
    const int32_t kI24PackedMin = 0xFF800000;
    for (int i = 0; i < numSamples; i++) {
      auto n = (int32_t)(*floatData++ * 0x00800000);
      n = std::min(kI24PackedMax, std::max(kI24PackedMin, n)); // clip
      // Write as a packed 24-bit integer in Little Endian format.
      *byteData++ = (uint8_t)n;
      *byteData++ = (uint8_t)(n >> 8);
      *byteData++ = (uint8_t)(n >> 16);
    }
    framesLeft -= framesRead;
  }
  return numFrames - framesLeft;
}

} // namespace coral
