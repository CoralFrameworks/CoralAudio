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

// =============================================================
//  tests_AudioConverter.cpp
//
//  Functional tests for coral::AudioConverter, covering:
//    - format validation in InitFromFormat()
//    - same-rate conversion between sample formats and interleavings
//    - sample rate conversion in both directions
//    - channel remapping, with and without down-mix
//    - the pull-based convertToBuffer() overload and its status codes
//    - dither determinism
// =============================================================

#include <coral/audio/AudioConverter.h>

#include <media/Utilities.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace coral;

namespace {

constexpr double kPi = 3.14159265358979323846;

int gChecks = 0;
int gFailures = 0;

void check(const bool condition, const std::string& what) {
  ++gChecks;
  if (condition) {
    std::printf("  ok   %s\n", what.c_str());
  } else {
    ++gFailures;
    std::printf("  FAIL %s\n", what.c_str());
  }
}

void checkNear(const float actual, const float expected, const float tolerance, const std::string& what) {
  const bool ok = std::fabs(actual - expected) <= tolerance;
  ++gChecks;
  if (ok) {
    std::printf("  ok   %s (%.6f)\n", what.c_str(), actual);
  } else {
    ++gFailures;
    std::printf("  FAIL %s: expected %.6f, got %.6f\n", what.c_str(), expected, actual);
  }
}

void section(const char* title) {
  std::printf("\n== %s ==\n", title);
}

/** A progressive sine, useful for checking that a conversion preserves shape. */
float tone(const uint32_t frame, const uint32_t channel, const double freq, const uint64_t sampleRate) {
  const double t = static_cast<double>(frame) / static_cast<double>(sampleRate);
  return static_cast<float>(0.5 * std::sin(2.0 * kPi * freq * t + 0.1 * channel));
}

std::unique_ptr<AudioPCMBuffer> makeToneBuffer(const AudioFormat& format,
  const AudioFrameCount frames,
  const double freq) {
  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, frames);
  if (!buffer) {
    return nullptr;
  }
  buffer->setFrameLength(frames);
  auto** data = buffer->floatChannelData();
  if (!data) {
    return nullptr;
  }
  for (uint32_t c = 0; c < format.getChannelCount(); ++c) {
    for (AudioFrameCount f = 0; f < frames; ++f) {
      data[c][f] = tone(f, c, freq, format.getSampleRate());
    }
  }
  return buffer;
}

/** Peak absolute sample of a planar float PCM buffer. */
float peakOf(AudioPCMBuffer& buffer) {
  const auto frames = buffer.getFrameLength();
  auto** data = buffer.floatChannelData();
  if (!data) {
    return 0.0f;
  }
  float peak = 0.0f;
  for (uint32_t c = 0; c < buffer.getFormat()->getChannelCount(); ++c) {
    for (AudioFrameCount f = 0; f < frames; ++f) {
      peak = std::max(peak, std::fabs(data[c][f]));
    }
  }
  return peak;
}

bool allFinite(AudioPCMBuffer& buffer) {
  const auto frames = buffer.getFrameLength();
  auto** data = buffer.floatChannelData();
  if (!data) {
    return false;
  }
  for (uint32_t c = 0; c < buffer.getFormat()->getChannelCount(); ++c) {
    for (AudioFrameCount f = 0; f < frames; ++f) {
      if (!std::isfinite(data[c][f])) {
        return false;
      }
    }
  }
  return true;
}

// =============================================================
//  InitFromFormat
// =============================================================

