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

#include <gtest/gtest.h>

#include "private/common/AudioRingBuffer.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace coral;

namespace {

// ---------- Packed 24-bit little-endian ----------

int32_t readPacked24(const std::vector<uint8_t>& buf, size_t sampleIdx) {
  const uint8_t* p = &buf[sampleIdx * 3];
  uint32_t u = static_cast<uint32_t>(p[0])
               | (static_cast<uint32_t>(p[1]) << 8)
               | (static_cast<uint32_t>(p[2]) << 16);
  if (u & 0x00800000u) u |= 0xFF000000u;
  return static_cast<int32_t>(u);
}

void writePacked24(std::vector<uint8_t>& buf, size_t sampleIdx, int32_t v) {
  buf[sampleIdx * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
  buf[sampleIdx * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  buf[sampleIdx * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
}

std::vector<int16_t> randomInt16(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> d(-32768, 32767);
  std::vector<int16_t> v(n);
  for (auto& x: v) x = static_cast<int16_t>(d(rng));
  return v;
}

std::vector<int32_t> randomInt24Values(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int32_t> d(-8388608, 8388607);
  std::vector<int32_t> v(n);
  for (auto& x: v) x = d(rng);
  return v;
}

} // namespace anonymous

// ===========================================================================
// Init
// ===========================================================================

TEST(AudioRingBufferInit, CapacityRoundsUpToPowerOfTwo) {
  AudioRingBuffer rb;
  rb.init(1000, 2);
  EXPECT_EQ(rb.capacity(), 1024u);
  rb.init(1024, 2);
  EXPECT_EQ(rb.capacity(), 1024u);
  rb.init(1025, 2);
  EXPECT_EQ(rb.capacity(), 2048u);
  rb.init(1, 2);
  EXPECT_EQ(rb.capacity(), 1u);
}

TEST(AudioRingBufferInit, ChannelsAndInitialState) {
  AudioRingBuffer rb;
  rb.init(512, 4);
  EXPECT_EQ(rb.channels(), 4u);
  EXPECT_EQ(rb.available(), 0u);
  EXPECT_EQ(rb.space(), rb.capacity());
}

TEST(AudioRingBufferInit, RejectsZeroChannels) {
  AudioRingBuffer rb;
  ASSERT_TRUE(rb.init(1024, 2));
  EXPECT_EQ(rb.capacity(), 1024u);
  EXPECT_EQ(rb.channels(), 2u);

  // Zero channels is a contract violation, not an empty buffer: reject it and
  // leave the object in a clearly unusable state rather than accepting writes
  // that could never be read back.
  EXPECT_FALSE(rb.init(1024, 0));
  EXPECT_EQ(rb.capacity(), 0u);
  EXPECT_EQ(rb.channels(), 0u);
  EXPECT_EQ(rb.available(), 0u);
  EXPECT_EQ(rb.space(), 0u);
  const float sample = 0.0f;
  EXPECT_FALSE(rb.write(&sample, 1));
}

// ===========================================================================
// float
// ===========================================================================

TEST(AudioRingBufferFloat, WriteReadRoundTrip) {
  AudioRingBuffer rb;
  rb.init(1024, 2);

  std::vector<float> in(100 * 2);
  for (size_t i = 0; i < in.size(); ++i) in[i] = std::sin(0.01f * static_cast<float>(i));

  ASSERT_TRUE(rb.write(in.data(), 100));
  EXPECT_EQ(rb.available(), 100u);

  std::vector<float> out(100 * 2);
  EXPECT_EQ(rb.read(out.data(), 100), 100u);
  EXPECT_EQ(in, out);
  EXPECT_EQ(rb.available(), 0u);
}

TEST(AudioRingBufferFloat, WriteFailsWhenFull) {
  AudioRingBuffer rb;
  rb.init(8, 1);

  std::vector<float> in(8, 0.5f);
  ASSERT_TRUE(rb.write(in.data(), 8));
  EXPECT_EQ(rb.space(), 0u);

  float one = 1.0f;
  EXPECT_FALSE(rb.write(&one, 1));
}

TEST(AudioRingBufferFloat, ReadReturnsAvailableOnly) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<float> in(5, 1.0f);
  ASSERT_TRUE(rb.write(in.data(), 5));

  std::vector<float> out(10, 0.0f);
  EXPECT_EQ(rb.read(out.data(), 10), 5u);
}

TEST(AudioRingBufferFloat, Wraparound) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<float> a(12);
  for (size_t i = 0; i < 12; ++i) a[i] = static_cast<float>(i);
  ASSERT_TRUE(rb.write(a.data(), 12));

  std::vector<float> dump(8);
  ASSERT_EQ(rb.read(dump.data(), 8), 8u);

  std::vector<float> b = { 100, 101, 102, 103, 104, 105, 106, 107 };
  ASSERT_TRUE(rb.write(b.data(), 8));

  std::vector<float> out(12);
  ASSERT_EQ(rb.read(out.data(), 12), 12u);
  EXPECT_FLOAT_EQ(out[0], 8.f);
  EXPECT_FLOAT_EQ(out[1], 9.f);
  EXPECT_FLOAT_EQ(out[2], 10.f);
  EXPECT_FLOAT_EQ(out[3], 11.f);
  for (int i = 0; i < 8; ++i)
    EXPECT_FLOAT_EQ(out[4 + i], static_cast<float>(100 + i));
}

// ===========================================================================
// Int16
// ===========================================================================

TEST(AudioRingBufferInt16, RoundTripExact) {
  AudioRingBuffer rb;
  rb.init(1024, 2);

  const size_t frames = 150;
  auto in = randomInt16(frames * 2, 42);

  ASSERT_TRUE(rb.writeInt16(in.data(), frames));

  std::vector<int16_t> out(frames * 2, 0);
  EXPECT_EQ(rb.readInt16(out.data(), frames), frames);
  EXPECT_EQ(in, out);
}

TEST(AudioRingBufferInt16, ExtremesExact) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<int16_t> in = {
    std::numeric_limits<int16_t>::min(),
    std::numeric_limits<int16_t>::max(),
    0, 1, -1
  };
  ASSERT_TRUE(rb.writeInt16(in.data(), in.size()));

