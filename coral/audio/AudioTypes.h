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

#ifndef CORAL_AUDIO_TYPES_H
#define CORAL_AUDIO_TYPES_H

#include <coral/audio/AudioBase.h>

#include <codec/Error.h>
#include <media/Definitions.h>
#include <media/Types.h>

#include <cstdint>
#include <vector>

namespace coral {

using Boolean = bool;
using Float32 = float;
using Float64 = double;

/**
 * A position in an audio file or stream.
 */
using AudioFramePosition = int64_t;

/**
 * A number of audio sample frames.
 */
using AudioFrameCount = uint32_t;

/**
 * A number of packets of compressed audio data.
 */
using AudioPacketCount = uint32_t;

/**
 * A number of audio channels.
 *
 * @discussion
 *   Rationale: making this a potentially larger-than-32-bit type like unsigned int
 *   would open the door to a large set of runtime failures due to underlying
 *   implementations' use of uint32_t.
 */
using AudioChannelCount = uint32_t;

/**
 * The index of a bus on an AudioNode.
 *
 * @discussion
 *   AudioNode objects potentially have multiple input and/or output buses.
 *   AudioNodeBus represents a bus as a zero-based index.
 */
using AudioNodeBus = uint32_t;

/**
 * The values that specify the sample rate audio quality for encoding and conversion.
 */
enum class AudioQuality : uint8_t {
  /**
   * Fastest but may not sound great.
   * This may be implemented using bilinear interpolation.
   */
  Fastest,
  /**
   * Low quality with 8 taps.
   */
  Low,
  /**
   * Medium quality with 16 taps.
   */
  Medium,
  /**
   * High quality with 32 taps.
   */
  High,
  /**
   * Highest quality, which may be expensive in terms of CPU.
   */
  Best,
};

/**
 * The result of an operation. All except the `OK` result indicates that an error occurred.
 */
enum class Result : int32_t {
  // clang-format off
  OK = 0,                       // OK
  ErrorBase = -900,             // ERROR_BASE,
  ErrorDisconnected = -899,     // ERROR_DISCONNECTED,
  ErrorIllegalArgument = -898,  // ERROR_ILLEGAL_ARGUMENT,
  ErrorInternal = -896,         // ERROR_INTERNAL,
  ErrorInvalidState = -895,     // ERROR_INVALID_STATE,
  ErrorInvalidHandle = -892,    // ERROR_INVALID_HANDLE,
  ErrorUnimplemented = -890,    // ERROR_UNIMPLEMENTED,
  ErrorUnavailable = -889,      // ERROR_UNAVAILABLE,
  ErrorNoFreeHandles = -888,    // ERROR_NO_FREE_HANDLES,
  ErrorNoMemory = -887,         // ERROR_NO_MEMORY,
  ErrorNull = -886,             // ERROR_NULL,
  ErrorTimeout = -885,          // ERROR_TIMEOUT,
  ErrorWouldBlock = -884,       // ERROR_WOULD_BLOCK,
  ErrorInvalidFormat = -883,    // ERROR_INVALID_FORMAT,
  ErrorOutOfRange = -882,       // ERROR_OUT_OF_RANGE,
  ErrorNoService = -881,        // ERROR_NO_SERVICE,
  ErrorInvalidRate = -880,      // ERROR_INVALID_RATE,
  // Reserved for future result types
  Reserved1,
  Reserved2,
  Reserved3,
  Reserved4,
  Reserved5,
  Reserved6,
  Reserved7,
  Reserved8,
  Reserved9,
  Reserved10,
  ErrorClosed = -869,
  // clang-format on
};

/**
 * This structure holds a pair of numbers that represent a continuous range of values.
 * @field mMinimum
 *            The minimum value.
 * @field mMaximum
 *            The maximum value.
 */
typedef struct AudioValueRange {
  Float64 mMinimum;
  Float64 mMaximum;
} AudioValueRange;

/**
 * This structure holds the buffers necessary for translation operations.
 * @field mInputData
 *            The buffer containing the data to be translated.
 * @field mInputDataSize
 *            The number of bytes in the buffer pointed at by mInputData.
 * @field mOutputData
 *            The buffer to hold the result of the translation.
 * @field mOutputDataSize
 *            The number of bytes in the buffer pointed at by mOutputData.
 */
typedef struct AudioValueTranslation {
  void* mInputData;
  uint32_t mInputDataSize;
  void* mOutputData;
  uint32_t mOutputDataSize;
} AudioValueTranslation;

/**
 * A structure to hold a buffer of audio data.
 *
 * @field mNumberChannels
 *            The number of interleaved channels in the buffer.
 * @field mDataByteSize
 *            The number of bytes in the buffer pointed at by mData.
 * @field mData
 *            A pointer to the buffer of audio data.
 */
typedef struct AudioBuffer {
  uint32_t mNumberChannels;
  uint32_t mDataByteSize;
  void* mData;
} AudioBuffer;

/**
 * A variable length array of AudioBuffer structures.
 */
class AudioBufferList final : public AudioNoncopyable {
public:
  AudioBufferList() : mBuffers(1) {
  }