void testInitFromFormat() {
  section("InitFromFormat validation");

  const auto pcm48 = AudioFormat::InitStandardFormatWithSampleRate(48000, 2);
  const auto pcm441 = AudioFormat::InitStandardFormatWithSampleRate(44100, 2);
  const auto i16 = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, 48000, 2, false);
  const auto i16Interleaved = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, 48000, 2, true);

  check(AudioConverter::InitFromFormat(*pcm48, *i16) != nullptr, "float 48k -> int16 48k is accepted");
  check(AudioConverter::InitFromFormat(*pcm48, *pcm441) != nullptr, "48k -> 44.1k is accepted");
  check(AudioConverter::InitFromFormat(*pcm48, *i16Interleaved) != nullptr, "planar -> interleaved is accepted");

  // check(AudioConverter::InitFromFormat(AudioFormat(), *pcm48) == nullptr, "invalid input format is rejected");
  // check(AudioConverter::InitFromFormat(*pcm48, AudioFormat()) == nullptr, "invalid output format is rejected");

  constexpr AudioBasicDescription abd = {
    .mSampleRate = 48000,
    .mFormatType = AudioFormatType::kAudioFormatTypeMpeg4AAC,
    .mFormatFlags = AudioFormatFlags::kLinearPCMFormatFlagsAreAllClear,
    .mBytesPerPacket = 0, // CBR: 384, VBR: 0
    .mFramesPerPacket = 1024, // AAC: 1024 MP3: 1152
    .mBytesPerFrame = 0, // 0 for compressed frame
    .mChannelsPerFrame = 2,
    .mBitsPerChannel = 0, // 0 for compressed frame.
  };
  auto aacFormat = AudioFormat::InitWithStreamDescription(abd, std::nullopt);

  check(AudioConverter::InitFromFormat(*pcm48, aacFormat.value()) == nullptr, "compressed output format is rejected");
  check(AudioConverter::InitFromFormat(aacFormat.value(), *pcm48) == nullptr, "compressed input format is rejected");

  check(AudioConverter::InitFromFormat(*pcm48, *pcm48) != nullptr, "identical formats are accepted");

  if (auto converter = AudioConverter::InitFromFormat(*pcm48, *i16)) {
    check(converter->getInputFormat().getSampleRate() == 48000, "getInputFormat reports the input rate");
    check(converter->getOutputFormat().getSampleFormat() == AudioSampleFormat::kInt16,
      "getOutputFormat reports the output sample format");
    check(!converter->isDownmix(), "down-mix defaults to off");
    check(!converter->isDither(), "dither defaults to off");
    check(converter->getSampleRateConverterQuality() == AudioQuality::Fastest, "quality defaults to Fastest");
    check(converter->getPrimeMethod() == AudioConverter::PrimeMethod::kNormal, "prime method defaults to kNormal");
  }
}

// =============================================================
//  Simple conversion
// =============================================================

