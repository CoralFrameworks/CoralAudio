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

#ifndef CORAL_AUDIO_FIFO_CONTROLLER_BASE_H
#define CORAL_AUDIO_FIFO_CONTROLLER_BASE_H

#include <coral/audio/AudioTypes.h>

#include <cstdint>

namespace coral {

/**
 * Manage the read/write indices of a circular buffer.
 *
 * The caller is responsible for reading and writing the actual data.
 * Note that the span of available frames may not be contiguous. They
 * may wrap around from the end to the beginning of the buffer. In that
 * case the data must be read or written in at least two blocks of frames.
 */
class FifoControllerBase {
public:
  virtual ~FifoControllerBase() = default;

  /**
   * Construct a `FifoControllerBase`.
   *
   * @param totalFrames capacity of the circular buffer in frames
   */
  explicit FifoControllerBase(AudioFrameCount totalFrames);

  /**
   * The frames available to read will be calculated from the read and write
   * counters. The result will be clipped to the capacity of the buffer. If the
   * buffer has underflow then this will return zero.
   *
   * @return number of valid frames available to read.
   */
  [[nodiscard]] AudioFrameCount getFullFramesAvailable() const;

  /**
   * The index in a circular buffer of the next frame to read.
   *
   * @return read index position
   */
  [[nodiscard]] AudioFrameCount getReadIndex() const;

  /**
   * Advance read index from a number of frames.
   * Equivalent of incrementReadCounter(numFrames).
   *
   * @param numFrames number of frames to advance the read index
   */
  void advanceReadIndex(AudioFrameCount numFrames);

  /**
   * Get the number of frame that are not written yet.
   *
   * @return maximum number of frames that can be written without exceeding the
   * threshold
   */
  [[nodiscard]] AudioFrameCount getEmptyFramesAvailable() const;

  /**
   * The index in a circular buffer of the next frame to write.
   *
   * @return index of the next frame to write
   */
  [[nodiscard]] AudioFrameCount getWriteIndex() const;

  /**
   * Advance write index from a number of frames.
   * Equivalent of incrementWriteCounter(numFrames).
   *
   * @param numFrames number of frames to advance the write index
   */
  void advanceWriteIndex(AudioFrameCount numFrames);

  /**
   * Get the frame capacity of the fifo.
   *
   * @return frame capacity
   */
  [[nodiscard]] AudioFrameCount getFrameCapacity() const {
    return mTotalFrames;
  }

  [[nodiscard]] virtual uint64_t getReadCounter() const = 0;

  virtual void setReadCounter(uint64_t n) = 0;

  virtual void incrementReadCounter(uint64_t n) = 0;

  [[nodiscard]] virtual uint64_t getWriteCounter() const = 0;

  virtual void setWriteCounter(uint64_t n) = 0;

  virtual void incrementWriteCounter(uint64_t n) = 0;

private:
  AudioFrameCount mTotalFrames;
};

} // namespace coral

#endif // CORAL_AUDIO_FIFO_CONTROLLER_BASE_H
