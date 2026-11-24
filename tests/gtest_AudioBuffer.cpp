/*
 * Copyright (C) 2026 The Coral Project
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

/*
 * Unit tests for coral::AudioPCMBuffer and coral::AudioCompressedBuffer.
 *
 * NOTE:
 *   The `makeXxxFormat()` helpers at the top of this file are the ONLY place
 *   that needs to be adapted to your actual AudioFormat API. Every test below
 *   treats them as the source of truth for format configuration.
 */

#include <gtest/gtest.h>

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioTypes.h>
#include <coral/audio/AudioUtils.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace coral {
namespace {

constexpr uint64_t kSampleRate = 44100;
constexpr uint32_t kChannels = 2;

AudioFormat makeFloat32(const uint32_t channels, const bool interleaved) {
  return AudioFormat::InitWithCommonFormat(AudioSampleFormat::kFloat,
    kSampleRate,
    channels,
    interleaved).value();
}

AudioFormat makeInt16(const uint32_t channels, const bool interleaved) {
  return AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16,
    kSampleRate,
    channels,
    interleaved).value();
}

AudioFormat makeInt24(const uint32_t channels, const bool interleaved) {
  return AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt24,
    kSampleRate,
    channels,
    interleaved).value();
}

AudioFormat makeInt32(const uint32_t channels, const bool interleaved) {
  return AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt32,
    kSampleRate,
    channels,
    interleaved).value();
}

AudioFormat makeCompressedFormat() {
  constexpr AudioBasicDescription abd = {
    .mSampleRate = kSampleRate,
    .mFormatType = AudioFormatType::kAudioFormatTypeMpeg4AAC,
    .mFormatFlags = AudioFormatFlags::kLinearPCMFormatFlagsAreAllClear,
    .mBytesPerPacket = 0, // CBR: 384, VBR: 0
    .mFramesPerPacket = 1024, // AAC: 1024 MP3: 1152
    .mBytesPerFrame = 0, // 0 for compressed frame
    .mChannelsPerFrame = kChannels,
    .mBitsPerChannel = 0, // 0 for compressed frame.
  };
  return AudioFormat::InitWithStreamDescription(abd, std::nullopt).value();
}

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------

void writePacked24LE(uint8_t* p, int32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
}

int32_t readPacked24LE(const uint8_t* p) {
  uint32_t u = static_cast<uint32_t>(p[0])
               | (static_cast<uint32_t>(p[1]) << 8)
               | (static_cast<uint32_t>(p[2]) << 16);
  if (u & 0x00800000u) u |= 0xFF000000u;
  return static_cast<int32_t>(u);
}

} // namespace anonymous

// ===========================================================================
// AudioPCMBuffer -- construction
// ===========================================================================

TEST(AudioPCMBufferInit, SucceedsOnValidFormat) {
  const auto fmt = makeFloat32(2, /*interleaved=*/true);
  const auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 1024);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(buf->getFrameCapacity(), 1024u);
  EXPECT_EQ(buf->getFrameLength(), 0u);
}

TEST(AudioPCMBufferInit, RawByteSizeMatchesInterleavedStorage) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, true), 100);
  ASSERT_NE(buf, nullptr);
  // 100 frames × 2 ch × 4 bytes
  EXPECT_EQ(buf->rawByteSize(), 800u);
}

TEST(AudioPCMBufferInit, RawByteSizeMatchesPlanarStorage) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 100);
  ASSERT_NE(buf, nullptr);
  // 100 frames × 4 bytes × 2 ch
  EXPECT_EQ(buf->rawByteSize(), 800u);
}

TEST(AudioPCMBufferInit, RawDataIsZeroInitialised) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 64);
  ASSERT_NE(buf, nullptr);
  const auto* p = static_cast<const uint8_t*>(buf->rawData());
  for (size_t i = 0; i < buf->rawByteSize(); ++i)
    ASSERT_EQ(p[i], 0) << i;
}

// ===========================================================================
// AudioPCMBuffer -- frame length / stride
// ===========================================================================

TEST(AudioPCMBufferFrameLength, ClampsToCapacity) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 128);
  buf->setFrameLength(1000);
  EXPECT_EQ(buf->getFrameLength(), 128u);

  buf->setFrameLength(50);
  EXPECT_EQ(buf->getFrameLength(), 50u);
}