void testSameRateConversions() {
  const auto pcm48 = AudioFormat::InitStandardFormatWithSampleRate(48000, 2);

  section("Same rate: planar float -> planar int16");
  {
    const auto i16 = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, 48000, 2, false);
    auto converter = AudioConverter::InitFromFormat(*pcm48, *i16);
    constexpr AudioFrameCount kFrames = 512;
    auto in = makeToneBuffer(*pcm48, kFrames, 440.0);
    auto out = AudioPCMBuffer::InitWithPCMFormat(*i16, kFrames);
    check(converter && in && out, "converter and buffers created");
    if (converter && in && out) {
      const auto error = converter->convertToBuffer(in, out);
      check(!error.has_value(), "convertToBuffer reports success");
      check(out->getFrameLength() == kFrames, "frame length is preserved");
      auto** outData = out->int16ChannelData();
      check(outData != nullptr, "int16 channel data is available");
      if (outData) {
        const float expected = in->floatChannelData()[0][100];
        checkNear(static_cast<float>(outData[0][100]) / 32768.0f, expected, 1.0f / 32767.0f,
          "sample round-trips within one LSB");
        check(std::abs(static_cast<int>(outData[0][0])) <= 1, "first sample of a sine starts near zero");
      }
    }
  }

  section("Same rate: planar float -> interleaved int16");
  {
    const auto i16Interleaved = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, 48000, 2, true);
    auto converter = AudioConverter::InitFromFormat(*pcm48, *i16Interleaved);
    constexpr AudioFrameCount kFrames = 256;
    auto in = makeToneBuffer(*pcm48, kFrames, 440.0);
    auto out = AudioPCMBuffer::InitWithPCMFormat(*i16Interleaved, kFrames);
    check(converter && in && out, "converter and buffers created");
    if (converter && in && out) {
      const auto error = converter->convertToBuffer(in, out);
      check(!error.has_value(), "convertToBuffer reports success");
      check(out->getFrameLength() == kFrames, "frame length is preserved");
      const auto* list = out->audioBufferList();
      check(list && list->numberOfBuffers() == 1, "interleaved output uses a single buffer");
      auto** outData = out->int16ChannelData();
      if (outData && list && list->numberOfBuffers() == 1) {
        check(list->getBuffer(0)->mNumberChannels == 2, "the single buffer carries both channels");
        // Channel 1 lands one frame past channel 0 in the interleaved layout.
        for (AudioFrameCount f = 0; f < 4; ++f) {
          const float left = static_cast<float>(outData[0][f * 2]) / 32768.0f;
          const float right = static_cast<float>(outData[0][f * 2 + 1]) / 32768.0f;
          if (std::fabs(left - in->floatChannelData()[0][f]) > 1.0f / 32767.0f ||
              std::fabs(right - in->floatChannelData()[1][f]) > 1.0f / 32767.0f) {
            check(false, "interleaved samples interleave left then right");
            return;
          }
        }
        check(true, "interleaved samples interleave left then right");
      }
    }
  }

  section("Same rate: float -> int32 and int24");
  {
    for (const auto sampleFormat: { AudioSampleFormat::kInt32, AudioSampleFormat::kInt24 }) {
      const auto target = AudioFormat::InitWithCommonFormat(sampleFormat, 48000, 2, false);
      auto converter = AudioConverter::InitFromFormat(*pcm48, *target);
      constexpr AudioFrameCount kFrames = 128;
      auto in = makeToneBuffer(*pcm48, kFrames, 440.0);
      auto out = AudioPCMBuffer::InitWithPCMFormat(*target, kFrames);
      if (converter && in && out) {
        const auto error = converter->convertToBuffer(in, out);
        check(!error.has_value(), std::string("conversion to ") +
                                  (sampleFormat == AudioSampleFormat::kInt32 ? "int32" : "int24") +
                                  " succeeds");
        check(out->getFrameLength() == kFrames, "frame length is preserved");
      } else {
        check(false, "converter and buffers created");
      }
    }
  }
}

// =============================================================
//  Sample rate conversion
// =============================================================