  std::vector<int16_t> out(in.size(), 0);
  ASSERT_EQ(rb.readInt16(out.data(), in.size()), in.size());
  EXPECT_EQ(in, out);
}

TEST(AudioRingBufferInt16, SaturationOnOverRangeFloat) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<float> in = { 2.0f, -2.0f, 1.5f, -1.5f, 1.0f, -1.0f };
  ASSERT_TRUE(rb.write(in.data(), in.size()));

  std::vector<int16_t> out(in.size(), 0);
  ASSERT_EQ(rb.readInt16(out.data(), in.size()), in.size());
  EXPECT_EQ(out[0], 32767);
  EXPECT_EQ(out[1], -32768);
  EXPECT_EQ(out[2], 32767);
  EXPECT_EQ(out[3], -32768);
  EXPECT_EQ(out[4], 32767);
  EXPECT_EQ(out[5], -32768);
}

// ===========================================================================
// Int24
// ===========================================================================

TEST(AudioRingBufferInt24, RoundTripExact) {
  AudioRingBuffer rb;
  rb.init(1024, 2);

  const size_t frames = 200;
  const size_t nsamples = frames * 2;
  auto values = randomInt24Values(nsamples, 7);
  std::vector<uint8_t> in(nsamples * 3);
  for (size_t i = 0; i < nsamples; ++i) writePacked24(in, i, values[i]);

  ASSERT_TRUE(rb.writeInt24(in.data(), frames));

  std::vector<uint8_t> out(nsamples * 3, 0);
  ASSERT_EQ(rb.readInt24(out.data(), frames), frames);
  EXPECT_EQ(in, out);
}

TEST(AudioRingBufferInt24, ExtremesExact) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<uint8_t> in(5 * 3);
  writePacked24(in, 0, -8388608);
  writePacked24(in, 1, 8388607);
  writePacked24(in, 2, 0);
  writePacked24(in, 3, 1);
  writePacked24(in, 4, -1);
  ASSERT_TRUE(rb.writeInt24(in.data(), 5));

  std::vector<uint8_t> out(5 * 3, 0);
  ASSERT_EQ(rb.readInt24(out.data(), 5), 5u);
  EXPECT_EQ(in, out);
}