TEST(AudioPCMBufferFrameLength, UpdatesDataByteSizeInterleaved) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, true), 128);
  buf->setFrameLength(32);
  const auto* list = buf->audioBufferList();
  ASSERT_EQ(list->numberOfBuffers(), 1u);
  // 32 frames × 2 ch × 4 bytes = 256
  EXPECT_EQ(list->getBuffer(0)->mDataByteSize, 256u);
}

TEST(AudioPCMBufferFrameLength, UpdatesDataByteSizePlanar) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 128);
  buf->setFrameLength(32);
  const auto* list = buf->audioBufferList();
  ASSERT_EQ(list->numberOfBuffers(), 2u);
  for (size_t c = 0; c < list->numberOfBuffers(); ++c) {
    EXPECT_EQ(list->getBuffer(c)->mDataByteSize, 32u * 4u) << c;
  }
}

TEST(AudioPCMBufferStride, IsOneForPlanar) {
  const auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 16);
  EXPECT_EQ(buf->getStride(), 1u);
}

TEST(AudioPCMBufferStride, EqualsChannelCountForInterleaved) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, true), 16);
  EXPECT_EQ(buf->getStride(), 2u);
}

// ===========================================================================
// AudioPCMBuffer -- floatChannelData
// ===========================================================================

TEST(AudioPCMBufferFloatData, ReturnsNullForNonFloatFormat) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt16(2, false), 32);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(buf->floatChannelData(), nullptr);
}

TEST(AudioPCMBufferFloatData, InterleavedPointerOffsets) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, true), 4);
  ASSERT_NE(buf, nullptr);
  buf->setFrameLength(4);
  float** data = buf->floatChannelData();
  ASSERT_NE(data, nullptr);

  // Interleaved storage base pointer
  float* base = static_cast<float*>(buf->mutableAudioBufferList()->getBuffer(0)->mData);
  for (int c = 0; c < 2; ++c) {
    EXPECT_EQ(data[c], base + c) << c;
  }
}

TEST(AudioPCMBufferFloatData, PlanarPointerPointsToEachChannel) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 4);
  ASSERT_NE(buf, nullptr);
  buf->setFrameLength(4);
  float** data = buf->floatChannelData();
  ASSERT_NE(data, nullptr);
  for (size_t c = 0; c < 2; ++c) {
    EXPECT_EQ(data[c], static_cast<float*>(buf->mutableAudioBufferList()->getBuffer(c)->mData)) << c;
  }
}

TEST(AudioPCMBufferFloatData, WriteAndReadBackInterleaved) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, true), 8);
  buf->setFrameLength(8);
  float** d = buf->floatChannelData();
  const uint32_t stride = buf->getStride();

  // pattern: d[ch][f * stride] = (f * 10 + ch)
  for (uint32_t f = 0; f < 8; ++f) for (uint32_t c = 0; c < 2; ++c) d[c][f * stride] = static_cast<float>(f * 10 + c);

  for (uint32_t f = 0; f < 8; ++f)
    for (uint32_t c = 0; c < 2; ++c)
      EXPECT_FLOAT_EQ(d[c][f * stride], static_cast<float>(f * 10 + c));
}

TEST(AudioPCMBufferFloatData, WriteAndReadBackPlanar) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 8);
  buf->setFrameLength(8);
  float** d = buf->floatChannelData();

  for (uint32_t f = 0; f < 8; ++f) for (uint32_t c = 0; c < 2; ++c) d[c][f] = static_cast<float>(f * 10 + c);

  for (uint32_t f = 0; f < 8; ++f)
    for (uint32_t c = 0; c < 2; ++c)
      EXPECT_FLOAT_EQ(d[c][f], static_cast<float>(f * 10 + c));
}

// ===========================================================================
// AudioPCMBuffer -- int16 / int32 channel data
// ===========================================================================

TEST(AudioPCMBufferInt16Data, NullForFloatFormat) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 32);
  EXPECT_EQ(buf->int16ChannelData(), nullptr);
}