void testSampleRateConversion() {
  const auto pcm48 = AudioFormat::InitStandardFormatWithSampleRate(48000, 2);
  const auto pcm441 = AudioFormat::InitStandardFormatWithSampleRate(44100, 2);

  section("Down-sample 48000 -> 44100");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm48, *pcm441);
    check(converter != nullptr, "converter created");
    if (converter) {
      converter->setSampleRateConverterQuality(AudioQuality::High);
      check(converter->getSampleRateConverterQuality() == AudioQuality::High, "quality round-trips");

      constexpr AudioFrameCount kInFrames = 4800; // 100 ms
      constexpr AudioFrameCount kOutCapacity = 8192;
      auto in = makeToneBuffer(*pcm48, kInFrames, 440.0);
      auto out = AudioPCMBuffer::InitWithPCMFormat(*pcm441, kOutCapacity);
      if (in && out) {
        const auto error = converter->convertToBuffer(in, out);
        check(!error.has_value(), "convertToBuffer reports success");
        const auto produced = out->getFrameLength();
        std::printf("       in=%u out=%u\n", kInFrames, produced);
        // 4800 * 44100 / 48000 == 4410, give or take a frame of phase.
        check(produced >= 4408 && produced <= 4412, "output frame count follows the rate ratio");
        check(allFinite(*out), "all resampled samples are finite");
        checkNear(peakOf(*out), 0.5f, 0.05f, "resampled tone keeps its amplitude");
      } else {
        check(false, "buffers created");
      }
    }
  }

  section("Up-sample 44100 -> 48000");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm441, *pcm48);
    check(converter != nullptr, "converter created");
    if (converter) {
      constexpr AudioFrameCount kInFrames = 4410; // 100 ms
      constexpr AudioFrameCount kOutCapacity = 8192;
      auto in = makeToneBuffer(*pcm441, kInFrames, 440.0);
      auto out = AudioPCMBuffer::InitWithPCMFormat(*pcm48, kOutCapacity);
      if (in && out) {
        const auto error = converter->convertToBuffer(in, out);
        check(!error.has_value(), "convertToBuffer reports success");
        const auto produced = out->getFrameLength();
        std::printf("       in=%u out=%u\n", kInFrames, produced);
        // 4410 * 48000 / 44100 == 4800.
        check(produced >= 4798 && produced <= 4802, "output frame count follows the rate ratio");
        check(allFinite(*out), "all resampled samples are finite");
        checkNear(peakOf(*out), 0.5f, 0.05f, "resampled tone keeps its amplitude");
      } else {
        check(false, "buffers created");
      }
    }
  }

  section("Streaming keeps phase across calls");
  {
    // Converting two halves separately must yield roughly the same total as one
    // call, which only holds if the resampler keeps its phase between calls.
    auto oneShot = AudioConverter::InitFromFormat(*pcm48, *pcm441);
    auto streamed = AudioConverter::InitFromFormat(*pcm48, *pcm441);
    constexpr AudioFrameCount kHalf = 2400;
    auto inA = makeToneBuffer(*pcm48, kHalf, 440.0);
    auto inB = makeToneBuffer(*pcm48, kHalf, 440.0);
    auto whole = makeToneBuffer(*pcm48, kHalf * 2, 440.0);
    auto outOne = AudioPCMBuffer::InitWithPCMFormat(*pcm441, 8192);

    if (oneShot && streamed && inA && inB && whole && outOne) {
      oneShot->convertToBuffer(whole, outOne);
      const auto oneFrames = outOne->getFrameLength();

      AudioFrameCount streamFrames = 0;
      for (auto* chunk: { &inA, &inB }) {
        auto outChunk = AudioPCMBuffer::InitWithPCMFormat(*pcm441, 4096);
        if (outChunk) {
          streamed->convertToBuffer(*chunk, outChunk);
          streamFrames += outChunk->getFrameLength();
        }
      }
      std::printf("       one-shot=%u streamed-sum=%u\n", oneFrames, streamFrames);
      check(std::abs(static_cast<int>(oneFrames) - static_cast<int>(streamFrames)) <= 2,
        "streamed conversion totals match the one-shot conversion");
    } else {
      check(false, "buffers created");
    }
  }

  section("reset() clears the conversion state");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm48, *pcm441);
    if (converter) {
      constexpr AudioFrameCount kFrames = 1024;
      auto in = makeToneBuffer(*pcm48, kFrames, 440.0);
      auto out = AudioPCMBuffer::InitWithPCMFormat(*pcm441, 4096);
      converter->convertToBuffer(in, out);
      const auto first = out->getFrameLength();
      converter->reset();
      auto out2 = AudioPCMBuffer::InitWithPCMFormat(*pcm441, 4096);
      converter->convertToBuffer(in, out2);
      check(out2->getFrameLength() == first, "the same input yields the same output after reset()");
    }
  }
}

// =============================================================
//  Channel remapping
// =============================================================

