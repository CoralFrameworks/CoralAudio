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

#ifndef CORAL_AUDIO_FIFO_BUFFER_H
#define CORAL_AUDIO_FIFO_BUFFER_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioTypes.h>
#include <coral/audio/OpResult.h>

#include "FifoControllerBase.h"

#include <memory>

namespace coral {

class FifoBuffer final : public AudioNoncopyable {
public:
  /**
   * Construct a `FifoBuffer`.
   *
   * @param bytesPerFrame amount of bytes for one frame
   * @param capacityInFrames the capacity of frames in fifo
   */
  explicit FifoBuffer(AudioFrameCount bytesPerFrame, AudioFrameCount capacityInFrames) noexcept;

  /**
   * Construct a `FifoBuffer`.
   * To be used if the storage allocation is done outside of FifoBuffer.
   *
   * @param bytesPerFrame amount of bytes for one frame
   * @param capacityInFrames capacity of frames in fifo
   * @param readCounterAddress address of read counter
   * @param writeCounterAddress address of write counter
   * @param dataStorageAddress address of storage
   */
  explicit FifoBuffer(AudioFrameCount bytesPerFrame,
    AudioFrameCount capacityInFrames,
    std::atomic<uint64_t>* readCounterAddress,
    std::atomic<uint64_t>* writeCounterAddress,
    uint8_t* dataStorageAddress) noexcept;

  ~FifoBuffer() override;

  /**
   * Convert a number of frames in bytes.
   *
   * @return number of bytes
   */
  [[nodiscard]] AudioFrameCount convertFramesToBytes(AudioFrameCount frames) const;

  /**
   * Read framesToRead or, if not enough, then read as many as are available.
   *
   * @param buffer the buffer to store audio data
   * @param numFrames number of frames requested
   * @return number of frames actually read, or Result::Error*
   */
  OpResult<AudioFrameCount> read(void* buffer, AudioFrameCount numFrames) const;

  /**
   * Write framesToWrite or, if too enough, then write as many as the fifo are
   * not empty.
   *
   * @param buffer the audio buffer to write
   * @param numFrames number of frames requested
   * @return number of frames actually write, or Result::Error*
   */
  OpResult<AudioFrameCount> write(const void* buffer, AudioFrameCount numFrames) const;

  /**
   * Get the buffer capacity in frames.
   *
   * @return number of frames
   */
  [[nodiscard]] AudioFrameCount getBufferCapacityInFrames() const;

  /**
   * Returns whether the fifo buffer is empty.
   */
  [[nodiscard]] bool empty() const;

  /**
   * Calls read(). If all of the frames cannot be read then the remainder of the
   * buffer is set to zero.
   *
   * @param buffer the buffer to store audio data
   * @param numFrames number of frames requested
   * @return number of frames actually read
   */
  OpResult<AudioFrameCount> readNow(void* buffer, AudioFrameCount numFrames);

  /**
   * Get the number of frames in the fifo.
   *
   * @return number of frames actually in the buffer
   */
  [[nodiscard]] AudioFrameCount getFullFramesAvailable() const {
    return mFifo->getFullFramesAvailable();
  }

  /**
   * Get the amount of bytes per frame.
   *
   * @return number of bytes per frame
   */
  [[nodiscard]] AudioFrameCount getBytesPerFrame() const {
    return mBytesPerFrame;
  }

  /**
   * Get the position of read counter.
   *
   * @return position of read counter
   */
  [[nodiscard]] uint64_t getReadCounter() const {
    return mFifo->getReadCounter();
  }

  /**
   * Set the position of read counter.
   *
   * @param n position of read counter
   */
  void setReadCounter(const uint64_t n) const {
    mFifo->setReadCounter(n);
  }

  /**
   * Get the position of write counter.
   *
   * @return position of write counter
   */
  [[nodiscard]] uint64_t getWriteCounter() const {
    return mFifo->getWriteCounter();
  }

  /**
   * Set the position of write counter.
   *
   * @param n position of write counter
   */
  void setWriteCounter(const uint64_t n) const {
    mFifo->setWriteCounter(n);
  }

  /**
   * Flush the fifo
   */
  void flush() const {
    setReadCounter(getWriteCounter());
  }

private:
  AudioFrameCount mBytesPerFrame;
  uint8_t* mStorage;
  bool mStorageOwned; // did this object allocate the storage?
  std::unique_ptr<FifoControllerBase> mFifo;
  uint64_t mFramesReadCount;
  uint64_t mFramesUnderrunCount;
};

} // namespace coral

#endif // CORAL_AUDIO_FIFO_BUFFER_H
