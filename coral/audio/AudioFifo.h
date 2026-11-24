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

#ifndef CORAL_AUDIO_FIFO_H
#define CORAL_AUDIO_FIFO_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioTypes.h>
#include <coral/audio/OpResult.h>

#include "private/common/AudioRingBuffer.h"

#include <memory>

namespace coral {

/**
 * AudioFifo to operate at the sample level rather than the byte level.
 */
class AudioFifo final : public AudioNoncopyable {
public:
  ~AudioFifo() override;

  /**
   * Allocates an AudioFifo.
   *
   * @param sampleFormat sample format
   * @param channels number of channels
   * @param capacityInFrames number of capacity samples
   *
   * @return newly allocated AudioFifo, or nullptr on error
   */
  static std::unique_ptr<AudioFifo>
  Create(AudioSampleFormat sampleFormat, AudioChannelCount channels, AudioFrameCount capacityInFrames);

  /**
   * Write data to an AudioFifo.
   *
   * @param data audio data to write
   * @param numSamples number of samples to write
   *
   * @return number of samples actually written
   */
  OpResult<AudioFrameCount> write(const void* data, AudioFrameCount numSamples);

  /**
   * Read data from an AudioFifo.
   *
   * @param data audio data to store
   * @param numSamples number of samples to read
   *
   * @return number of samples actually read
   */
  OpResult<AudioFrameCount> read(void* data, AudioFrameCount numSamples);

  /**
   * Removes the data without reading it.
   *
   * @param numSamples number of samples to drain
   *
   * @return Result::OK if OK, or negative error code on failure.
   */
  [[nodiscard]] Result drain(AudioFrameCount numSamples);

  /**
   * Resets the AudioFifo buffer.
   */
  void reset();

  /**
   * Returns the current number of samples in the AudioFifo available for reading.
   */
  [[nodiscard]] AudioFrameCount size() const;

  /**
   * Returns the current number of samples in the AudioFifo available for writing.
   */
  [[nodiscard]] AudioFrameCount space() const;

  /**
   * Returns the sample format
   */
  [[nodiscard]] AudioSampleFormat getSampleFormat() const {
    return mSampleFormat;
  }

  /**
   * Returns the number of channels
   */
  [[nodiscard]] AudioChannelCount getChannels() const {
    return mRingBuffer.channels();
  }

  /**
   * Returns the number of the capacity frames
   */
  [[nodiscard]] AudioFrameCount getCapacityInFrames() const {
    return mRingBuffer.capacity();
  }

private:
  explicit AudioFifo(AudioSampleFormat sampleFormat,
    AudioChannelCount channels,
    AudioFrameCount capacityInFrames) noexcept;

  AudioSampleFormat mSampleFormat;
  AudioRingBuffer mRingBuffer;
};

} // namespace coral

#endif // CORAL_AUDIO_FIFO_H