void testChannelMapping() {
  const auto stereo = AudioFormat::InitStandardFormatWithSampleRate(48000, 2);
  const auto mono = AudioFormat::InitStandardFormatWithSampleRate(48000, 1);

  section("Stereo -> mono");
  {
    auto converter = AudioConverter::InitFromFormat(*stereo, *mono);
    check(converter != nullptr, "converter created");
    if (converter) {
      constexpr AudioFrameCount kFrames = 256;
      auto in = makeToneBuffer(*stereo, kFrames, 440.0);
      auto out = AudioPCMBuffer::InitWithPCMFormat(*mono, kFrames);
      if (in && out) {
        const auto error = converter->convertToBuffer(in, out);
        check(!error.has_value(), "convertToBuffer reports success");
        check(out->getFrameLength() == kFrames, "frame length is preserved");
        check(out->getFormat()->getChannelCount() == 1, "output has a single channel");
      }
    }
  }

  section("Mono -> stereo, and with down-mix enabled");
  {
    auto converter = AudioConverter::InitFromFormat(*mono, *stereo);
    if (converter) {
      constexpr AudioFrameCount kFrames = 256;
      auto in = makeToneBuffer(*mono, kFrames, 440.0);
      auto out = AudioPCMBuffer::InitWithPCMFormat(*stereo, kFrames);
      if (in && out) {
        converter->setDownMix(true);
        check(converter->isDownmix(), "down-mix flag round-trips");
        const auto error = converter->convertToBuffer(in, out);
        check(!error.has_value(), "convertToBuffer reports success");
        check(out->getFrameLength() == kFrames, "frame length is preserved");
      }
    }
  }

  section("Mono -> stereo repeats the mono channel on every output channel");
  {
    auto converter = AudioConverter::InitFromFormat(*mono, *stereo);
    if (converter) {
      constexpr AudioFrameCount kFrames = 128;
      auto in = makeToneBuffer(*mono, kFrames, 440.0);
      auto out = AudioPCMBuffer::InitWithPCMFormat(*stereo, kFrames);
      if (in && out) {
        const auto error = converter->convertToBuffer(in, out);
        check(!error.has_value(), "convertToBuffer reports success");
        auto** data = out->floatChannelData();
        bool rightMatches = data != nullptr;
        float leftPeak = 0.0f;
        if (data) {
          for (AudioFrameCount f = 0; f < kFrames; ++f) {
            // A lone input channel is placed on every output channel rather than
            // only on the first one.
            if (data[1][f] != data[0][f]) {
              rightMatches = false;
            }
            leftPeak = std::max(leftPeak, std::fabs(data[0][f]));
          }
        }
        check(rightMatches, "the mono channel is repeated on the right channel");
        check(leftPeak > 0.2f, "the mono channel carries the signal");
      }
    }
  }
}

// =============================================================
//  Pull-based conversion
// =============================================================

