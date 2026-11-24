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

#ifndef CORAL_AUDIO_BUFFER_H
#define CORAL_AUDIO_BUFFER_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioTypes.h>

#include <memory>
#include <optional>
#include <vector>

namespace coral {

/**
 * A base class of the audio buffer, with a format.
 *
 * An AudioBufferBase represents a buffer of audio data and its format.
 */
class AudioBufferBase : public AudioNoncopyable {
public:
  /**
   * Returns the format of the audio in the buffer.
   */
  [[nodiscard]] std::optional<AudioFormat> getFormat() const {
    return mFormat;
  }

  /**
   * Returns the pointer of the AudioBufferList.
   */
  [[nodiscard]] AudioBufferList* mutableAudioBufferList() noexcept {
    return &mBufferList;
  }

  /**
   * Returns the pointer of the AudioBufferList.
   */
  [[nodiscard]] const AudioBufferList* audioBufferList() const noexcept {
    return &mBufferList;
  }

protected:
  AudioBufferBase()
    : mFormat(std::nullopt),
      mBufferList(0) {
  }

  explicit AudioBufferBase(const AudioFormat& format) noexcept
    : mFormat(format),
      mBufferList(1) {
  }

  std::optional<AudioFormat> mFormat;
  AudioBufferList mBufferList;
};

/**
 * A subclass of AudioBuffer for use with PCM audio formats.
 *
 * AudioPCMBuffer provides a number of methods useful for manipulating buffers
 * of audio in PCM format.
 */
class AudioPCMBuffer final : public AudioBufferBase {
public:
  /**
   * Initialize a buffer that is to contain PCM audio samples.
   *
   * @param format The format of the PCM audio to be contained in the buffer.
   * @param frameCapacity The capacity of the buffer in PCM sample frames.
   * @return Returns null in the following cases:
   *     - if the format has zero bytes per frame
   * (format.streamDescription->mBytesPerFrame == 0)
   *     - if the buffer byte capacity (frameCapacity *
   * format.streamDescription->mBytesPerFrame) cannot be represented by an
   * uint32_t
   */
  static std::unique_ptr<AudioPCMBuffer>
  InitWithPCMFormat(const AudioFormat& format, uint32_t frameCapacity);

  /**
   * Returns the buffer's capacity, in audio sample frames.
   */
  [[nodiscard]] AudioFrameCount getFrameCapacity() const {
    return mFrameCapacity;
  }

  /**
   * Returns the current number of valid sample frames in the buffer.
   */
  [[nodiscard]] AudioFrameCount getFrameLength() const {
    return mFrameLength;
  }

  /**
   * Sets the current number of valid sample frames in the buffer.
   *
   * @discussion
   * You may modify the length of the buffer as part of an operation that
   * modifies its contents. The length must be less than or equal to the
   * frameCapacity. Modifying frameLength will update the mDataByteSize in each
   * of the underlying AudioBuffer's correspondingly, and vice versa. Note that
   * in the case of deinterleaved formats, mDataByteSize will refers the size of
   * one channel's worth of audio samples.
   */
  void setFrameLength(AudioFrameCount frameLength);

  /**
   * Returns the buffer's number of interleaved channels.
   *
   * @discussion
   * Useful in conjunction with floatChannelData etc.
   */
  [[nodiscard]] uint32_t getStride() const;

  /**
   * Access the buffer's float audio samples.
   *
   * @discussion
   * floatChannelData returns pointers to the buffer's audio samples if the
   * buffer's format is 32-bit float, or nullptr if it is another format.
   *
   * The returned pointer is to format.getChannelCount() pointers to float. Each
   * of these pointers is to "frameLength" valid samples, which are spaced by
   * "stride" samples.
   *
   * If format.isInterleaved() is false (as with the standard deinterleaved
   * float format), then the pointers will be to separate chunks of memory.
   * "stride" is 1.
   *
   * If format.isInterleaved() is true, then the pointers will refer into the
   * same chunk of interleaved samples, each offset by 1 frame. "stride" is the
   * number of interleaved channels.
   */
  [[nodiscard]] float** floatChannelData() const;

  /**
   * Access the buffer's int16_t audio samples.
   *
   * @discussion
   * int16ChannelData returns the buffer's audio samples if the buffer's format
   * has 2-byte integer samples, or nullptr if it is another format.
   *
   * See the discussion of floatChannelData.
   */
  [[nodiscard]] int16_t** int16ChannelData() const;

  /**
   * Access the buffer's int32_t audio samples.
   *
   * @discussion
   * int32ChannelData returns the buffer's audio samples if the buffer's format
   * has 4-byte integer samples, or nullptr if it is another format.
   *
   * See the discussion of floatChannelData.
   */
  [[nodiscard]] int32_t** int32ChannelData() const;

  /**
   * Converts to float channel data.
   * If format.isInterleaved() is true, deinterleave and convert to float
   * format. If format.isInterleaved() is false, copy audio data to vector.
   */
  [[nodiscard]] std::vector<std::vector<float>> toFloatChannelData() const;