TEST(AudioRingBufferInt24, LittleEndianByteOrder) {
  AudioRingBuffer rb;
  rb.init(4, 1);

  std::vector<uint8_t> in(3);
  writePacked24(in, 0, 0x123456);
  ASSERT_TRUE(rb.writeInt24(in.data(), 1));

  std::vector<uint8_t> out(3, 0);
  ASSERT_EQ(rb.readInt24(out.data(), 1), 1u);
  EXPECT_EQ(out[0], 0x56);
  EXPECT_EQ(out[1], 0x34);
  EXPECT_EQ(out[2], 0x12);
}

TEST(AudioRingBufferInt24, TypedPointerOverload) {
  AudioRingBuffer rb;
  rb.init(8, 1);

  std::vector<AudioRingBuffer::Packed24> in(4);
  writePacked24(*reinterpret_cast<std::vector<uint8_t>*>(&in), 0, 0x010203);

  in[0].b[0] = 0x03;
  in[0].b[1] = 0x02;
  in[0].b[2] = 0x01; // 0x010203
  in[1].b[0] = 0xFF;
  in[1].b[1] = 0xFF;
  in[1].b[2] = 0xFF; // -1
  in[2].b[0] = 0x00;
  in[2].b[1] = 0x00;
  in[2].b[2] = 0x80; // -8388608
  in[3].b[0] = 0xFF;
  in[3].b[1] = 0xFF;
  in[3].b[2] = 0x7F; // 8388607

  ASSERT_TRUE(rb.writeInt24(in.data(), in.size()));

  std::vector<AudioRingBuffer::Packed24> out(in.size());
  ASSERT_EQ(rb.readInt24(out.data(), in.size()), in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(out[i].b[0], in[i].b[0]) << i;
    EXPECT_EQ(out[i].b[1], in[i].b[1]) << i;
    EXPECT_EQ(out[i].b[2], in[i].b[2]) << i;
  }
}

// ===========================================================================
// Int32
// ===========================================================================

TEST(AudioRingBufferInt32, RoundTripWithinFloatPrecision) {
  AudioRingBuffer rb;
  rb.init(1024, 1);

  std::vector<int32_t> in = {
    0, 1, -1,
    1000000, -1000000,
    std::numeric_limits<int32_t>::min(),
    std::numeric_limits<int32_t>::max()
  };
  ASSERT_TRUE(rb.writeInt32(in.data(), in.size()));

  std::vector<int32_t> out(in.size(), 0);
  ASSERT_EQ(rb.readInt32(out.data(), in.size()), in.size());

  for (size_t i = 0; i < in.size(); ++i) {
    int64_t diff = std::llabs(static_cast<int64_t>(in[i])
                              - static_cast<int64_t>(out[i]));
    EXPECT_LE(diff, 256) << "idx=" << i
                         << " in=" << in[i] << " out=" << out[i];
  }
}

TEST(AudioRingBufferInt32, ExtremesSaturate) {
  AudioRingBuffer rb;
  rb.init(4, 1);

  std::vector<float> in = { 2.0f, -2.0f, 1.0f, -1.0f };
  ASSERT_TRUE(rb.write(in.data(), 4));

  std::vector<int32_t> out(4, 0);
  ASSERT_EQ(rb.readInt32(out.data(), 4), 4u);
  EXPECT_EQ(out[0], std::numeric_limits<int32_t>::max());
  EXPECT_EQ(out[1], std::numeric_limits<int32_t>::min());
  EXPECT_EQ(out[2], std::numeric_limits<int32_t>::max());
  EXPECT_EQ(out[3], std::numeric_limits<int32_t>::min());
}

// ===========================================================================
// Converts
// ===========================================================================

TEST(AudioRingBufferCrossFormat, Int16ToFloat) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<int16_t> in = { 0, 16384, -16384, 32767, -32768 };
  ASSERT_TRUE(rb.writeInt16(in.data(), in.size()));

  std::vector<float> out(in.size(), 0.f);
  ASSERT_EQ(rb.read(out.data(), in.size()), in.size());

  EXPECT_FLOAT_EQ(out[0], 0.0f);
  EXPECT_FLOAT_EQ(out[1], 0.5f);
  EXPECT_FLOAT_EQ(out[2], -0.5f);
  EXPECT_NEAR(out[3], 1.0f - 1.0f / 32768.0f, 1e-6f);
  EXPECT_FLOAT_EQ(out[4], -1.0f);
}