void testInputBlockOverload() {
  const auto pcm441 = AudioFormat::InitStandardFormatWithSampleRate(44100, 2);
  const auto pcm48 = AudioFormat::InitStandardFormatWithSampleRate(48000, 2);

  section("InputBlock overload, 44100 -> 48000");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm441, *pcm48);
    check(converter != nullptr, "converter created");
    if (converter) {
      converter->setSampleRateConverterQuality(AudioQuality::Medium);

      constexpr AudioFrameCount kInFrames = 4410;
      constexpr AudioFrameCount kOutFrames = 4800;
      auto source = makeToneBuffer(*pcm441, kInFrames, 440.0);
      check(source != nullptr, "source buffer created");
      if (source) {
        // Serve the source as interleaved raw PCM, one chunk at a time.
        std::vector<float> interleaved(static_cast<size_t>(kInFrames) * 2);
        for (AudioFrameCount f = 0; f < kInFrames; ++f) {
          interleaved[f * 2 + 0] = source->floatChannelData()[0][f];
          interleaved[f * 2 + 1] = source->floatChannelData()[1][f];
        }

        std::vector<float> outSamples(static_cast<size_t>(kOutFrames) * 2, 0.0f);
        auto rawOut = std::make_unique<AudioBuffer>();
        rawOut->mNumberChannels = 2;
        rawOut->mDataByteSize = static_cast<uint32_t>(outSamples.size() * sizeof(float));
        rawOut->mData = outSamples.data();

        AudioFrameCount consumed = 0;
        constexpr AudioFrameCount kChunk = 512;
        Error* error = nullptr;

        const auto status = converter->convertToBuffer(rawOut, &error,
          [&](AudioPacketCount, AudioConverter::InputStatus* outStatus) -> std::unique_ptr<AudioBuffer> {
            if (consumed >= kInFrames) {
              *outStatus = AudioConverter::InputStatus::EndOfStream;
              return nullptr;
            }
            const AudioFrameCount available = std::min(kChunk, kInFrames - consumed);
            auto buffer = std::make_unique<AudioBuffer>();
            buffer->mNumberChannels = 2;
            buffer->mDataByteSize = available * 2 * sizeof(float);
            buffer->mData = interleaved.data() + static_cast<size_t>(consumed) * 2;
            consumed += available;
            *outStatus = AudioConverter::InputStatus::HaveData;
            return buffer;
          });

        const auto producedFrames = rawOut->mDataByteSize / (2 * sizeof(float));
        std::printf("       status=%d consumed=%u produced=%u\n", static_cast<int>(status), consumed,
          producedFrames);
        check(error == nullptr, "no error is reported");
        check(status == AudioConverter::OutputStatus::HaveData, "status is HaveData");
        check(producedFrames == kOutFrames, "the output buffer is filled to capacity");

        float peak = 0.0f;
        bool finite = true;
        for (const float v: outSamples) {
          if (!std::isfinite(v)) {
            finite = false;
          }
          peak = std::max(peak, std::fabs(v));
        }
        check(finite, "all pulled samples are finite");
        checkNear(peak, 0.5f, 0.05f, "pulled tone keeps its amplitude");
        delete error;
        error = nullptr;
      }
    }
  }

  section("InputBlock overload reports EndOfStream when no input is available");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm48, *pcm48);
    if (converter) {
      std::vector<float> outSamples(512 * 2, 0.0f);
      auto rawOut = std::make_unique<AudioBuffer>();
      rawOut->mNumberChannels = 2;
      rawOut->mDataByteSize = static_cast<uint32_t>(outSamples.size() * sizeof(float));
      rawOut->mData = outSamples.data();

      Error* error = nullptr;
      const auto status = converter->convertToBuffer(rawOut, &error,
        [](AudioPacketCount, AudioConverter::InputStatus* outStatus) -> std::unique_ptr<AudioBuffer> {
          *outStatus = AudioConverter::InputStatus::EndOfStream;
          return nullptr;
        });
      check(status == AudioConverter::OutputStatus::EndOfStream, "status is EndOfStream");
      check(rawOut->mDataByteSize == 0, "no frames are reported");
      delete error;
    }
  }

  section("InputBlock overload rejects unusable arguments");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm48, *pcm48);
    if (converter) {
      Error* error = nullptr;
      const auto nullOutput = converter->convertToBuffer(std::unique_ptr<AudioBuffer>(), &error, nullptr);
      check(nullOutput == AudioConverter::OutputStatus::Error, "a null output buffer is an error");
      check(error != nullptr, "an error object is handed back");
      delete error;
      error = nullptr;

      std::vector<float> outSamples(512 * 2, 0.0f);
      auto rawOut = std::make_unique<AudioBuffer>();
      rawOut->mNumberChannels = 2;
      rawOut->mDataByteSize = static_cast<uint32_t>(outSamples.size() * sizeof(float));
      rawOut->mData = outSamples.data();
      const AudioConverter::InputBlock noBlock;
      const auto nullBlock = converter->convertToBuffer(rawOut, &error, noBlock);
      check(nullBlock == AudioConverter::OutputStatus::Error, "a null input block is an error");
      check(error != nullptr, "an error object is handed back");
      delete error;
      error = nullptr;

      auto zeroCapacity = std::make_unique<AudioBuffer>();
      zeroCapacity->mNumberChannels = 2;
      zeroCapacity->mDataByteSize = 0;
      zeroCapacity->mData = nullptr;
      const auto noRoom = converter->convertToBuffer(zeroCapacity, &error, noBlock);
      check(noRoom == AudioConverter::OutputStatus::Error, "a zero capacity output buffer is an error");
      delete error;
    }
  }
}

// =============================================================
//  Flags and error handling
// =============================================================

