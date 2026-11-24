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

#include "FifoControllerBase.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace coral {

FifoControllerBase::FifoControllerBase(const AudioFrameCount totalFrames)
    : mTotalFrames(totalFrames) {
  // Avoid ridiculously large buffers and the arithmetic wraparound issues that
  // can follow.
  assert(totalFrames <= (UINT32_MAX / 4));
}

AudioFrameCount FifoControllerBase::getFullFramesAvailable() const {
  const uint64_t writeCounter = getWriteCounter();
  const uint64_t readCounter = getReadCounter();
  if (readCounter > writeCounter) {
    return 0;
  }
  const auto delta = writeCounter - readCounter;
  if (delta >= mTotalFrames) {
    return mTotalFrames;
  }
  // delta is now guaranteed to fit within the range of a uint32_t
  return static_cast<AudioFrameCount>(delta);
}

AudioFrameCount FifoControllerBase::getReadIndex() const {
  // % works with non-power of two sizes
  return static_cast<AudioFrameCount>(getReadCounter() % mTotalFrames);
}

void FifoControllerBase::advanceReadIndex(const AudioFrameCount numFrames) {
  incrementReadCounter(numFrames);
}

AudioFrameCount FifoControllerBase::getEmptyFramesAvailable() const {
  return mTotalFrames - getFullFramesAvailable();
}

AudioFrameCount FifoControllerBase::getWriteIndex() const {
  // % works with non-power of two sizes
  return static_cast<AudioFrameCount>(getWriteCounter() % mTotalFrames);
}

void FifoControllerBase::advanceWriteIndex(const AudioFrameCount numFrames) {
  incrementWriteCounter(numFrames);
}

} // namespace coral