TEST(AudioRingBufferCrossFormat, Int16ToInt24) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<int16_t> in = { 0, 16384, -16384, 32767, -32768 };
  ASSERT_TRUE(rb.writeInt16(in.data(), in.size()));

  std::vector<uint8_t> out(in.size() * 3, 0);
  ASSERT_EQ(rb.readInt24(out.data(), in.size()), in.size());

  EXPECT_EQ(readPacked24(out, 0), 0);
  EXPECT_EQ(readPacked24(out, 1), 16384 * 256);
  EXPECT_EQ(readPacked24(out, 2), -16384 * 256);
  EXPECT_EQ(readPacked24(out, 3), 32767 * 256);
  EXPECT_EQ(readPacked24(out, 4), -8388608);
}

TEST(AudioRingBufferCrossFormat, Int32ToInt16) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<int32_t> in = {
    0,
    std::numeric_limits<int32_t>::max(),
    std::numeric_limits<int32_t>::min(),
    1 << 30
  };
  ASSERT_TRUE(rb.writeInt32(in.data(), in.size()));

  std::vector<int16_t> out(in.size(), 0);
  ASSERT_EQ(rb.readInt16(out.data(), in.size()), in.size());

  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[1], 32767);
  EXPECT_EQ(out[2], -32768);
  EXPECT_EQ(out[3], 16384);
}

// ===========================================================================
// Multi-channel
// ===========================================================================

TEST(AudioRingBufferChannels, StereoFloat) {
  AudioRingBuffer rb;
  rb.init(64, 2);

  std::vector<float> in = {
    0.1f, 0.2f, // frame 0
    0.3f, 0.4f, // frame 1
    0.5f, 0.6f, // frame 2
  };
  ASSERT_TRUE(rb.write(in.data(), 3));

  std::vector<float> out(6, 0.f);
  ASSERT_EQ(rb.read(out.data(), 3), 3u);
  EXPECT_EQ(in, out);
}

TEST(AudioRingBufferChannels, FourChannelsInt16) {
  AudioRingBuffer rb;
  rb.init(64, 4);

  std::vector<int16_t> in = {
    100, 200, 300, 400,
    -100, -200, -300, -400,
  };
  ASSERT_TRUE(rb.writeInt16(in.data(), 2));

  std::vector<int16_t> out(8, 0);
  ASSERT_EQ(rb.readInt16(out.data(), 2), 2u);
  EXPECT_EQ(in, out);
}

// ===========================================================================
// block read write / reset
// ===========================================================================

TEST(AudioRingBuffer, PartialFrameBlock) {
  AudioRingBuffer rb;
  rb.init(64, 2);

  std::vector<float> in(10);
  for (size_t i = 0; i < 10; ++i) in[i] = static_cast<float>(i);
  ASSERT_TRUE(rb.write(in.data(), 5));

  std::vector<float> chunk(4, 0.f);
  EXPECT_EQ(rb.read(chunk.data(), 2), 2u);
  EXPECT_EQ(rb.available(), 3u);
  EXPECT_EQ(rb.read(chunk.data(), 2), 2u);
  EXPECT_EQ(rb.read(chunk.data(), 2), 1u);
  EXPECT_EQ(rb.available(), 0u);
}

TEST(AudioRingBuffer, ResetClearsState) {
  AudioRingBuffer rb;
  rb.init(64, 2);

  std::vector<float> in(32 * 2, 0.25f);
  ASSERT_TRUE(rb.write(in.data(), 32));
  EXPECT_EQ(rb.available(), 32u);

  rb.reset();
  EXPECT_EQ(rb.available(), 0u);
  EXPECT_EQ(rb.space(), rb.capacity());
}

// ===========================================================================
// Large-scale randomized cross-testing
// ===========================================================================