TEST(AudioPCMBufferInt16Data, PlanarRoundTrip) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt16(2, false), 8);
  buf->setFrameLength(8);
  int16_t** d = buf->int16ChannelData();
  ASSERT_NE(d, nullptr);

  for (int f = 0; f < 8; ++f) {
    d[0][f] = int16_t(1000 + f);
    d[1][f] = int16_t(-1000 - f);
  }
  for (int f = 0; f < 8; ++f) {
    EXPECT_EQ(d[0][f], 1000 + f);
    EXPECT_EQ(d[1][f], -1000 - f);
  }
}

TEST(AudioPCMBufferInt32Data, NullForInt16Format) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt16(2, true), 32);
  EXPECT_EQ(buf->int32ChannelData(), nullptr);
}

TEST(AudioPCMBufferInt32Data, InterleavedRoundTrip) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt32(2, true), 4);
  buf->setFrameLength(4);
  int32_t** d = buf->int32ChannelData();
  const uint32_t stride = buf->getStride();

  for (uint32_t f = 0; f < 4; ++f)
    for (uint32_t c = 0; c < 2; ++
         c)
      d[c][f * stride] = static_cast<int32_t>(f * 100 + c);

  for (uint32_t f = 0; f < 4; ++f)
    for (uint32_t c = 0; c < 2; ++c)
      EXPECT_EQ(d[c][f * stride], static_cast<int32_t>(f * 100 + c));
}

// ===========================================================================
// AudioPCMBuffer -- toFloatChannelData
// ===========================================================================

TEST(AudioPCMBufferToFloat, EmptyWhenFrameLengthZero) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 16);
  auto v = buf->toFloatChannelData();
  ASSERT_EQ(v.size(), 2u);
  EXPECT_TRUE(v[0].empty());
  EXPECT_TRUE(v[1].empty());
}

TEST(AudioPCMBufferToFloat, FloatPlanarIdentity) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, false), 4);
  buf->setFrameLength(4);
  float** d = buf->floatChannelData();
  for (int f = 0; f < 4; ++f) {
    d[0][f] = 0.1f * f;
    d[1][f] = -0.2f * f;
  }

  auto v = buf->toFloatChannelData();
  ASSERT_EQ(v.size(), 2u);
  for (int f = 0; f < 4; ++f) {
    EXPECT_FLOAT_EQ(v[0][f], 0.1f * f);
    EXPECT_FLOAT_EQ(v[1][f], -0.2f * f);
  }
}

TEST(AudioPCMBufferToFloat, FloatInterleavedDeinterleaves) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeFloat32(2, true), 4);
  buf->setFrameLength(4);
  auto* base = static_cast<float*>(buf->mutableAudioBufferList()->getBuffer(0)->mData);
  // Interleaved stream: [f0c0, f0c1, f1c0, f1c1, ...]
  for (int i = 0; i < 8; ++i) base[i] = static_cast<float>(i);

  auto v = buf->toFloatChannelData();
  ASSERT_EQ(v.size(), 2u);
  ASSERT_EQ(v[0].size(), 4u);
  // ch0 = even indices, ch1 = odd indices
  for (int f = 0; f < 4; ++f) {
    EXPECT_FLOAT_EQ(v[0][f], static_cast<float>(f * 2));
    EXPECT_FLOAT_EQ(v[1][f], static_cast<float>(f * 2 + 1));
  }
}

TEST(AudioPCMBufferToFloat, Int16ScalesByInv32768) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt16(2, false), 4);
  buf->setFrameLength(4);
  int16_t** d = buf->int16ChannelData();
  d[0][0] = 0;
  d[0][1] = 16384;
  d[0][2] = -16384;
  d[0][3] = 32767;
  d[1][0] = -32768;
  d[1][1] = 1;
  d[1][2] = -1;
  d[1][3] = 100;

  auto v = buf->toFloatChannelData();
  ASSERT_EQ(v.size(), 2u);
  EXPECT_FLOAT_EQ(v[0][0], 0.0f);
  EXPECT_FLOAT_EQ(v[0][1], 0.5f);
  EXPECT_FLOAT_EQ(v[0][2], -0.5f);
  EXPECT_NEAR(v[0][3], 32767.0f / 32768.0f, 1e-7f);
  EXPECT_FLOAT_EQ(v[1][0], -1.0f);
  EXPECT_NEAR(v[1][1], 1.0f / 32768.0f, 1e-7f);
  EXPECT_NEAR(v[1][2], -1.0f / 32768.0f, 1e-7f);
}