  explicit AudioBufferList(const size_t numberOfBuffers) noexcept
    : mBuffers(numberOfBuffers) {
  }

  /**
   * Returns the number of buffers
   */
  [[nodiscard]] size_t numberOfBuffers() const {
    return mBuffers.size();
  }

  /**
   * Returns the pointer of the buffers.
   */
  [[nodiscard]] const AudioBuffer* getBuffers() const {
    return mBuffers.data();
  }

  /**
   * Returns the specified buffer
   */
  AudioBuffer* getBuffer(const size_t index) {
    return index < mBuffers.size() ? &mBuffers[index] : nullptr;
  }

  /**
   * Returns the specified buffer
   */
  [[nodiscard]] const AudioBuffer* getBuffer(const size_t index) const {
    return index < mBuffers.size() ? &mBuffers[index] : nullptr;
  }

  void resize(const size_t numberOfBuffers) {
    mBuffers.resize(numberOfBuffers);
  }

private:
  std::vector<AudioBuffer> mBuffers;
};

/**
 * This structure encapsulates all the information for describing the basic
 * format properties of a stream of audio data.
 *
 * @field mSampleRate
 *            The number of sample frames per second of the data in the stream.
 * @field mFormatType
 *            The AudioFormatType indicating the general kind of data in the stream.
 * @field mFormatFlags
 *            The AudioFormatFlags for the format indicated by mFormatType.
 * @field mBytesPerPacket
 *            The number of bytes in a packet of data.
 * @field mFramesPerPacket
 *            The number of sample frames in each packet of data.
 * @field mBytesPerFrame
 *            The number of bytes in a single sample frame of data.
 * @field mChannelsPerFrame
 *            The number of channels in each frame of data.
 * @field mBitsPerChannel
 *            The number of bits of sample data for each channel in a frame of data.
 * @field mReserved
 *            Pads the structure out to force an even 8 byte alignment.
 */
typedef struct AudioBasicDescription {
  // clang-format off
  Float64          mSampleRate;
  AudioFormatType  mFormatType;
  AudioFormatFlags mFormatFlags;
  uint32_t         mBytesPerPacket;
  uint32_t         mFramesPerPacket;
  uint32_t         mBytesPerFrame;
  uint32_t         mChannelsPerFrame;
  uint32_t         mBitsPerChannel;
  uint32_t         mReserved;
  // clang-format on
} AudioBasicDescription;

inline bool operator==(const AudioBasicDescription& a, const AudioBasicDescription& b) {
  return a.mSampleRate == b.mSampleRate &&
         a.mFormatType == b.mFormatType &&
         a.mFormatFlags == b.mFormatFlags &&
         a.mBytesPerPacket == b.mBytesPerPacket &&
         a.mFramesPerPacket == b.mFramesPerPacket &&
         a.mBytesPerFrame == b.mBytesPerFrame &&
         a.mChannelsPerFrame == b.mChannelsPerFrame &&
         a.mBitsPerChannel == b.mBitsPerChannel;
}

/**
 * This structure describes the packet layout of a buffer of data where the size of
 * each packet may not be the same or where there is extraneous data between packets.
 *
 *  @field mStartOffset
 *             The number of bytes from the start of the buffer to the beginning of the
 *             packet.
 *  @field mFramesInPacket
 *             The number of sample frames of data in the packet. For formats with a
 *             constant number of frames per packet, this field is set to 0.
 *
 *  @field mDataByteSize
 *             The number of bytes in the packet.
 */
typedef struct AudioPacketDescription {
  // clang-format off
  int64_t  mStartOffset;
  uint32_t mFramesInPacket;
  uint32_t mDataByteSize;
  // clang-format on
} AudioPacketDescription;

} // namespace coral

#endif // CORAL_AUDIO_TYPES_H
