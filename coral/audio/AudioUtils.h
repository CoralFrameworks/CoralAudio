/*
 * Copyright (C) 2024 The Coral Project
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

#ifndef CORAL_AUDIO_UTILS_H
#define CORAL_AUDIO_UTILS_H

#include <coral/audio/AudioBuffer.h>

#include <utils/compiler.h>

#include <cmath>
#include <memory>

namespace coral {

class AudioFormat;

class AudioUtils final {
public:
  /**
   * Copies audio pcm data into an interleaved output buffer from the AudioPCMBuffer
   *
   * @param outputBuffer the interleaved output buffer for audio pcm data.
   *                     it's format must be same as the AudioPCMBuffer.
   * @param numFrames    the number of frames of the output buffer.
   * @param pcmBuffer    the AudioPCMBuffer to copy
   * @return the number of frames actually copies
   */
  static AudioFrameCount
  copyFromPCMBuffer(void* outputBuffer, AudioFrameCount numFrames, const std::unique_ptr<AudioPCMBuffer>& pcmBuffer);

  /**
   * Copies audio pcm data from an interleaved output buffer into the AudioPCMBuffer
   *
   * @param inputBuffer the interleaved input buffer for audio pcm data.
   *                    it's format must be same as the AudioPCMBuffer.
   * @param numFrames   the number of frames of the input buffer.
   *                    the number must be no large than the AudioPCMBuffer's
   * frame capacity
   * @param pcmBuffer   the specified AudioPCMBuffer to store.
   * @return the number of frames actually copies.
   */
  static AudioFrameCount
  copyIntoPCMBuffer(void* inputBuffer, AudioFrameCount numFrames, std::unique_ptr<AudioPCMBuffer>& pcmBuffer);

  /**
   * Copies audio pcm data from an interleaved output buffer into the AudioPCMBuffer
   *
   * @param inputBuffer the interleaved input buffer for audio pcm data.
   *                    it's format must be same as the AudioPCMBuffer.
   * @param numFrames   the number of frames of the input buffer.
   *                    the number must be no large than the AudioPCMBuffer's
   * frame capacity
   * @param pcmBuffer   the specified AudioPCMBuffer to store.
   * @param volume      the volume of the audio
   * @return the number of frames actually copies.
   */
  static AudioFrameCount copyIntoPCMBuffer(void* inputBuffer,
    AudioFrameCount numFrames,
    std::unique_ptr<AudioPCMBuffer>& pcmBuffer,
    float volume);

  /**
   * Changes the volume of the pcm buffer.
   *
   * @param pcmBuffer the pcm buffer to change volume
   * @param volume   the volume to change
   */
  static void changeVolume(std::unique_ptr<AudioPCMBuffer>& pcmBuffer, const float volume) {
    changeVolume(pcmBuffer.get(), volume);
  }

  static void changeVolume(AudioPCMBuffer* pcmBuffer, float volume);

  /**
   * Convert a single-precision floating point value to a Q0.31 integer value.
   * Rounds to nearest, ties away from 0.
   *
   * Values outside the range [-1.0, 1.0) are properly clamped to -2147483648
   * and 2147483647, including -Inf and +Inf. NaN values are considered
   * undefined, and behavior may change depending on hardware and future
   * implementation of this function.
   */
  static int32_t clamp32FromFloat(float f);

  /**
   * Convert an array of floats to an array of 16-bit integers.
   *
   * @param source the input array.
   * @param destination the output array.
   * @param numSamples the number of values to convert.
   */
  static void convertFloatToPcm16(const float* source, int16_t* destination, int32_t numSamples);

  /**
   * Convert an array of 16-bit integers to an array of floats.
   *
   * @param source the input array.
   * @param destination the output array.
   * @param numSamples the number of values to convert.
   */
  static void convertPcm16ToFloat(const int16_t* source, float* destination, int32_t numSamples);

  /**
   * Convert an array of floats to an array of 24-bit integers.
   *
   * @param source the input array.
   * @param destination the output array.
   * @param numSamples the number of values to convert.
   */
  static void convertFloatToPcm24(const float* source, uint8_t* destination, int32_t numSamples);

  /**
   * Convert an array of 24-bit integers to an array of floats.
   *
   * @param source the input array.
   * @param destination the output array.
   * @param numSamples the number of values to convert.
   */
  static void convertPcm24ToFloat(const uint8_t* source, float* destination, int32_t numSamples);

  static int64_t getNanoseconds();

  static void sleepForNanos(int64_t nanoseconds);