TEST(AudioPCMBufferToFloat, Int16Interleaved) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt16(2, true), 4);
  buf->setFrameLength(4);
  auto* base = static_cast<int16_t*>(buf->mutableAudioBufferList()->getBuffer(0)->mData);
  int16_t src[8] = { 0, 100, 200, 300, -100, -200, -300, -400 };
  std::memcpy(base, src, sizeof(src));

  auto v = buf->toFloatChannelData();
  EXPECT_FLOAT_EQ(v[0][0], 0.0f);
  EXPECT_FLOAT_EQ(v[0][1], 200.0f / 32768.0f);
  EXPECT_FLOAT_EQ(v[0][2], -100.0f / 32768.0f);
  EXPECT_FLOAT_EQ(v[0][3], -300.0f / 32768.0f);
  EXPECT_FLOAT_EQ(v[1][0], 100.0f / 32768.0f);
  EXPECT_FLOAT_EQ(v[1][1], 300.0f / 32768.0f);
  EXPECT_FLOAT_EQ(v[1][2], -200.0f / 32768.0f);
  EXPECT_FLOAT_EQ(v[1][3], -400.0f / 32768.0f);
}

TEST(AudioPCMBufferToFloat, Int24PlanarExtremesAndSign) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt24(2, false), 4);
  buf->setFrameLength(4);
  auto* list = buf->mutableAudioBufferList();
  uint8_t* c0 = static_cast<uint8_t*>(list->getBuffer(0)->mData);
  uint8_t* c1 = static_cast<uint8_t*>(list->getBuffer(1)->mData);

  writePacked24LE(c0 + 0 * 3, 0);
  writePacked24LE(c0 + 1 * 3, 8388607);
  writePacked24LE(c0 + 2 * 3, -8388608);
  writePacked24LE(c0 + 3 * 3, 1);

  writePacked24LE(c1 + 0 * 3, -1);
  writePacked24LE(c1 + 1 * 3, 8388606);
  writePacked24LE(c1 + 2 * 3, -8388607);
  writePacked24LE(c1 + 3 * 3, 0);

  auto v = buf->toFloatChannelData();
  EXPECT_FLOAT_EQ(v[0][0], 0.0f);
  EXPECT_NEAR(v[0][1], 1.0f - 1.0f / 8388608.0f, 1e-7f);
  EXPECT_FLOAT_EQ(v[0][2], -1.0f);
  EXPECT_NEAR(v[0][3], 1.0f / 8388608.0f, 1e-7f);
  EXPECT_NEAR(v[1][0], -1.0f / 8388608.0f, 1e-7f);
  EXPECT_NEAR(v[1][1], (8388606.0f / 8388608.0f), 1e-6f);
  EXPECT_NEAR(v[1][2], -(8388607.0f / 8388608.0f), 1e-6f);
  EXPECT_FLOAT_EQ(v[1][3], 0.0f);
}

TEST(AudioPCMBufferToFloat, Int24InterleavedSign) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt24(2, true), 3);
  buf->setFrameLength(3);
  uint8_t* base = static_cast<uint8_t*>(buf->mutableAudioBufferList()->getBuffer(0)->mData);
  // frames: (ch0, ch1)
  writePacked24LE(base + 0 * 6 + 0, 100);
  writePacked24LE(base + 0 * 6 + 3, -100);
  writePacked24LE(base + 1 * 6 + 0, 8388607);
  writePacked24LE(base + 1 * 6 + 3, -8388608);
  writePacked24LE(base + 2 * 6 + 0, 0);
  writePacked24LE(base + 2 * 6 + 3, 1);

  auto v = buf->toFloatChannelData();
  EXPECT_NEAR(v[0][0], 100.0f / 8388608.0f, 1e-7f);
  EXPECT_NEAR(v[1][0], -100.0f / 8388608.0f, 1e-7f);
  EXPECT_NEAR(v[0][1], 1.0f - 1.0f / 8388608.0f, 1e-7f);
  EXPECT_FLOAT_EQ(v[1][1], -1.0f);
  EXPECT_FLOAT_EQ(v[0][2], 0.0f);
  EXPECT_NEAR(v[1][2], 1.0f / 8388608.0f, 1e-7f);
}