void testResampledToneFrequency() {
  section("A resampled tone keeps its frequency");
  {
    const auto pcm441 = AudioFormat::InitStandardFormatWithSampleRate(44100, 2);
    const auto pcm48 = AudioFormat::InitStandardFormatWithSampleRate(48000, 2);
    auto converter = AudioConverter::InitFromFormat(*pcm441, *pcm48);
    auto in = makeToneBuffer(*pcm441, 44100, 440.0);
    auto out = AudioPCMBuffer::InitWithPCMFormat(*pcm48, 65536);
    if (converter && in && out) {
      const auto error = converter->convertToBuffer(in, out);
      check(!error.has_value(), "the tone converts");
      const auto frames = out->getFrameLength();
      auto* const* channels = out->floatChannelData();
      if (frames > 1000 && channels != nullptr) {
        // Count positive-going zero crossings: a 440 Hz tone has 440 of them per second
        // whatever the sample rate is.
        auto crossings = 0;
        for (AudioFrameCount f = 1; f < frames; ++f) {
          if (channels[0][f - 1] <= 0.0f && channels[0][f] > 0.0f) {
            ++crossings;
          }
        }
        const auto frequency = static_cast<double>(crossings) * pcm48->getSampleRate() /
            static_cast<double>(frames);
        std::printf("       %u frames at %g Hz, %d crossings, measured %g Hz\n", frames,
          pcm48->getSampleRate(), crossings, frequency);
        check(std::abs(frequency - 440.0) < 20.0, "the resampled tone is still 440 Hz");

        // A resampler that repeats or drops samples shows up as a step between neighbours.
        auto maxStep = 0.0f;
        for (AudioFrameCount f = 1; f < frames; ++f) {
          maxStep = std::max(maxStep, std::abs(channels[0][f] - channels[0][f - 1]));
        }
        check(maxStep < 0.2f, "the resampled tone has no discontinuities");
      }
    }
  }
}