TEST(AudioRingBuffer, RandomizedInterleavedChunks) {
  AudioRingBuffer rb;
  rb.init(256, 2);

  std::mt19937 rng(123);
  std::uniform_real_distribution<float> dist(-1.f, 1.f);
  std::uniform_int_distribution<int> chunkDist(1, 20);

  std::vector<float> reference;
  std::vector<float> scratch(64);
  std::vector<float> out(64);

  for (int iter = 0; iter < 200; ++iter) {
    const int frames = chunkDist(rng);
    std::vector<float> chunk(frames * 2);
    for (auto& x: chunk) x = dist(rng);

    ASSERT_TRUE(rb.write(chunk.data(), frames));
    reference.insert(reference.end(), chunk.begin(), chunk.end());

    while (rb.available() > 0) {
      size_t got = rb.read(out.data(), 8); // 8 frames = 16 samples
      ASSERT_GT(got, 0u);
      for (size_t i = 0; i < got * 2; ++i) {
        ASSERT_FALSE(reference.empty());
        EXPECT_FLOAT_EQ(reference.front(), out[i]);
        reference.erase(reference.begin());
      }
    }
  }
}

// ===========================================================================
// Discard
// ===========================================================================

TEST(AudioRingBufferDiscard, DiscardOldestFrames) {
  AudioRingBuffer rb;
  rb.init(16, 1);

  std::vector<float> in = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f};
  ASSERT_TRUE(rb.write(in.data(), in.size()));
  EXPECT_EQ(rb.available(), 8u);

  EXPECT_EQ(rb.discard(3), 3u);
  EXPECT_EQ(rb.available(), 5u);

  std::vector<float> out(5, 0.f);
  ASSERT_EQ(rb.read(out.data(), out.size()), out.size());
  EXPECT_EQ(out, (std::vector<float>{3.f, 4.f, 5.f, 6.f, 7.f}));
}

TEST(AudioRingBufferDiscard, DiscardMoreThanAvailable) {
  AudioRingBuffer rb;
  rb.init(8, 1);

  std::vector<float> in = {1.f, 2.f, 3.f};
  ASSERT_TRUE(rb.write(in.data(), in.size()));
  EXPECT_EQ(rb.available(), 3u);

  EXPECT_EQ(rb.discard(100), 3u);
  EXPECT_EQ(rb.available(), 0u);
  EXPECT_EQ(rb.space(), rb.capacity());
}

TEST(AudioRingBufferDiscard, FreesSpaceForWrite) {
  AudioRingBuffer rb;
  rb.init(8, 1);

  std::vector<float> in(8, 1.f);
  ASSERT_TRUE(rb.write(in.data(), in.size()));
  EXPECT_EQ(rb.space(), 0u);

  float next = 2.f;
  EXPECT_FALSE(rb.write(&next, 1));

  EXPECT_EQ(rb.discard(1), 1u);
  EXPECT_EQ(rb.space(), 1u);
  EXPECT_TRUE(rb.write(&next, 1));
  EXPECT_EQ(rb.available(), 8u);

  std::vector<float> out(8, 0.f);
  ASSERT_EQ(rb.read(out.data(), out.size()), out.size());
  for (size_t i = 0; i < 7; ++i) {
    EXPECT_FLOAT_EQ(out[i], 1.f);
  }
  EXPECT_FLOAT_EQ(out[7], 2.f);
}

TEST(AudioRingBufferDiscard, MultiChannelDiscardFramesNotSamples) {
  AudioRingBuffer rb;
  rb.init(16, 2);

  std::vector<float> in = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f};
  ASSERT_TRUE(rb.write(in.data(), 4));
  EXPECT_EQ(rb.available(), 4u);

  EXPECT_EQ(rb.discard(2), 2u);
  EXPECT_EQ(rb.available(), 2u);

  std::vector<float> out(4, 0.f);
  ASSERT_EQ(rb.read(out.data(), 2), 2u);
  EXPECT_EQ(out, (std::vector<float>{4.f, 5.f, 6.f, 7.f}));
}

TEST(AudioRingBufferDiscard, Wraparound) {
  AudioRingBuffer rb;
  rb.init(8, 1);

  std::vector<float> a = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f};
  ASSERT_TRUE(rb.write(a.data(), a.size()));
  ASSERT_EQ(rb.discard(5), 5u);

  std::vector<float> b = {10.f, 11.f, 12.f, 13.f, 14.f};
  ASSERT_TRUE(rb.write(b.data(), b.size()));
  EXPECT_EQ(rb.available(), 8u);

  std::vector<float> out(8, 0.f);
  ASSERT_EQ(rb.read(out.data(), out.size()), out.size());
  EXPECT_EQ(out, (std::vector<float>{5.f, 6.f, 7.f, 10.f, 11.f, 12.f, 13.f, 14.f}));
}