TEST(AudioPCMBufferToFloat, Int32SignedConversion) {
  auto buf = AudioPCMBuffer::InitWithPCMFormat(makeInt32(2, false), 2);
  buf->setFrameLength(2);
  int32_t** d = buf->int32ChannelData();
  d[0][0] = 0;
  d[0][1] = std::numeric_limits<int32_t>::max();
  d[1][0] = std::numeric_limits<int32_t>::min();
  d[1][1] = -(1 << 30);

  auto v = buf->toFloatChannelData();
  EXPECT_FLOAT_EQ(v[0][0], 0.0f);
  EXPECT_NEAR(v[0][1], 1.0f - 1.0f / 2147483648.0f, 1e-6f);
  EXPECT_FLOAT_EQ(v[1][0], -1.0f);
  EXPECT_FLOAT_EQ(v[1][1], -0.5f);
}

// ===========================================================================
// AudioCompressedBuffer -- construction
// ===========================================================================

TEST(AudioCompressedBufferInit, ReturnsNullForPCMFormat) {
  auto f = makeFloat32(2, false);
  EXPECT_EQ(AudioCompressedBuffer::InitWithFormat(f, 16), nullptr);
  EXPECT_EQ(AudioCompressedBuffer::InitWithFormat(f, 16, 4096), nullptr);
}

TEST(AudioCompressedBufferInit, ReturnsNullForZeroBytesPerFrame_Overload1) {
  auto fmt =
   AudioFormat::InitWithCommonFormat(AudioSampleFormat::kFloat, kSampleRate, kChannels, true);
  EXPECT_EQ(AudioCompressedBuffer::InitWithFormat(fmt.value(), 8), nullptr);
}

TEST(AudioCompressedBufferInit, SucceedsWithExplicitMaxPacketSize) {
  const auto fmt = makeCompressedFormat();
  const auto buf = AudioCompressedBuffer::InitWithFormat(fmt, /*packetCapacity=*/8,
    /*maxPacketSize=*/1024);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(buf->getPacketCapacity(), 8u);
  EXPECT_EQ(buf->getMaximumPacketSize(), 1024u);
  EXPECT_EQ(buf->getByteCapacity(), 8u * 1024u);
  EXPECT_EQ(buf->getByteLength(), 0u);
  EXPECT_EQ(buf->getPacketCount(), 1u); // per ctor initialiser
}

TEST(AudioCompressedBufferInit, SucceedsWithBytesPerFrameAsPacketSize) {
  auto fmt = makeCompressedFormat();
  auto buf = AudioCompressedBuffer::InitWithFormat(fmt, 4, 512);
  ASSERT_NE(buf, nullptr);
  EXPECT_EQ(buf->getMaximumPacketSize(), 512u);
  EXPECT_EQ(buf->getByteCapacity(), 4u * 512u);
}

TEST(AudioCompressedBufferInit, AllocatesPacketDescriptionsForVariableBR) {
  auto fmt = makeCompressedFormat();
  auto buf = AudioCompressedBuffer::InitWithFormat(fmt, 6, 1024);
  ASSERT_NE(buf, nullptr);
  EXPECT_NE(buf->mutablePacketDescriptions(), nullptr);
}

// ===========================================================================
// AudioCompressedBuffer -- packet / byte length
// ===========================================================================

TEST(AudioCompressedBufferPackets, SetClampsToCapacity) {
  auto fmt = makeCompressedFormat();
  auto buf = AudioCompressedBuffer::InitWithFormat(fmt, /*packetCapacity=*/4, 1024);
  buf->setPacketCount(1000);
  EXPECT_EQ(buf->getPacketCount(), 4u);

  buf->setPacketCount(2);
  EXPECT_EQ(buf->getPacketCount(), 2u);
}

TEST(AudioCompressedBufferBytes, SetClampsToCapacity) {
  auto fmt = makeCompressedFormat();
  auto buf = AudioCompressedBuffer::InitWithFormat(fmt, 4, 1024);
  buf->setByteLength(1000000);
  EXPECT_EQ(buf->getByteLength(), buf->getByteCapacity());

  buf->setByteLength(100);
  EXPECT_EQ(buf->getByteLength(), 100u);

  // mDataByteSize should be updated too
  EXPECT_EQ(buf->audioBufferList()->getBuffer(0)->mDataByteSize, 100u);
}

