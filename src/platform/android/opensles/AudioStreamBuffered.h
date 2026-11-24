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

#ifndef CORAL_AUDIO_ANDROID_AUDIO_STREAM_BUFFERED_H
#define CORAL_AUDIO_ANDROID_AUDIO_STREAM_BUFFERED_H

#include "platform/android/AudioStreamAndroid.h"
#include "platform/android/fifo/FifoBuffer.h"

#include <memory>
#include <string>

namespace coral {

/**
 * A stream that contains a FIFO buffer.
 * This is used to implement blocking reads and writes.
 */
class AudioStreamBuffered : public AudioStreamAndroid {
public:
  explicit AudioStreamBuffered(const AudioStreamBuilder& builder);

  void allocateFifo();

  OpResult<uint32_t> write(const void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> read(void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> setBufferSizeInFrames(uint32_t requestedFrames) override;

  uint32_t getBufferCapacityInFrames() const override;

  OpResult<uint32_t> getXRunCount() override {
    return OpResult(mXRunCount);
  }

  bool isXRunCountSupported() const override;

  virtual std::string getName() const = 0;

protected:
  DataCallbackResult onDefaultCallback(void* audioData, uint32_t numFrames) override;

  /**
   * If there is no callback then we need a FIFO between the App and OpenSL ES.
   */
  bool usingFIFO() const {
    return !isDataCallbackSpecified();
  }

  virtual Result updateServiceFrameCounter() = 0;

  void updateFramesRead() override;

  void updateFramesWritten() override;

private:
  int64_t predictNextCallbackTime() const;

  void markCallbackTime(int32_t numFrames);

  OpResult<uint32_t>
  transfer(void* readBuffer, const void* writeBuffer, uint32_t numFrames, int64_t timeoutNanoseconds);

  void incrementXRunCount() { ++mXRunCount; }

  //!< arbitrary, allows dynamic latency tuning
  static constexpr int kDefaultBurstsPerBuffer = 16;
  //!< arbitrary, allows dynamic latency tuning
  static constexpr int kMinBurstsPerBuffer = 4;
  //!< arbitrary
  static constexpr int kMinFramesPerBuffer = 48 * 32;

  std::unique_ptr<FifoBuffer> mFifoBuffer;
  int64_t mBackgroundRanAtNanoseconds = 0;
  int32_t mLastBackgroundSize = 0;
  uint32_t mXRunCount = 0;
};

} // namespace coral

#endif // CORAL_AUDIO_ANDROID_AUDIO_STREAM_BUFFERED_H