private:
  AudioUtils() = default;
  ~AudioUtils() = default;

  static constexpr float kScaleI16ToFloat = (1.0f / 32768.0f);
};

/**
 * Convert a 32-bit Q0.31 value to single-precision floating-point.
 * The output float range is [-1.0, 1.0] for the fixed-point range
 * [0x80000000, 0x7fffffff].
 *
 * Rounding may occur in the least significant 8 bits for large fixed point
 * values due to storage into the 24-bit floating-point significand.
 * Rounding will be to nearest, ties to even.
 */
static inline float float_from_i32(const int32_t value) {
  static constexpr float scale = 1.0 / static_cast<float>(1UL << 31);
  return static_cast<float>(value) * scale;
}

/**
 * Convert a packed 24bit Q0.23 value stored native-endian in a uint8_t ptr
 * to a signed fixed-point 32 bit integer Q0.31 value. The output Q0.31 range
 * is [0x80000000, 0x7fffff00] for the fixed-point range [0x800000, 0x7fffff].
 * Even though the output range is limited on the positive side, there is no
 * DC offset on the output, if the input has no DC offset.
 *
 * Avoid relying on the limited output range, as future implementations may go
 * to full range.
 */
static inline int32_t i32_from_p24(const uint8_t* packed24) {
#if defined(CORAL_CPU_LENDIAN)
  const uint32_t v = (static_cast<uint32_t>(packed24[0]) << 8) |
                     (static_cast<uint32_t>(packed24[1]) << 16) |
                     (static_cast<uint32_t>(packed24[2]) << 24);
  return static_cast<int32_t>(v);
#else
  const uint32_t v = (static_cast<uint32_t>(packed24[2]) << 8) |
                     (static_cast<uint32_t>(packed24[1]) << 16) |
                     (static_cast<uint32_t>(packed24[0]) << 24);
  return static_cast<int32_t>(v);
#endif
}

/**
 * Convert a packed 24bit Q0.23 value stored native endian in a uint8_t ptr
 * to single-precision floating-point. The output float range is [-1.0, 1.0)
 * for the fixed-point range [0x800000, 0x7fffff].
 *
 * There is no rounding, the conversion and representation is exact.
 */
static inline float float_from_p24(const uint8_t* packed24) {
  return float_from_i32(i32_from_p24(packed24));
}

/**
 * Convert a single-precision floating point value to a Q0.23 integer value,
 * stored in a 32 bit signed integer (technically stored as Q8.23, but clamped
 * to Q0.23).
 *
 * OLD code disabled: Rounds to nearest, ties away from 0.
 * NEW code enabled: Rounding of 0.5 lsb is away from 0.
 *
 * Values outside the range [-1.0, 1.0) are properly clamped to -8388608 and
 * 8388607, including -Inf and +Inf. NaN values are considered undefined, and
 * behavior may change depending on hardware and future implementation of this
 * function.
 */
static inline int32_t clamp24_from_float(float f) {
#if 0
  static const float scale = (float) (1 << 23);
  static const float limpos = 0x7fffff / scale;
  static const float limneg = -0x800000 / scale;

  if (f <= limneg) {
    return -0x800000;
  } else if (f >= limpos) {
    return 0x7fffff;
  }
  f *= scale;
  /* integer conversion is through truncation (though int to float is not).
   * ensure that we round to nearest, ties away from 0.
   */
  return f > 0 ? f + 0.5 : f - 0.5;
#else
  static const float scale = 1 << 23;
  return roundf(fmaxf(fminf(f * scale, scale - 1.f), -scale));
#endif
}


} // namespace coral

#endif // CORAL_AUDIO_UTILS_H