TEST(AudioCompressedBufferBytes, DataPointerIsNonNull) {
  auto fmt = makeCompressedFormat();
  auto buf = AudioCompressedBuffer::InitWithFormat(fmt, 2, 512);
  EXPECT_NE(buf->data(), nullptr);
  EXPECT_NE(buf->audioBufferList()->getBuffer(0)->mData, nullptr);
}

// ===========================================================================
// AudioCompressedBuffer -- reset
// ===========================================================================

TEST(AudioCompressedBufferReset, ClearsState) {
  auto fmt = makeCompressedFormat();
  auto buf = AudioCompressedBuffer::InitWithFormat(fmt, 4, 1024);
  buf->setPacketCount(3);
  buf->setByteLength(900);

  // Fill data with non-zero so we can verify it gets zeroed
  auto* p = static_cast<uint8_t*>(buf->data());
  for (uint32_t i = 0; i < buf->getByteLength(); ++i) p[i] = 0xAB;

  buf->reset();

  EXPECT_EQ(buf->getByteLength(), 0u);
  EXPECT_EQ(buf->getPacketCount(), 0u);
  EXPECT_EQ(buf->audioBufferList()->getBuffer(0)->mDataByteSize, 0u);

  for (uint32_t i = 0; i < buf->getByteCapacity(); ++i) {
    ASSERT_EQ(p[i], 0) << i;
  }
}

TEST(AudioCompressedBufferReset, NullsPacketDescriptions) {
  auto fmt = makeCompressedFormat();
  auto buf = AudioCompressedBuffer::InitWithFormat(fmt, 4, 1024);
  AudioPacketDescription* description = buf->mutablePacketDescriptions();
  ASSERT_NE(description, nullptr);
  for (int i = 0; i < 4; ++i) {
    description[i].mStartOffset = 123;
    description[i].mFramesInPacket = 45;
    description[i].mDataByteSize = 67;
  }

  buf->reset();
  const auto* after = buf->packetDescriptions();
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(after[i].mStartOffset, 0);
    EXPECT_EQ(after[i].mFramesInPacket, 0);
    EXPECT_EQ(after[i].mDataByteSize, 0);
  }
}

/*
 * The raw buffers AudioUtils copies to and from are interleaved, whatever the layout of the
 * AudioPCMBuffer, which is how a decoded file reaches a deinterleaved processing format.
 */
TEST(AudioUtilsCopy, DeinterleavesIntoPlanarBuffer) {
  const auto fmt = makeFloat32(kChannels, false);
  auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 4);
  ASSERT_NE(buf, nullptr);

  std::vector<float> interleaved{ 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f };
  ASSERT_EQ(AudioUtils::copyIntoPCMBuffer(interleaved.data(), 4, buf), 4u);
  ASSERT_EQ(buf->getFrameLength(), 4u);

  auto* const* planes = buf->floatChannelData();
  ASSERT_NE(planes, nullptr);
  for (AudioFrameCount f = 0; f < 4; ++f) {
    EXPECT_FLOAT_EQ(planes[0][f], interleaved[f * kChannels]);
    EXPECT_FLOAT_EQ(planes[1][f], interleaved[f * kChannels + 1]);
  }
}

TEST(AudioUtilsCopy, DeinterleavesInt16IntoPlanarBuffer) {
  const auto fmt = makeInt16(kChannels, false);
  auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 3);
  ASSERT_NE(buf, nullptr);

  std::vector<int16_t> interleaved{ 1, -1, 2, -2, 3, -3 };
  ASSERT_EQ(AudioUtils::copyIntoPCMBuffer(interleaved.data(), 3, buf), 3u);

  auto* const* planes = buf->int16ChannelData();
  ASSERT_NE(planes, nullptr);
  for (AudioFrameCount f = 0; f < 3; ++f) {
    EXPECT_EQ(planes[0][f], interleaved[f * kChannels]);
    EXPECT_EQ(planes[1][f], interleaved[f * kChannels + 1]);
  }
}

