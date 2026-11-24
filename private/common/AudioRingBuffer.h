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

#ifndef CORAL_AUDIO_RING_BUFFER_H
#define CORAL_AUDIO_RING_BUFFER_H

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace coral {

// ---------------------------------------------------------------------------
// Packed 24-bit little-endian sample (3 bytes/sample).
// ---------------------------------------------------------------------------
struct SampleDataPacked24 {
  uint8_t b[3];
};

// ---------------------------------------------------------------------------
// Sample conversion traits: every supported type maps to/from float in [-1, 1].
// ---------------------------------------------------------------------------
template<typename T>
struct SampleTraits;

template<>
struct SampleTraits<float> {
  static float toFloat(const float v) noexcept { return v; }
  static float fromFloat(const float v) noexcept { return v; }
};

template<>
struct SampleTraits<int16_t> {
  static float toFloat(const int16_t v) noexcept {
    return static_cast<float>(v) * (1.0f / 32768.0f);
  }
  static int16_t fromFloat(const float v) noexcept {
    long r = std::lroundf(std::clamp(v, -1.0f, 1.0f) * 32768.0f);
    if (r > 32767L) r = 32767L;
    else if (r < -32768L) r = -32768L;
    return static_cast<int16_t>(r);
  }
};

template<>
struct SampleTraits<SampleDataPacked24> {
  static float toFloat(const SampleDataPacked24 v) noexcept {
    uint32_t u = static_cast<uint32_t>(v.b[0])
               | (static_cast<uint32_t>(v.b[1]) << 8)
               | (static_cast<uint32_t>(v.b[2]) << 16);
    if (u & 0x00800000u) u |= 0xFF000000u; // sign-extend bit 23
    return static_cast<float>(static_cast<int32_t>(u)) * (1.0f / 8388608.0f);
  }

  static SampleDataPacked24 fromFloat(const float v) noexcept {
    long long r = std::llround(
        static_cast<double>(std::clamp(v, -1.0f, 1.0f)) * 8388608.0);
    if (r > 8388607LL) r = 8388607LL;
    else if (r < -8388608LL) r = -8388608LL;
    SampleDataPacked24 out{};
    out.b[0] = static_cast<uint8_t>(r & 0xFF);
    out.b[1] = static_cast<uint8_t>((r >> 8) & 0xFF);
    out.b[2] = static_cast<uint8_t>((r >> 16) & 0xFF);
    return out;
  }
};

template<>
struct SampleTraits<int32_t> {
  static float toFloat(const int32_t v) noexcept {
    return static_cast<float>(v) * (1.0f / 2147483648.0f);
  }
  static int32_t fromFloat(const float v) noexcept {
    long long r = std::llround(
        static_cast<double>(std::clamp(v, -1.0f, 1.0f)) * 2147483648.0);
    if (r > 2147483647LL) r = 2147483647LL;
    else if (r < -2147483648LL) r = -2147483648LL;
    return static_cast<int32_t>(r);
  }
};

/**
 * A single-producer, single-consumer circular buffer for audio sample data.
 *
 * Internal storage is planar float:
 *   mRingBuffer[channel * mCapacity + frameIndex]
 *
 * Public APIs support both channel layouts:
 *   - packed  (interleaved): write() / read() / writeInt16() / ...
 *   - planar   (per-channel): writePlanar() / readPlanar() / writePlanarInt16() / ...
 *
 * Every public API converts on the fly. The planar float path is zero-copy
 * per channel (memcpy), the packed path is per-sample.
 */
class AudioRingBuffer {
public:
  using Packed24 = SampleDataPacked24;

  /**
   * Initializes the buffer.
   *
   * @param capacityFrames requested capacity; rounded up to a power of two.
   * @param channels number of channels; must be non-zero.
   * @return `true` when initialized, `false` when `channels` is zero (the buffer
   *         is left empty and must not be used until a successful call).
   */
  bool init(const size_t capacityFrames, const uint32_t channels) {
    if (channels == 0) {
      // A zero-channel buffer has no meaningful frames and would silently accept
      // writes that can never be read back. Reject the contract violation.
      mCapacity = 0;
      mChannels = 0;
      mRingBuffer.clear();
      mHeader.store(0, std::memory_order_relaxed);
      mTailer.store(0, std::memory_order_relaxed);
      return false;
    }
    mCapacity = 1;
    while (mCapacity < capacityFrames) mCapacity <<= 1;
    mChannels = channels;
    // Planar layout: channels contiguous blocks of mCapacity floats each.
    mRingBuffer.assign(static_cast<size_t>(mChannels) * mCapacity, 0.0f);
    mHeader.store(0, std::memory_order_relaxed);
    mTailer.store(0, std::memory_order_relaxed);
    return true;
  }

  size_t capacity() const noexcept { return mCapacity; }
  uint32_t channels() const noexcept { return mChannels; }

  size_t available() const noexcept {
    const size_t h = mHeader.load(std::memory_order_acquire);
    const size_t t = mTailer.load(std::memory_order_acquire);
    return h - t;
  }

  size_t space() const noexcept {
    return mCapacity - available();
  }

  // =========================================================================
  // Packed (interleaved) API
  // =========================================================================

  bool write(const float* interleaved, const size_t frames) noexcept {
    return writeImpl<float>(interleaved, frames);
  }
  size_t read(float* interleaved, const size_t frames) noexcept {
    return readImpl<float>(interleaved, frames);
  }

  bool writeFloat(const uint8_t* interleaved, const size_t frames) noexcept {
    return writeImpl<float>(reinterpret_cast<const float*>(interleaved), frames);
  }
  size_t readFloat(uint8_t* interleaved, const size_t frames) noexcept {
    return readImpl<float>(reinterpret_cast<float*>(interleaved), frames);
  }

  bool writeInt16(const int16_t* interleaved, const size_t frames) noexcept {
    return writeImpl<int16_t>(interleaved, frames);
  }
  size_t readInt16(int16_t* interleaved, const size_t frames) noexcept {
    return readImpl<int16_t>(interleaved, frames);
  }

  bool writeInt24(const Packed24* interleaved, const size_t frames) noexcept {
    return writeImpl<Packed24>(interleaved, frames);
  }
  size_t readInt24(Packed24* interleaved, const size_t frames) noexcept {
    return readImpl<Packed24>(interleaved, frames);
  }

  bool writeInt24(const uint8_t* interleaved, const size_t frames) noexcept {
    return writeImpl<Packed24>(
        reinterpret_cast<const Packed24*>(interleaved), frames);
  }
  size_t readInt24(uint8_t* interleaved, const size_t frames) noexcept {
    return readImpl<Packed24>(reinterpret_cast<Packed24*>(interleaved), frames);
  }

  bool writeInt32(const int32_t* interleaved, const size_t frames) noexcept {
    return writeImpl<int32_t>(interleaved, frames);
  }
  size_t readInt32(int32_t* interleaved, const size_t frames) noexcept {
    return readImpl<int32_t>(interleaved, frames);
  }

  // =========================================================================
  // Planar API
  //
  // planar[c] points to the first sample of channel c.
  // Write requires at least `frames` samples per channel.
  // Read writes at most `n` samples per channel and returns n.
  // =========================================================================

  // Generic planar write/read: T = float, int16_t, int32_t, Packed24.
  template<typename T>
  bool writePlanar(const T* const* planar, const size_t frames) noexcept {
    return writePlanarImpl<T>(planar, frames);
  }
  template<typename T>
  size_t readPlanar(T* const* planar, const size_t frames) noexcept {
    return readPlanarImpl<T>(planar, frames);
  }

  // Convenience wrappers.
  bool writePlanarFloat(const float* const* planar, const size_t frames) noexcept {
    return writePlanarImpl<float>(planar, frames);
  }
  size_t readPlanarFloat(float* const* planar, const size_t frames) noexcept {
    return readPlanarImpl<float>(planar, frames);
  }

  bool writePlanarInt16(const int16_t* const* planar, const size_t frames) noexcept {
    return writePlanarImpl<int16_t>(planar, frames);
  }
  size_t readPlanarInt16(int16_t* const* planar, const size_t frames) noexcept {
    return readPlanarImpl<int16_t>(planar, frames);
  }

  bool writePlanarInt32(const int32_t* const* planar, const size_t frames) noexcept {
    return writePlanarImpl<int32_t>(planar, frames);
  }
  size_t readPlanarInt32(int32_t* const* planar, const size_t frames) noexcept {
    return readPlanarImpl<int32_t>(planar, frames);
  }

  // Planar 24-bit with Packed24 structs.
  bool writePlanarInt24(const Packed24* const* planar, const size_t frames) noexcept {
    return writePlanarImpl<Packed24>(planar, frames);
  }
  size_t readPlanarInt24(Packed24* const* planar, const size_t frames) noexcept {
    return readPlanarImpl<Packed24>(planar, frames);
  }

  // Planar 24-bit raw bytes (3 bytes per sample, little-endian).
  bool writePlanarInt24(const uint8_t* const* planar, const size_t frames) noexcept {
    const size_t h = mHeader.load(std::memory_order_relaxed);
    const size_t t = mTailer.load(std::memory_order_acquire);
    if (mCapacity - (h - t) < frames) return false;

    const size_t pos = h & (mCapacity - 1);
    const size_t part1 = std::min(frames, mCapacity - pos);
    const size_t part2 = frames - part1;

    for (uint32_t c = 0; c < mChannels; ++c) {
      const uint8_t* src = planar[c];
      float* dstBase = &mRingBuffer[static_cast<size_t>(c) * mCapacity];
      for (size_t i = 0; i < part1; ++i) {
        Packed24 v{ src[i * 3], src[i * 3 + 1], src[i * 3 + 2] };
        dstBase[pos + i] = SampleTraits<Packed24>::toFloat(v);
      }
      for (size_t i = 0; i < part2; ++i) {
        const size_t j = part1 + i;
        Packed24 v{ src[j * 3], src[j * 3 + 1], src[j * 3 + 2] };
        dstBase[i] = SampleTraits<Packed24>::toFloat(v);
      }
    }

    mHeader.store(h + frames, std::memory_order_release);
    return true;
  }

  size_t readPlanarInt24(uint8_t* const* planar, const size_t frames) noexcept {
    const size_t t = mTailer.load(std::memory_order_relaxed);
    const size_t h = mHeader.load(std::memory_order_acquire);
    const size_t avl = h - t;
    const size_t n = (frames < avl) ? frames : avl;

    const size_t pos = t & (mCapacity - 1);
    const size_t part1 = std::min(n, mCapacity - pos);
    const size_t part2 = n - part1;

    for (uint32_t c = 0; c < mChannels; ++c) {
      uint8_t* dst = planar[c];
      const float* srcBase = &mRingBuffer[static_cast<size_t>(c) * mCapacity];
      for (size_t i = 0; i < part1; ++i) {
        const Packed24 v = SampleTraits<Packed24>::fromFloat(srcBase[pos + i]);
        dst[i * 3]     = v.b[0];
        dst[i * 3 + 1] = v.b[1];
        dst[i * 3 + 2] = v.b[2];
      }
      for (size_t i = 0; i < part2; ++i) {
        const Packed24 v = SampleTraits<Packed24>::fromFloat(srcBase[i]);
        const size_t j = part1 + i;
        dst[j * 3]     = v.b[0];
        dst[j * 3 + 1] = v.b[1];
        dst[j * 3 + 2] = v.b[2];
      }
    }

    mTailer.store(t + n, std::memory_order_release);
    return n;
  }

  // =========================================================================
  // Control
  // =========================================================================

  size_t discard(const size_t numFrames) noexcept {
    const size_t t = mTailer.load(std::memory_order_relaxed);
    const size_t h = mHeader.load(std::memory_order_acquire);
    const size_t avl = h - t;
    const size_t n = (numFrames < avl) ? numFrames : avl;
    mTailer.store(t + n, std::memory_order_release);
    return n;
  }

  /**
   * clean buffer.
   * Call on stop()
   */
  void reset() noexcept {
    mHeader.store(0, std::memory_order_relaxed);
    mTailer.store(0, std::memory_order_relaxed);
  }

private:
  std::vector<float> mRingBuffer;             // planar: c * mCapacity + idx
  size_t   mCapacity = 0;
  uint32_t mChannels = 0;
  alignas(64) std::atomic<size_t> mHeader{ 0 };
  alignas(64) std::atomic<size_t> mTailer{ 0 };

  // ========================================================================
  // Packed (interleaved) implementation -> planar storage
  // ========================================================================
  template<typename T>
  bool writeImpl(const T* interleaved, const size_t frames) noexcept {
    const size_t h = mHeader.load(std::memory_order_relaxed);
    const size_t t = mTailer.load(std::memory_order_acquire);
    if (mCapacity - (h - t) < frames) return false;

    for (size_t i = 0; i < frames; ++i) {
      const size_t idx = (h + i) & (mCapacity - 1);
      const T* src = interleaved + i * mChannels;

      if constexpr (std::is_same_v<T, float>) {
        for (uint32_t c = 0; c < mChannels; ++c) {
          mRingBuffer[static_cast<size_t>(c) * mCapacity + idx] = src[c];
        }
      } else {
        for (uint32_t c = 0; c < mChannels; ++c) {
          mRingBuffer[static_cast<size_t>(c) * mCapacity + idx] =
              SampleTraits<T>::toFloat(src[c]);
        }
      }
    }

    mHeader.store(h + frames, std::memory_order_release);
    return true;
  }

  template<typename T>
  size_t readImpl(T* interleaved, const size_t frames) noexcept {
    const size_t t = mTailer.load(std::memory_order_relaxed);
    const size_t h = mHeader.load(std::memory_order_acquire);
    const size_t avl = h - t;
    const size_t n = (frames < avl) ? frames : avl;

    for (size_t i = 0; i < n; ++i) {
      const size_t idx = (t + i) & (mCapacity - 1);
      T* dst = interleaved + i * mChannels;

      if constexpr (std::is_same_v<T, float>) {
        for (uint32_t c = 0; c < mChannels; ++c) {
          dst[c] = mRingBuffer[static_cast<size_t>(c) * mCapacity + idx];
        }
      } else {
        for (uint32_t c = 0; c < mChannels; ++c) {
          dst[c] = SampleTraits<T>::fromFloat(
              mRingBuffer[static_cast<size_t>(c) * mCapacity + idx]);
        }
      }
    }

    mTailer.store(t + n, std::memory_order_release);
    return n;
  }

  // ========================================================================
  // Planar implementation -> planar storage
  //
  // Each channel is a contiguous circular sub-buffer of length mCapacity.
  // We split into up to two linear chunks to handle wraparound.
  // ========================================================================
  template<typename T>
  bool writePlanarImpl(const T* const* planar, const size_t frames) noexcept {
    const size_t h = mHeader.load(std::memory_order_relaxed);
    const size_t t = mTailer.load(std::memory_order_acquire);
    if (mCapacity - (h - t) < frames) return false;

    const size_t pos   = h & (mCapacity - 1);
    const size_t part1 = std::min(frames, mCapacity - pos);
    const size_t part2 = frames - part1;

    for (uint32_t c = 0; c < mChannels; ++c) {
      const T* src = planar[c];
      float* dstBase = &mRingBuffer[static_cast<size_t>(c) * mCapacity];

      if constexpr (std::is_same_v<T, float>) {
        std::memcpy(dstBase + pos, src, part1 * sizeof(float));
        if (part2) std::memcpy(dstBase, src + part1, part2 * sizeof(float));
      } else {
        for (size_t i = 0; i < part1; ++i) {
          dstBase[pos + i] = SampleTraits<T>::toFloat(src[i]);
        }
        for (size_t i = 0; i < part2; ++i) {
          dstBase[i] = SampleTraits<T>::toFloat(src[part1 + i]);
        }
      }
    }

    mHeader.store(h + frames, std::memory_order_release);
    return true;
  }

  template<typename T>
  size_t readPlanarImpl(T* const* planar, const size_t frames) noexcept {
    const size_t t = mTailer.load(std::memory_order_relaxed);
    const size_t h = mHeader.load(std::memory_order_acquire);
    const size_t avl = h - t;
    const size_t n = (frames < avl) ? frames : avl;

    const size_t pos   = t & (mCapacity - 1);
    const size_t part1 = std::min(n, mCapacity - pos);
    const size_t part2 = n - part1;

    for (uint32_t c = 0; c < mChannels; ++c) {
      T* dst = planar[c];
      const float* srcBase = &mRingBuffer[static_cast<size_t>(c) * mCapacity];

      if constexpr (std::is_same_v<T, float>) {
        std::memcpy(dst, srcBase + pos, part1 * sizeof(float));
        if (part2) std::memcpy(dst + part1, srcBase, part2 * sizeof(float));
      } else {
        for (size_t i = 0; i < part1; ++i) {
          dst[i] = SampleTraits<T>::fromFloat(srcBase[pos + i]);
        }
        for (size_t i = 0; i < part2; ++i) {
          dst[part1 + i] = SampleTraits<T>::fromFloat(srcBase[i]);
        }
      }
    }

    mTailer.store(t + n, std::memory_order_release);
    return n;
  }
};

} // namespace coral

#endif // CORAL_AUDIO_RING_BUFFER_H
