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

#include <coral/audio/AudioTypes.h>

#include "FifoBuffer.h"
#include "FifoController.h"
#include "FifoControllerIndirect.h"

#include <minilog/log.h>

#include <algorithm>

namespace coral {

FifoBuffer::FifoBuffer(const AudioFrameCount bytesPerFrame, AudioFrameCount capacityInFrames) noexcept
    : mBytesPerFrame(bytesPerFrame),
      mStorage(nullptr),
      mFramesReadCount(0),
      mFramesUnderrunCount(0) {
  mFifo = std::make_unique<FifoController>(capacityInFrames);
  // allocate buffer
  const auto bytesPerBuffer = static_cast<int32_t>(bytesPerFrame * capacityInFrames);
  mStorage = new uint8_t[bytesPerBuffer];
  mStorageOwned = true;
}

FifoBuffer::FifoBuffer(const uint32_t bytesPerFrame,
    AudioFrameCount capacityInFrames,
    std::atomic<uint64_t> *readCounterAddress,
    std::atomic<uint64_t> *writeCounterAddress,
    uint8_t *dataStorageAddress) noexcept
    : mBytesPerFrame(bytesPerFrame),
      mStorage(dataStorageAddress),
      mFramesReadCount(0),
      mFramesUnderrunCount(0) {
  mFifo = std::make_unique<FifoControllerIndirect>(capacityInFrames, readCounterAddress, writeCounterAddress);
  mStorage = dataStorageAddress;
  mStorageOwned = false;
}

FifoBuffer::~FifoBuffer() {
  if (mStorageOwned) {
    delete[] mStorage;
  }
}

AudioFrameCount FifoBuffer::convertFramesToBytes(const AudioFrameCount frames) const {
  return frames * mBytesPerFrame;
}

OpResult<AudioFrameCount>
FifoBuffer::read(void *buffer, const AudioFrameCount numFrames) const {
  if (numFrames <= 0) {
    MINILOG_ERROR("Failed to read frames: {}", numFrames);
    return Result::OK;
  }
  // safe because numFrames is guaranteed positive
  auto framesToRead = numFrames;
  const auto framesAvailable = mFifo->getFullFramesAvailable();
  framesToRead = std::min(framesToRead, framesAvailable);

  const auto readIndex = mFifo->getReadIndex(); // ranges 0 to capacity
  auto destination = static_cast<uint8_t *>(buffer);
  auto source = &mStorage[convertFramesToBytes(readIndex)];
  if ((readIndex + framesToRead) > mFifo->getFrameCapacity()) {
    // read in two parts, first part here is at the end of the mStorage buffer
    const auto frames1 = static_cast<int32_t>(mFifo->getFrameCapacity() - readIndex);
    auto numBytes = convertFramesToBytes(frames1);
    if (numBytes < 0) {
      return Result::ErrorOutOfRange;
    }
    memcpy(destination, source, static_cast<size_t>(numBytes));
    destination += numBytes;
    // read second part, which is at the beginning of mStorage
    source = &mStorage[0];
    const auto frames2 = static_cast<int32_t>(framesToRead - frames1);
    numBytes = convertFramesToBytes(frames2);
    if (numBytes < 0) {
      return Result::ErrorOutOfRange;
    }
    memcpy(destination, source, numBytes);
  } else {
    // just read in one shot
    const auto numBytes = convertFramesToBytes(framesToRead);
    if (numBytes < 0) {
      return Result::ErrorOutOfRange;
    }
    memcpy(destination, source, static_cast<size_t>(numBytes));
  }
  mFifo->advanceReadIndex(framesToRead);

  return OpResult<AudioFrameCount>(framesToRead);
}

OpResult<AudioFrameCount>
FifoBuffer::write(const void *buffer, const AudioFrameCount numFrames) const {
  if (numFrames <= 0) {
    return Result::OK;
  }
  // Guaranteed positive.
  auto framesToWrite = numFrames;
  framesToWrite = std::min(framesToWrite, mFifo->getEmptyFramesAvailable());

  const auto writeIndex = mFifo->getWriteIndex();
  const auto byteIndex = convertFramesToBytes(writeIndex);
  const auto *source = static_cast<const uint8_t *>(buffer);
  uint8_t *destination = &mStorage[byteIndex];
  if ((writeIndex + framesToWrite) > mFifo->getFrameCapacity()) {
    // write in two parts, first part here
    const auto frames1 = static_cast<int32_t>(mFifo->getFrameCapacity() - writeIndex);
    auto numBytes = convertFramesToBytes(frames1);
    if (numBytes < 0) {
      return Result::ErrorOutOfRange;
    }
    memcpy(destination, source, numBytes);
    // read second part
    source += convertFramesToBytes(frames1);
    destination = &mStorage[0];
    const auto frames2 = static_cast<int32_t>(framesToWrite - frames1);
    numBytes = convertFramesToBytes(frames2);
    if (numBytes < 0) {
      return Result::ErrorOutOfRange;
    }
    memcpy(destination, source, numBytes);
  } else {
    // just write in one shot
    const auto numBytes = convertFramesToBytes(framesToWrite);
    if (numBytes < 0) {
      return Result::ErrorOutOfRange;
    }
    memcpy(destination, source, numBytes);
  }
  mFifo->advanceWriteIndex(framesToWrite);

  return OpResult(framesToWrite);
}

OpResult<AudioFrameCount>
FifoBuffer::readNow(void *buffer, const AudioFrameCount numFrames) {
  const auto framesRead = read(buffer, numFrames);
  if (!framesRead) {
    return framesRead;
  }

  const auto numRead = framesRead.value();
  const auto framesLeft = numFrames - numRead;
  mFramesReadCount += numRead;
  mFramesUnderrunCount += framesLeft;
  // Zero out any samples we could not set.
  if (framesLeft > 0) {
    auto *destination = static_cast<uint8_t *>(buffer);
    destination += convertFramesToBytes(numRead); // point to first byte not set
    const auto bytesToZero = convertFramesToBytes(framesLeft);
    memset(destination, 0, static_cast<size_t>(bytesToZero));
  }

  return framesRead;
}

AudioFrameCount FifoBuffer::getBufferCapacityInFrames() const {
  return mFifo->getFrameCapacity();
}

bool FifoBuffer::empty() const {
  return mFifo->getFullFramesAvailable() <= 0;
}

} // namespace coral