TEST(AudioUtilsCopy, InterleavesFromPlanarBuffer) {
  const auto fmt = makeFloat32(kChannels, false);
  auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 4);
  ASSERT_NE(buf, nullptr);

  std::vector<float> interleaved{ 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f };
  ASSERT_EQ(AudioUtils::copyIntoPCMBuffer(interleaved.data(), 4, buf), 4u);

  std::vector<float> roundTripped(interleaved.size(), 0.0f);
  ASSERT_EQ(AudioUtils::copyFromPCMBuffer(roundTripped.data(), 4, buf), 4u);
  for (size_t i = 0; i < interleaved.size(); ++i) {
    EXPECT_FLOAT_EQ(roundTripped[i], interleaved[i]);
  }
}

TEST(AudioUtilsCopy, AppliesVolumeWhileDeinterleaving) {
  const auto fmt = makeFloat32(kChannels, false);
  auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 2);
  ASSERT_NE(buf, nullptr);

  std::vector<float> interleaved{ 0.5f, -0.5f, 0.25f, -0.25f };
  ASSERT_EQ(AudioUtils::copyIntoPCMBuffer(interleaved.data(), 2, buf, 0.5f), 2u);

  auto* const* planes = buf->floatChannelData();
  ASSERT_NE(planes, nullptr);
  EXPECT_FLOAT_EQ(planes[0][0], 0.25f);
  EXPECT_FLOAT_EQ(planes[1][0], -0.25f);
  EXPECT_FLOAT_EQ(planes[0][1], 0.125f);
  EXPECT_FLOAT_EQ(planes[1][1], -0.125f);
}

TEST(AudioUtilsCopy, RoundTripsPacked24PlanarBuffer) {
  const auto fmt = makeInt24(kChannels, false);
  auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 2);
  ASSERT_NE(buf, nullptr);

  // 24 bit samples 0x001000, 0x002000, 0x003000 and 0x004000, packed little endian.
  std::vector<uint8_t> interleaved{
    0x00, 0x10, 0x00, 0x00, 0x20, 0x00,
    0x00, 0x30, 0x00, 0x00, 0x40, 0x00
  };
  ASSERT_EQ(AudioUtils::copyIntoPCMBuffer(interleaved.data(), 2, buf), 2u);

  std::vector<uint8_t> roundTripped(interleaved.size(), 0);
  ASSERT_EQ(AudioUtils::copyFromPCMBuffer(roundTripped.data(), 2, buf), 2u);
  EXPECT_EQ(roundTripped, interleaved);
}

TEST(AudioUtilsChangeVolume, ScalesPlanarBuffer) {
  const auto fmt = makeFloat32(kChannels, false);
  auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 2);
  ASSERT_NE(buf, nullptr);

  std::vector<float> interleaved{ 0.5f, -0.25f, 0.25f, -0.125f };
  ASSERT_EQ(AudioUtils::copyIntoPCMBuffer(interleaved.data(), 2, buf), 2u);
  AudioUtils::changeVolume(buf.get(), 0.5f);

  auto* const* planes = buf->floatChannelData();
  ASSERT_NE(planes, nullptr);
  EXPECT_FLOAT_EQ(planes[0][0], 0.25f);
  EXPECT_FLOAT_EQ(planes[1][0], -0.125f);
  EXPECT_FLOAT_EQ(planes[0][1], 0.125f);
  EXPECT_FLOAT_EQ(planes[1][1], -0.0625f);
}

TEST(AudioUtilsChangeVolume, ScalesInterleavedBufferWithoutMixingChannels) {
  const auto fmt = makeFloat32(kChannels, true);
  auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 2);
  ASSERT_NE(buf, nullptr);

  std::vector<float> interleaved{ 0.5f, -0.25f, 0.25f, -0.125f };
  ASSERT_EQ(AudioUtils::copyIntoPCMBuffer(interleaved.data(), 2, buf), 2u);
  AudioUtils::changeVolume(buf.get(), 2.0f);

  std::vector<float> out(interleaved.size(), 0.0f);
  ASSERT_EQ(AudioUtils::copyFromPCMBuffer(out.data(), 2, buf), 2u);
  EXPECT_FLOAT_EQ(out[0], 1.0f); // clamped
  EXPECT_FLOAT_EQ(out[1], -0.5f);
  EXPECT_FLOAT_EQ(out[2], 0.5f);
  EXPECT_FLOAT_EQ(out[3], -0.25f);
}