void testFlagsAndErrors() {
  const auto pcm48 = AudioFormat::InitStandardFormatWithSampleRate(48000, 2);
  const auto i16 = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, 48000, 2, false);

  section("Dither is deterministic");
  {
    auto a = AudioConverter::InitFromFormat(*pcm48, *i16);
    auto b = AudioConverter::InitFromFormat(*pcm48, *i16);
    constexpr AudioFrameCount kFrames = 256;
    auto in = makeToneBuffer(*pcm48, kFrames, 440.0);
    auto outA = AudioPCMBuffer::InitWithPCMFormat(*i16, kFrames);
    auto outB = AudioPCMBuffer::InitWithPCMFormat(*i16, kFrames);
    if (a && b && in && outA && outB) {
      a->setDither(true);
      b->setDither(true);
      check(a->isDither() && b->isDither(), "dither flag round-trips");
      a->convertToBuffer(in, outA);
      b->convertToBuffer(in, outB);
      auto** dataA = outA->int16ChannelData();
      auto** dataB = outB->int16ChannelData();
      bool identical = dataA != nullptr && dataB != nullptr;
      if (identical) {
        for (AudioFrameCount f = 0; f < kFrames && identical; ++f) {
          for (uint32_t c = 0; c < 2; ++c) {
            if (dataA[c][f] != dataB[c][f]) {
              identical = false;
            }
          }
        }
      }
      check(identical, "two converters with dither produce identical output");
    } else {
      check(false, "converters and buffers created");
    }
  }

  section("Prime info follows the prime method");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm48, *i16);
    if (converter) {
      converter->setPrimeMethod(AudioConverter::PrimeMethod::kNone);
      const auto none = converter->getPrimeInfo();
      check(none.leadingFrames == 0 && none.trailingFrames == 0, "kNone needs no priming frames");

      converter->setPrimeMethod(AudioConverter::PrimeMethod::kNormal);
      const auto normal = converter->getPrimeInfo();
      check(normal.leadingFrames == 0 && normal.trailingFrames > 0, "kNormal needs trailing frames only");

      converter->setPrimeMethod(AudioConverter::PrimeMethod::kPre);
      const auto pre = converter->getPrimeInfo();
      check(pre.leadingFrames > 0 && pre.trailingFrames > 0, "kPre needs both leading and trailing frames");
    }
  }

  section("convertToBuffer rejects null buffers");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm48, *i16);
    if (converter) {
      constexpr AudioFrameCount kFrames = 64;
      auto in = makeToneBuffer(*pcm48, kFrames, 440.0);
      auto out = AudioPCMBuffer::InitWithPCMFormat(*i16, kFrames);

      std::unique_ptr<AudioPCMBuffer> nullIn;
      const auto nullInput = converter->convertToBuffer(nullIn, out);
      check(nullInput.has_value(), "a null input buffer is an error");

      std::unique_ptr<AudioPCMBuffer> nullOut;
      const auto nullOutput = converter->convertToBuffer(in, nullOut);
      check(nullOutput.has_value(), "a null output buffer is an error");

      constexpr AudioFrameCount kSmall = 32;
      auto smallOut = AudioPCMBuffer::InitWithPCMFormat(*i16, kSmall);
      const auto tooSmall = converter->convertToBuffer(in, smallOut);
      check(tooSmall.has_value(), "an output buffer with too little capacity is an error");

      auto mismatch = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, 48000, 1, false);
      auto wrongChannels = AudioPCMBuffer::InitWithPCMFormat(*mismatch, kFrames);
      const auto mismatchError = converter->convertToBuffer(in, wrongChannels);
      check(mismatchError.has_value(), "a buffer whose channel layout does not match is an error");
    }
  }

  section("Empty input produces empty output");
  {
    auto converter = AudioConverter::InitFromFormat(*pcm48, *i16);
    auto in = makeToneBuffer(*pcm48, 16, 440.0);
    auto out = AudioPCMBuffer::InitWithPCMFormat(*i16, 16);
    if (converter && in && out) {
      in->setFrameLength(0);
      const auto error = converter->convertToBuffer(in, out);
      check(!error.has_value(), "conversion of an empty buffer succeeds");
      check(out->getFrameLength() == 0, "no frames are produced");
    }
  }

  section("A resampled conversion repeated into the same output buffer");
  {
    auto pcm44 = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kFloat, 44100, 2, false);
    auto pcm48 = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kFloat, 48000, 2, false);
    auto converter = AudioConverter::InitFromFormat(*pcm44, *pcm48);
    auto in = makeToneBuffer(*pcm44, 1024, 440.0);
    auto out = AudioPCMBuffer::InitWithPCMFormat(*pcm48, 2048);
    if (converter && in && out) {
      // A short block first: it leaves the output buffer reporting fewer frames than its
      // capacity, and the next block has to be able to use the whole capacity again.
      in->setFrameLength(256);
      const auto shortBlock = converter->convertToBuffer(in, out);
      check(!shortBlock.has_value(), "a short block is converted");
      const auto shortFrames = out->getFrameLength();

      in->setFrameLength(1024);
      const auto longBlock = converter->convertToBuffer(in, out);
      check(!longBlock.has_value(), "a longer block converts into the same output buffer");
      const auto longFrames = out->getFrameLength();
      check(longFrames > shortFrames, "the longer block produced more frames than the short one");
      const auto ratio = static_cast<double>(longFrames) / 1024.0;
      check(std::abs(ratio - 48000.0 / 44100.0) < 0.02, "the produced frames follow the sample rate ratio");
    }
  }
}

} // namespace anonymous

int main() {
  std::printf("AudioConverter tests\n");

  testInitFromFormat();
  testSameRateConversions();
  testSampleRateConversion();
  testResampledToneFrequency();
  testChannelMapping();
  testInputBlockOverload();
  testFlagsAndErrors();

  std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
  std::printf("%s\n", gFailures == 0 ? "tests_AudioConverter: PASS" : "tests_AudioConverter: FAIL");
  return gFailures == 0 ? 0 : 1;
}
