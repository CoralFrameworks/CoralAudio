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

#include "AudioStreamBuffered.h"

#include <coral/audio/AudioUtils.h>

#include <minilog/log.h>

namespace coral {

/*
 * AudioStream with a FifoBuffer
 */
AudioStreamBuffered::AudioStreamBuffered(const AudioStreamBuilder& builder)
  : AudioStreamAndroid(builder) {
}

void AudioStreamBuffered::allocateFifo() {
  // If the caller does not provide a callback use our own internal
  // callback that reads data from the FIFO.
  if (usingFIFO()) {
    // FIFO is configured with the same format and channels as the stream.
    int32_t capacityFrames = getBufferCapacityInFrames();
    if (capacityFrames == kUnspecified) {
      capacityFrames = getFramesPerBurst() * kDefaultBurstsPerBuffer;
    } else {
      int32_t minFramesPerBufferByBursts =
        getFramesPerBurst() * kMinBurstsPerBuffer;
      if (capacityFrames <= minFramesPerBufferByBursts) {
        capacityFrames = minFramesPerBufferByBursts;
      } else {
        capacityFrames = std::max(kMinFramesPerBuffer, capacityFrames);
        // round up to nearest burst
        int32_t numBursts =
          (capacityFrames + getFramesPerBurst() - 1) / getFramesPerBurst();
        capacityFrames = numBursts * getFramesPerBurst();
      }
    }

    mFifoBuffer = std::make_unique<FifoBuffer>(getBytesPerFrame(), capacityFrames);
    mBufferCapacityInFrames = capacityFrames;
    mBufferSizeInFrames = mBufferCapacityInFrames;
  }
}

void AudioStreamBuffered::updateFramesWritten() {
  if (mFifoBuffer) {
    mFramesWritten = static_cast<int64_t>(mFifoBuffer->getWriteCounter());
  } // or else it will get updated by processBufferCallback()
}

void AudioStreamBuffered::updateFramesRead() {
  if (mFifoBuffer) {
    mFramesRead = static_cast<int64_t>(mFifoBuffer->getReadCounter());
  } // or else it will get updated by processBufferCallback()
}

DataCallbackResult
AudioStreamBuffered::onDefaultCallback(void* audioData, uint32_t numFrames) {
  int32_t framesTransferred = 0;

  if (getDirection() == Direction::Render) {
    // Read from the FIFO and write to audioData, clear part of buffer if not enough data.
    const auto result = mFifoBuffer->readNow(audioData, numFrames);
    if (result) {
      framesTransferred = result.value();
    }
  } else {
    // Read from audioData and write to the FIFO
    const auto result = mFifoBuffer->write(audioData, numFrames); // There is no writeNow()
    if (result) {
      framesTransferred = result.value();
    }
  }

  if (framesTransferred < numFrames) {
    MINILOG_DEBUG("xrun! framesTransferred = {}, numFrames = {}", framesTransferred, numFrames);
    // TODO If we do not allow FIFO to wrap then our timestamps will drift when there is an XRun!
    incrementXRunCount();
  }
  markCallbackTime(static_cast<int32_t>(numFrames)); // so foreground knows how long to wait.
  return DataCallbackResult::Continue;
}

int64_t AudioStreamBuffered::predictNextCallbackTime() const {
  if (mBackgroundRanAtNanoseconds == 0) {
    return 0;
  }
  const auto nanosPerBuffer =
    (Definitions::kNanosPerSecond * mLastBackgroundSize) / getSampleRate();
  // arbitrary delay so we wake up just after
  constexpr int64_t margin = 200 * Definitions::kNanosPerMicrosecond;
  return mBackgroundRanAtNanoseconds + nanosPerBuffer + margin;
}

void AudioStreamBuffered::markCallbackTime(int32_t numFrames) {
  mLastBackgroundSize = numFrames;
  mBackgroundRanAtNanoseconds = AudioUtils::getNanoseconds();
}

OpResult<uint32_t>
AudioStreamBuffered::transfer(void* readBuffer,
  const void* writeBuffer,
  const uint32_t numFrames,
  const int64_t timeoutNanoseconds) {
  // Validate arguments.
  if (readBuffer != nullptr && writeBuffer != nullptr) {
    MINILOG_ERROR("Both buffers are not NULL");
    return Result::ErrorInternal;
  }
  if (getDirection() == Direction::Capture && readBuffer == nullptr) {
    MINILOG_ERROR("readBuffer is NULL");
    return Result::ErrorNull;
  }
  if (getDirection() == Direction::Render && writeBuffer == nullptr) {
    MINILOG_ERROR("writeBuffer is NULL");
    return Result::ErrorNull;
  }
  if (numFrames < 0) {
    MINILOG_ERROR("numFrames is negative");
    return Result::ErrorOutOfRange;
  } else if (numFrames == 0) {
    return OpResult<uint32_t>(numFrames);
  }
  if (timeoutNanoseconds < 0) {
    MINILOG_ERROR("timeoutNanoseconds is negative");
    return Result::ErrorOutOfRange;
  }

  int32_t result = 0;
  auto readData = static_cast<uint8_t*>(readBuffer);
  const auto *writeData = static_cast<const uint8_t*>(writeBuffer);
  int32_t framesLeft = numFrames;
  int64_t timeToQuit = 0;
  bool repeat = true;

  // Calculate when to timeout.
  if (timeoutNanoseconds > 0) {
    timeToQuit = AudioUtils::getNanoseconds() + timeoutNanoseconds;
  }

  // Loop until we get the data, or we have an error, or we timeout.
  do {
    // read or write
    if (getDirection() == Direction::Capture) {
      auto framesRead = mFifoBuffer->read(readData, framesLeft);
      if (framesRead) {
        result = framesRead.value();
      }
      if (result > 0) {
        readData += mFifoBuffer->convertFramesToBytes(result);
        framesLeft -= result;
      }
    } else {
      // between zero and capacity
      uint32_t fullFrames = mFifoBuffer->getFullFramesAvailable();
      // Do not write above threshold size.
      int32_t emptyFrames = getBufferSizeInFrames() - fullFrames;
      int32_t framesToWrite = std::max(0, std::min(framesLeft, emptyFrames));
      result = mFifoBuffer->write(writeData, framesToWrite).value();
      if (result > 0) {
        writeData += mFifoBuffer->convertFramesToBytes(result);
        framesLeft -= result;
      }
    }

    // If we need more data then sleep and try again.
    if (framesLeft > 0 && result >= 0 && timeoutNanoseconds > 0) {
      const auto timeNow = AudioUtils::getNanoseconds();
      if (timeNow >= timeToQuit) {
        MINILOG_ERROR("TIMEOUT");
        repeat = false; // TIMEOUT
      } else {
        // Figure out how long to sleep.
        int64_t sleepForNanos;
        int64_t wakeTimeNanos = predictNextCallbackTime();
        if (wakeTimeNanos <= 0) {
          // No estimate available. Sleep for one burst.
          sleepForNanos = (getFramesPerBurst() * Definitions::kNanosPerSecond) / getSampleRate();
        } else {
          // Don't sleep past timeout.
          if (wakeTimeNanos > timeToQuit) {
            wakeTimeNanos = timeToQuit;
          }
          sleepForNanos = wakeTimeNanos - timeNow;
          // Avoid rapid loop with no sleep.
          constexpr auto minSleepTime = Definitions::kNanosPerMillisecond; // arbitrary
          if (sleepForNanos < minSleepTime) {
            sleepForNanos = minSleepTime;
          }
        }

        AudioUtils::sleepForNanos(sleepForNanos);
      }

    } else {
      repeat = false;
    }
  } while (repeat);

  if (result < 0) {
    return OpResult<uint32_t>(static_cast<Result>(result));
  } else {
    int32_t framesWritten = numFrames - framesLeft;
    return OpResult<uint32_t>(framesWritten);
  }
}

OpResult<uint32_t>
AudioStreamBuffered::write(const void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) {
  if (getState() == StreamState::Closed) {
    return Result::ErrorClosed;
  }

  if (getDirection() == Direction::Capture) {
    // TODO review, better error code?
    return Result::ErrorUnavailable;
  }
  Result result = updateServiceFrameCounter();
  if (result != Result::OK) {
    return result;
  }
  return transfer(nullptr, buffer, numFrames, timeoutNanoseconds);
}

OpResult<uint32_t>
AudioStreamBuffered::read(void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) {
  if (getState() == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  if (getDirection() == Direction::Render) {
    // TODO review, better error code?
    return Result::ErrorUnavailable;
  }
  Result result = updateServiceFrameCounter();
  if (result != Result::OK) {
    return result;
  }
  return transfer(buffer, nullptr, numFrames, timeoutNanoseconds);
}

OpResult<uint32_t>
AudioStreamBuffered::setBufferSizeInFrames(uint32_t requestedFrames) {
  if (getState() == StreamState::Closed) {
    return Result::ErrorClosed;
  }

  if (!mFifoBuffer) {
    return Result::ErrorUnimplemented;
  }

  if (requestedFrames > mFifoBuffer->getBufferCapacityInFrames()) {
    requestedFrames = mFifoBuffer->getBufferCapacityInFrames();
  } else if (requestedFrames < getFramesPerBurst()) {
    requestedFrames = getFramesPerBurst();
  }
  mBufferSizeInFrames = requestedFrames;
  return OpResult<uint32_t>(requestedFrames);
}

uint32_t AudioStreamBuffered::getBufferCapacityInFrames() const {
  if (mFifoBuffer) {
    return mFifoBuffer->getBufferCapacityInFrames();
  } else {
    return AudioStream::getBufferCapacityInFrames();
  }
}

bool AudioStreamBuffered::isXRunCountSupported() const {
  // XRun count is only supported if we're using blocking I/O (not callbacks)
  return (!isDataCallbackSpecified());
}

} // namespace coral