TEST(AudioUtilsChangeVolume, ZeroSilencesBothLayouts) {
  const std::vector<float> interleaved{ 0.5f, -0.25f, 0.25f, -0.125f };

  for (const auto interleavedLayout: { false, true }) {
    const auto fmt = makeFloat32(kChannels, interleavedLayout);
    auto buf = AudioPCMBuffer::InitWithPCMFormat(fmt, 2);
    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(AudioUtils::copyIntoPCMBuffer(
                const_cast<float*>(interleaved.data()), 2, buf), 2u);

    AudioUtils::changeVolume(buf.get(), 0.0f);
    std::vector<float> out(interleaved.size(), 1.0f);
    ASSERT_EQ(AudioUtils::copyFromPCMBuffer(out.data(), 2, buf), 2u);
    for (const auto sample: out) {
      EXPECT_FLOAT_EQ(sample, 0.0f);
    }
  }
}

TEST(AudioUtilsConvert, FloatToInt32AndBack) {
  EXPECT_EQ(AudioUtils::clamp32FromFloat(0.0f), 0);
  EXPECT_EQ(AudioUtils::clamp32FromFloat(0.5f), 1073741824);
  EXPECT_EQ(AudioUtils::clamp32FromFloat(-0.5f), -1073741824);
  EXPECT_EQ(AudioUtils::clamp32FromFloat(1.0f), std::numeric_limits<int32_t>::max());
  EXPECT_EQ(AudioUtils::clamp32FromFloat(-1.0f), std::numeric_limits<int32_t>::min());

  EXPECT_FLOAT_EQ(float_from_i32(1073741824), 0.5f);
  EXPECT_FLOAT_EQ(float_from_i32(-1073741824), -0.5f);
}

TEST(AudioUtilsConvert, Pcm16RoundTrips) {
  const std::vector<float> in{ 0.0f, 0.5f, -0.5f, 1.0f, -1.0f };
  std::vector<int16_t> pcm(in.size(), 0);
  AudioUtils::convertFloatToPcm16(in.data(), pcm.data(), static_cast<int32_t>(in.size()));
  EXPECT_EQ(pcm[0], 0);
  EXPECT_EQ(pcm[1], 16384);
  EXPECT_EQ(pcm[2], -16384);
  EXPECT_EQ(pcm[3], 32767);
  EXPECT_EQ(pcm[4], -32768);

  std::vector<float> out(in.size(), 0.0f);
  AudioUtils::convertPcm16ToFloat(pcm.data(), out.data(), static_cast<int32_t>(pcm.size()));
  EXPECT_FLOAT_EQ(out[0], 0.0f);
  EXPECT_FLOAT_EQ(out[1], 0.5f);
  EXPECT_FLOAT_EQ(out[2], -0.5f);
  EXPECT_FLOAT_EQ(out[3], 32767.0f / 32768.0f);
  EXPECT_FLOAT_EQ(out[4], -1.0f);
}

TEST(AudioUtilsConvert, Pcm24RoundTrips) {
  const std::vector<float> in{ 0.0f, 0.5f, -0.5f, 0.75f };
  std::vector<uint8_t> pcm(in.size() * 3, 0);
  AudioUtils::convertFloatToPcm24(in.data(), pcm.data(), static_cast<int32_t>(in.size()));
  // 0.5 * 2^23 = 0x400000 and -0.5 * 2^23 = -0x400000, in the native byte order.
#if defined(CORAL_CPU_LENDIAN)
  EXPECT_EQ(pcm[3], 0x00);
  EXPECT_EQ(pcm[4], 0x00);
  EXPECT_EQ(pcm[5], 0x40);
  EXPECT_EQ(pcm[6], 0x00);
  EXPECT_EQ(pcm[7], 0x00);
  EXPECT_EQ(pcm[8], 0xC0);
#else
  EXPECT_EQ(pcm[3], 0x40);
  EXPECT_EQ(pcm[4], 0x00);
  EXPECT_EQ(pcm[5], 0x00);
  EXPECT_EQ(pcm[6], 0xC0);
  EXPECT_EQ(pcm[7], 0x00);
  EXPECT_EQ(pcm[8], 0x00);
#endif

  std::vector<float> out(in.size(), 0.0f);
  AudioUtils::convertPcm24ToFloat(pcm.data(), out.data(), static_cast<int32_t>(in.size()));
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_FLOAT_EQ(out[i], in[i]);
  }
}

} // namespace coral