  // These methods are useful to copy audio data, such as jni copy.
  [[nodiscard]] void* rawData() noexcept { return mStorage.data(); }
  [[nodiscard]] const void* rawData() const noexcept { return mStorage.data(); }
  [[nodiscard]] size_t rawByteSize() const noexcept { return mStorage.size(); }

private:
  explicit AudioPCMBuffer(const AudioFormat& format, uint32_t frameCapacity) noexcept;

  template<typename T>
  T** buildPointerArray(std::vector<T*>& cache) const;

  //!< the capacity in frames;
  AudioFrameCount mFrameCapacity;
  //!< the length in frames
  AudioFrameCount mFrameLength;
  std::vector<uint8_t> mStorage;

  // caches
  mutable std::vector<float*> mFloatPtrs;
  mutable std::vector<int16_t*> mInt16Ptrs;
  mutable std::vector<int32_t*> mInt32Ptrs;
};

/**
 * A subclass of AudioBuffer for use with compressed audio formats.
 */
class AudioCompressedBuffer final : public AudioBufferBase {
public:
  /**
   * Initialize a buffer that is to contain constant bytes per packet compressed
   * audio data.
   *
   * @param format The format of the audio to be contained in the buffer.
   * @param packetCapacity The capacity of the buffer in packets.
   * @return an instance of the AudioCompressedBuffer, or nullptr when any error
   * occurs.
   *
   * @discussion
   * This fails if the format is PCM or if the format has variable bytes per
   * packet (format.getStreamDescription()->mBytesPerPacket == 0).
   */
  static std::unique_ptr<AudioCompressedBuffer>
  InitWithFormat(const AudioFormat& format, AudioPacketCount packetCapacity);

  /**
   * Initialize a buffer that is to contain compressed audio data.
   *
   * @param format The format of the audio to be contained in the buffer.
   * @param packetCapacity The capacity of the buffer in packets.
   * @param maximumPacketSize The maximum size in bytes of a compressed packet.
   *     The maximum packet size can be obtained from the maximumOutputPacketSize
   *     property of an AudioConverter configured for encoding this format.
   * @return an instance of the AudioCompressedBuffer, or nullptr when any error occurs.
   *
   * An exception is raised if the format is PCM.
   */
  static std::unique_ptr<AudioCompressedBuffer>
  InitWithFormat(const AudioFormat& format, AudioPacketCount packetCapacity, uint32_t maximumPacketSize);

  /**
   * Returns the pointer of the buffer.
   */
  [[nodiscard]] void* data() noexcept {
    return mStorage.data();
  }

  /**
   * Returns the pointer of the buffer.
   */
  [[nodiscard]] const void* data() const noexcept {
    return mStorage.data();
  }

  /**
   * The number of compressed packets the buffer can contain.
   */
  [[nodiscard]] AudioPacketCount getPacketCapacity() const {
    return mPacketCapacity;
  }

  /**
   * The current number of compressed packets in the buffer.
   *
   * You may modify the packetCount as part of an operation that modifies its
   * contents. The packetCount must be less than or equal to the packetCapacity.
   */
  [[nodiscard]] AudioPacketCount getPacketCount() const {
    return mPacketCount;
  }

  /**
   * Sets the number of compressed packets in the buffer.
   *
   * @param packetCount the number of compressed packets to set.
   *
   * The packetCount must be less than or equal to the packetCapacity.
   */
  void setPacketCount(AudioPacketCount packetCount);

  /**
   * Returns the maximum size of a compressed packet in bytes.
   */
  [[nodiscard]] uint32_t getMaximumPacketSize() const {
    return mMaximumPacketSize;
  }

  /**
   * Returns the buffer's capacity in bytes
   */
  [[nodiscard]] uint32_t getByteCapacity() const {
    return mByteCapacity;
  }

  /**
   * Returns the current number of valid bytes in the buffer.
   */
  [[nodiscard]] uint32_t getByteLength() const {
    return mByteLength;
  }

  /**
   * Sets the new number of valid bytes in the buffer.
   */
  void setByteLength(uint32_t newByteLength);

  /**
   * Returns the buffer's array of packet descriptions, if any.
   */
  [[nodiscard]] const AudioPacketDescription* packetDescriptions() const {
    return mPacketDescriptions.data();
  }

  AudioPacketDescription* mutablePacketDescriptions() {
    return mPacketDescriptions.empty() ? nullptr : mPacketDescriptions.data();
  }

  void reset();

private:
  explicit AudioCompressedBuffer(const AudioFormat& format,
    AudioPacketCount capacity,
    uint32_t maxPacketSize);

  //!< the number of the packet
  AudioPacketCount mPacketCount;
  //!<  the maximum size of a compressed packet in bytes.
  uint32_t mMaximumPacketSize;
  //!< the number of compressed packets the buffer can contain.
  AudioPacketCount mPacketCapacity;
  //!< the buffer's capacity in bytes
  uint32_t mByteCapacity;
  uint32_t mByteLength;

  std::vector<uint8_t> mStorage;

  //!< Access the buffer's array of packet descriptions, if any.
  std::vector<AudioPacketDescription> mPacketDescriptions;
};

} // namespace coral

#endif // CORAL_AUDIO_BUFFER_H
