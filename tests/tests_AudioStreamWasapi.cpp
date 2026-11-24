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
//  tests_AudioStreamWasapi.cpp
//
//  Exercises coral::AudioStreamWasapi against the real WASAPI
//  endpoints of the machine it runs on:
//    - a render stream opens, reports a usable device format and starts
//    - the data callback is driven from the stream thread, and stopping the
//      stream really stops it
//    - a capture stream opens and delivers frames
//    - the blocking path moves frames through write() and read()
//    - the state machine reports the transitions it was asked for, and refuses
//      the ones that make no sense
//
//  A machine without the endpoint a section needs has no way to pass it, so that
//  section is reported as SKIP and the run still succeeds. Everything else is a
//  failure: this test is meant to be run on a machine with a sound card.
// =============================================================

#if !defined(_WIN32)

#include <cstdio>

int main() {
  std::printf("tests_AudioStreamWasapi: SKIP (Windows only)\n");
  return 0;
}

#else

#include "private/audio/AudioStream.h"
#include "private/audio/AudioStreamBuilder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace coral;

namespace {

int gChecks = 0;
int gFailures = 0;
int gSkips = 0;

void check(const bool condition, const std::string& what) {
  ++gChecks;
  if (condition) {
    std::printf("  ok   %s\n", what.c_str());
  } else {
    ++gFailures;
    std::printf("  FAIL %s\n", what.c_str());
  }
}

void skip(const std::string& what) {
  ++gSkips;
  std::printf("  SKIP %s\n", what.c_str());
}

void section(const char* title) {
  std::printf("\n== %s ==\n", title);
}

constexpr double kPi = 3.14159265358979323846;

/** Quiet enough not to be unpleasant, loud enough to be a real signal. */
constexpr float kAmplitude = 0.02f;

/** Fills the buffer the callback was handed with a quiet tone, and counts the work. */
class RenderProbe final : public AudioStreamDataCallback {
public:
  DataCallbackResult onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) override {
    const int callbacks = mCallbacks.fetch_add(1) + 1;
    mFrames.fetch_add(numFrames);
    // Only ever raised, so the sampler below reports the largest buffer of the run.
    AudioFrameCount seen = mMaxFrames.load();
    while (seen < numFrames && !mMaxFrames.compare_exchange_weak(seen, numFrames)) {
    }
    if (callbacks == 1) {
      mFormat.store(stream->getFormat());
      mSampleRate.store(stream->getSampleRate());
      mChannels.store(stream->getChannelCount());
    }

    if (audioData != nullptr && stream->getFormat() == AudioSampleFormat::kFloat) {
      auto* samples = static_cast<float*>(audioData);
      const auto channels = std::max<uint32_t>(stream->getChannelCount(), 1u);
      const auto sampleRate = std::max<uint64_t>(stream->getSampleRate(), 1u);
      const uint64_t phase = mPhase.fetch_add(numFrames);
      for (AudioFrameCount frame = 0; frame < numFrames; ++frame) {
        const double t = static_cast<double>(phase + frame) / static_cast<double>(sampleRate);
        const auto value = static_cast<float>(std::sin(2.0 * kPi * 440.0 * t) * kAmplitude);
        for (uint32_t channel = 0; channel < channels; ++channel) {
          samples[frame * channels + channel] = value;
        }
      }
    }
    return DataCallbackResult::Continue;
  }

  int callbacks() const {
    return mCallbacks.load();
  }

  int64_t frames() const {
    return mFrames.load();
  }

  /** The most frames any single callback was handed. */
  AudioFrameCount maxFramesPerCallback() const {
    return mMaxFrames.load();
  }

  AudioSampleFormat format() const {
    return mFormat.load();
  }

  uint64_t sampleRate() const {
    return mSampleRate.load();
  }

  uint32_t channels() const {
    return mChannels.load();
  }

private:
  std::atomic<int> mCallbacks{ 0 };
  std::atomic<int64_t> mFrames{ 0 };
  std::atomic<AudioFrameCount> mMaxFrames{ 0 };
  std::atomic<uint64_t> mPhase{ 0 };
  std::atomic<AudioSampleFormat> mFormat{ AudioSampleFormat::kDefault };
  std::atomic<uint64_t> mSampleRate{ 0 };
  std::atomic<uint32_t> mChannels{ 0 };
};

/** Counts what a capture stream delivered and remembers whether any of it was audible. */
class CaptureProbe final : public AudioStreamDataCallback {
public:
  DataCallbackResult onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) override {
    const int callbacks = mCallbacks.fetch_add(1) + 1;
    mFrames.fetch_add(numFrames);
    if (callbacks == 1) {
      mFormat.store(stream->getFormat());
      mSampleRate.store(stream->getSampleRate());
      mChannels.store(stream->getChannelCount());
    }

    if (audioData != nullptr && stream->getFormat() == AudioSampleFormat::kFloat) {
      const auto* samples = static_cast<const float*>(audioData);
      const auto sampleCount = static_cast<size_t>(numFrames) * std::max<uint32_t>(stream->getChannelCount(), 1u);
      for (size_t i = 0; i < sampleCount; ++i) {
        if (std::fabs(samples[i]) > 1e-6f) {
          mNonSilent.fetch_add(1);
          break;
        }
      }
    }
    return DataCallbackResult::Continue;
  }

  int callbacks() const {
    return mCallbacks.load();
  }

  int64_t frames() const {
    return mFrames.load();
  }

  int nonSilentBuffers() const {
    return mNonSilent.load();
  }

  AudioSampleFormat format() const {
    return mFormat.load();
  }

private:
  std::atomic<int> mCallbacks{ 0 };
  std::atomic<int64_t> mFrames{ 0 };
  std::atomic<int> mNonSilent{ 0 };
  std::atomic<AudioSampleFormat> mFormat{ AudioSampleFormat::kDefault };
  std::atomic<uint64_t> mSampleRate{ 0 };
  std::atomic<uint32_t> mChannels{ 0 };
};

void sleepMilliseconds(const int64_t millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

/** Opens a stream, reporting why it could not be opened. */
Result openStream(AudioStreamBuilder& builder, std::shared_ptr<AudioStream>& stream) {
  return builder.openStream(stream);
}

/**
 * Fills a byte buffer with a quiet tone in the stream's own format.
 *
 * The device format is not necessarily the one that was asked for, so the caller's
 * buffer is built from what the stream reports.
 */
std::vector<uint8_t> makeTone(const std::shared_ptr<AudioStream>& stream, const AudioFrameCount frames) {
  const auto bytesPerFrame = stream->getBytesPerFrame();
  std::vector<uint8_t> buffer(static_cast<size_t>(frames) * bytesPerFrame, 0);
  const auto sampleRate = std::max<uint64_t>(stream->getSampleRate(), 1u);
  const auto channels = std::max<uint32_t>(stream->getChannelCount(), 1u);
  static uint64_t phase = 0;

  for (AudioFrameCount frame = 0; frame < frames; ++frame) {
    const double t = static_cast<double>(phase + frame) / static_cast<double>(sampleRate);
    const double value = std::sin(2.0 * kPi * 440.0 * t) * kAmplitude;
    for (uint32_t channel = 0; channel < channels; ++channel) {
      const size_t sample = static_cast<size_t>(frame) * channels + channel;
      switch (stream->getFormat()) {
        case AudioSampleFormat::kFloat:
          reinterpret_cast<float*>(buffer.data())[sample] = static_cast<float>(value);
          break;
        case AudioSampleFormat::kInt16:
          reinterpret_cast<int16_t*>(buffer.data())[sample] = static_cast<int16_t>(value * 32767.0);
          break;
        case AudioSampleFormat::kInt32:
          reinterpret_cast<int32_t*>(buffer.data())[sample] =
              static_cast<int32_t>(value * 2147483647.0);
          break;
        case AudioSampleFormat::kInt24: {
          const auto raw = static_cast<int32_t>(value * 8388607.0);
          uint8_t* sample24 = buffer.data() + sample * 3;
          sample24[0] = static_cast<uint8_t>(raw & 0xFF);
          sample24[1] = static_cast<uint8_t>((raw >> 8) & 0xFF);
          sample24[2] = static_cast<uint8_t>((raw >> 16) & 0xFF);
          break;
        }
        default:
          break;
      }
    }
  }
  phase += frames;
  return buffer;
}

// ---------------------------------------------------------------------------
//  Playback driven by the data callback
// ---------------------------------------------------------------------------

void testRenderCallback() {
  section("Render stream driven by the data callback");

  const auto probe = std::make_shared<RenderProbe>();
  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Render)
                                       .setSampleRate(48000)
                                       .setChannelCount(2)
                                       .setFormat(AudioSampleFormat::kFloat)
                                       .setDataCallback(probe),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip(std::string("no playback endpoint is available (") + convertToText(opened) + ")");
    return;
  }

  check(stream->getState() == StreamState::Open, "a freshly opened stream is Open");
  check(stream->getSampleRate() > 0, "the stream reports a sample rate");
  check(stream->getChannelCount() > 0, "the stream reports a channel count");
  check(stream->getFormat() != AudioSampleFormat::kInvalid, "the stream reports a sample format");
  check(stream->getBytesPerFrame() > 0, "the stream reports a frame size");
  check(stream->getFramesPerBurst() > 0, "the stream reports a burst size");
  check(stream->getBufferCapacityInFrames() > 0, "the stream reports a device buffer size");
  check(!stream->isXRunCountSupported(), "the callback path does not claim to see dropped buffers");

  check(stream->start() == Result::OK, "starting the stream succeeded");
  check(stream->getState() == StreamState::Started, "the stream is Started");

  StreamState next = StreamState::Unknown;
  check(stream->waitForStateChange(StreamState::Started, &next, 0) == Result::ErrorTimeout,
      "waiting for a change on a running stream times out");

  sleepMilliseconds(250);
  const int callbacksWhileRunning = probe->callbacks();
  const int64_t framesWhileRunning = probe->frames();
  std::printf("       callbacks=%d frames=%lld formatReported=%d\n", callbacksWhileRunning,
      static_cast<long long>(framesWhileRunning), static_cast<int>(probe->format()));

  check(callbacksWhileRunning > 0, "the data callback was called");
  check(framesWhileRunning > 0, "the data callback was handed frames");
  check(stream->getFramesWritten() > 0, "the stream counted the frames it was given");
  check(stream->getFramesRead() >= 0, "the device clock reports what was played");

  check(stream->pause() == Result::OK, "pausing the stream succeeded");
  check(stream->getState() == StreamState::Paused, "the stream is Paused");

  const int callbacksAtPause = probe->callbacks();
  sleepMilliseconds(80);
  check(probe->callbacks() - callbacksAtPause <= 1, "a paused stream stops driving the callback");

  check(stream->start() == Result::OK, "restarting a paused stream succeeded");
  check(stream->getState() == StreamState::Started, "the stream is Started again");

  check(stream->stop() == Result::OK, "stopping the stream succeeded");
  check(stream->getState() == StreamState::Stopped, "the stream is Stopped");

  check(stream->close() == Result::OK, "closing the stream succeeded");
  check(stream->getState() == StreamState::Closed, "the stream is Closed");
  check(stream->start() != Result::OK, "a closed stream refuses to start");
  check(stream->close() == Result::ErrorClosed, "closing twice is reported");
}

// ---------------------------------------------------------------------------
//  Recording driven by the data callback
// ---------------------------------------------------------------------------

void testCaptureCallback() {
  section("Capture stream driven by the data callback");

  const auto probe = std::make_shared<CaptureProbe>();
  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Capture)
                                       .setSampleRate(48000)
                                       .setChannelCount(1)
                                       .setFormat(AudioSampleFormat::kFloat)
                                       .setDataCallback(probe),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip(std::string("no recording endpoint is available (") + convertToText(opened) + ")");
    return;
  }

  check(stream->getState() == StreamState::Open, "a freshly opened capture stream is Open");
  check(stream->getSampleRate() > 0, "the capture stream reports a sample rate");
  check(stream->getChannelCount() > 0, "the capture stream reports a channel count");

  check(stream->start() == Result::OK, "starting the capture stream succeeded");
  sleepMilliseconds(250);

  const int callbacks = probe->callbacks();
  const int64_t frames = probe->frames();
  std::printf("       callbacks=%d frames=%lld nonSilentBuffers=%d\n", callbacks,
      static_cast<long long>(frames), probe->nonSilentBuffers());

  check(callbacks > 0, "the capture callback was called");
  check(frames > 0, "the capture callback was handed frames");
  check(stream->getFramesWritten() > 0, "the stream counted the frames it received");

  check(stream->stop() == Result::OK, "stopping the capture stream succeeded");
  check(stream->close() == Result::OK, "closing the capture stream succeeded");
}

// ---------------------------------------------------------------------------
//  The callback size
// ---------------------------------------------------------------------------

/** Small enough that a device buffer holds more than one callback of it. */
constexpr AudioFrameCount kRequestedCallbackFrames = 240;

/**
 * Checks that the callback is never handed more frames than it asked for.
 *
 * The endpoint buffer is not the callback size: an endpoint whose buffer is larger than the
 * requested callback has to serve it in several callbacks, because an application sizes its
 * scratch buffers from what the stream reports through `getFramesPerDataCallback()`. Sizing them
 * from the device buffer instead is what a callback that arrives many times larger overruns.
 */
void testCallbackFrameSize() {
  section("The data callback is handed no more frames than it asked for");

  const auto probe = std::make_shared<RenderProbe>();
  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Render)
                                       .setSampleRate(48000)
                                       .setChannelCount(2)
                                       .setFormat(AudioSampleFormat::kFloat)
                                       .setFramesPerDataCallback(kRequestedCallbackFrames)
                                       .setDataCallback(probe),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip(std::string("no playback endpoint is available (") + convertToText(opened) + ")");
    return;
  }

  check(stream->getFramesPerDataCallback() == kRequestedCallbackFrames,
      "the stream reports the callback size it was asked for");
  check(stream->getBufferSizeInFrames() > kRequestedCallbackFrames,
      "the device buffer is larger than one callback");

  check(stream->start() == Result::OK, "starting the stream succeeded");
  sleepMilliseconds(250);

  const auto callbacks = probe->callbacks();
  const auto maxFrames = probe->maxFramesPerCallback();
  std::printf("       callbacks=%d maxFramesPerCallback=%u\n", callbacks, maxFrames);

  check(callbacks > 0, "the data callback was called");
  check(maxFrames <= kRequestedCallbackFrames,
      "no callback was handed more frames than the stream was asked for");

  check(stream->stop() == Result::OK, "stopping the stream succeeded");
  check(stream->close() == Result::OK, "closing the stream succeeded");
}

// ---------------------------------------------------------------------------
//  Playback through write()
// ---------------------------------------------------------------------------

void testRenderBlocking() {
  section("Render stream driven by write()");

  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Render)
                                       .setSampleRate(48000)
                                       .setChannelCount(2)
                                       .setFormat(AudioSampleFormat::kFloat),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip(std::string("no playback endpoint is available (") + convertToText(opened) + ")");
    return;
  }

  check(stream->isXRunCountSupported(), "the blocking path can see dropped buffers");
  std::vector<uint8_t> wrongWay(stream->getBytesPerFrame());
  check(!stream->read(wrongWay.data(), 1, 0), "a render stream refuses to be read");
  check(stream->start() == Result::OK, "starting the blocking stream succeeded");

  constexpr AudioFrameCount kChunkFrames = 480;
  const auto chunk = makeTone(stream, kChunkFrames);
  int64_t accepted = 0;
  int timeouts = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto written = stream->write(chunk.data(), kChunkFrames, 100 * Definitions::kNanosPerMillisecond);
    if (written) {
      accepted += written.value();
    } else {
      ++timeouts;
    }
  }

  std::printf("       acceptedFrames=%lld timeouts=%d writtenFrames=%lld playedFrames=%lld\n",
      static_cast<long long>(accepted), timeouts,
      static_cast<long long>(stream->getFramesWritten()),
      static_cast<long long>(stream->getFramesRead()));

  check(accepted > 0, "write() accepted frames");
  check(timeouts == 0, "write() never timed out while the device was draining");
  check(stream->getFramesWritten() == accepted, "the stream counted every frame write() accepted");
  check(stream->getFramesRead() > 0, "the endpoint consumed frames");

  const auto xRuns = stream->getXRunCount();
  check(static_cast<bool>(xRuns), "the underrun counter can be read");
  std::printf("       xRuns=%u\n", xRuns ? xRuns.value() : 0u);

  check(stream->flush() == Result::OK, "flushing the stream succeeded");
  check(stream->stop() == Result::OK, "stopping the blocking stream succeeded");
  check(stream->close() == Result::OK, "closing the blocking stream succeeded");
}

// ---------------------------------------------------------------------------
//  Recording through read()
// ---------------------------------------------------------------------------

void testCaptureBlocking() {
  section("Capture stream driven by read()");

  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Capture)
                                       .setSampleRate(48000)
                                       .setChannelCount(1)
                                       .setFormat(AudioSampleFormat::kFloat),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip(std::string("no recording endpoint is available (") + convertToText(opened) + ")");
    return;
  }

  std::vector<uint8_t> wrongWay(stream->getBytesPerFrame());
  check(!stream->write(wrongWay.data(), 1, 0), "a capture stream refuses to be written");
  check(stream->start() == Result::OK, "starting the capture stream succeeded");

  constexpr AudioFrameCount kChunkFrames = 480;
  const auto bytesPerFrame = stream->getBytesPerFrame();
  std::vector<uint8_t> buffer(static_cast<size_t>(kChunkFrames) * bytesPerFrame, 0);
  int64_t received = 0;
  int timeouts = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto got = stream->read(buffer.data(), kChunkFrames, 100 * Definitions::kNanosPerMillisecond);
    if (got) {
      received += got.value();
    } else {
      ++timeouts;
    }
  }

  std::printf("       receivedFrames=%lld timeouts=%d capturedFrames=%lld\n",
      static_cast<long long>(received), timeouts,
      static_cast<long long>(stream->getFramesWritten()));

  check(received > 0, "read() returned frames");
  check(stream->getFramesRead() >= received, "the consumed frame counter advanced");

  check(stream->stop() == Result::OK, "stopping the blocking capture stream succeeded");
  check(stream->close() == Result::OK, "closing the blocking capture stream succeeded");
}

/**
 * Releases a capture device and opens it again.
 *
 * The section also checks what a released stream reports: it has no device behind it any
 * more, so it has to describe an unspecified format rather than the one that is gone. The
 * stream that is opened again is the one the application asked for, so it has to describe the
 * same device as the first open did rather than whatever the endpoint happens to be mixing.
 */
void testReleaseCaptureDevice() {
  section("A released capture stream forgets the device it described");

  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Capture)
                                       .setSampleRate(48000)
                                       .setChannelCount(1)
                                       .setFormat(AudioSampleFormat::kFloat),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip(std::string("no recording endpoint is available (") + convertToText(opened) + ")");
    return;
  }

  const auto sampleRate = stream->getSampleRate();
  const auto channelCount = stream->getChannelCount();
  const auto format = stream->getFormat();
  check(sampleRate > 0, "the open capture stream reports a sample rate");
  check(stream->getFormat() != AudioSampleFormat::kDefault, "the open capture stream reports a sample format");

  check(stream->release() == Result::OK, "the capture device was released");
  check(stream->getState() == StreamState::Uninitialized, "a released stream is Uninitialized");
  check(stream->getSampleRate() == 0, "a released stream reports no sample rate");
  check(stream->getFormat() == AudioSampleFormat::kDefault, "a released stream reports no sample format");
  check(stream->getBufferCapacityInFrames() == 0, "a released stream reports no buffer capacity");
  check(stream->release() == Result::ErrorClosed, "releasing twice is reported");

  // What the stream describes after the second open is the stream the application asked for, not
  // the endpoint's own mix format or the mode the first open fell back to: the request survives
  // the release, which is what makes opening it again the same stream.
  check(stream->open() == Result::OK, "the released stream can be opened again");
  check(stream->getState() == StreamState::Open, "the reopened stream is Open");
  check(stream->getSampleRate() == sampleRate, "the reopened stream described its device again");
  check(stream->getChannelCount() == channelCount, "the reopened stream described its channels again");
  check(stream->getFormat() == format, "the reopened stream described its sample format again");

  check(stream->close() == Result::OK, "the reopened stream closed");
}

// ---------------------------------------------------------------------------
//  The device clock
// ---------------------------------------------------------------------------

/**
 * Checks that the device clock is reported in frames of the stream.
 *
 * `IAudioClock::GetPosition()` reports ticks of the device clock, whose frequency is the
 * endpoint's own and is often not the stream's sample rate: a timestamp that returned those
 * ticks would describe a stream that runs several times faster than it does, and it would
 * disagree with `getFramesRead()`, which counts frames. Both are sampled here and compared
 * with the wall clock time between the two samples, and with each other.
 */
void testDeviceClock() {
  section("The device clock reports frames at the rate of the stream");

  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Capture)
                                       .setSampleRate(48000)
                                       .setChannelCount(1)
                                       .setFormat(AudioSampleFormat::kFloat)
                                       .setDataCallback(std::make_shared<CaptureProbe>()),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip(std::string("no recording endpoint is available (") + convertToText(opened) + ")");
    return;
  }

  int64_t refusedFrames = 0;
  int64_t refusedNanos = 0;
  check(stream->getTimestamp(AudioStream::ClockMode::Monotonic, &refusedFrames, &refusedNanos) != Result::OK,
      "a stream that is not running refuses to report a timestamp");

  check(stream->start() == Result::OK, "starting the capture stream succeeded");

  // The endpoint has no position to report before it recorded one, and both of its clock
  // readings start at zero: the stream is given a moment so that the samples below describe
  // a stream that has been running.
  sleepMilliseconds(50);

  int64_t firstFrames = 0;
  int64_t firstNanos = 0;
  const auto first = stream->getTimestamp(AudioStream::ClockMode::Monotonic, &firstFrames, &firstNanos);
  if (first == Result::ErrorUnimplemented) {
    skip("the recording endpoint does not report a device clock");
    stream->stop();
    stream->close();
    return;
  }
  check(first == Result::OK, "the running stream reports a timestamp");
  check(firstNanos > 0, "the timestamp carries a host time");

  // Long enough that the position the endpoint reports, which is paired with the host time
  // only as often as the driver updates it, is a small part of the window: the two samples
  // are compared below.
  sleepMilliseconds(1000);

  int64_t secondFrames = 0;
  int64_t secondNanos = 0;
  check(stream->getTimestamp(AudioStream::ClockMode::Monotonic, &secondFrames, &secondNanos) == Result::OK,
      "the stream reports a second timestamp");
  check(secondNanos > firstNanos, "the host time advanced with the stream");

  const auto elapsedSeconds = static_cast<double>(secondNanos - firstNanos) / 1e9;
  const auto measured = static_cast<double>(secondFrames - firstFrames);
  const auto expected = elapsedSeconds * static_cast<double>(stream->getSampleRate());
  const auto ratio = expected > 0.0 ? measured / expected : 0.0;
  std::printf("       firstNanos=%lld secondNanos=%lld elapsedSeconds=%.3f framesAdvanced=%lld expected=%.0f "
              "ratio=%.3f sampleRate=%llu\n",
      static_cast<long long>(firstNanos), static_cast<long long>(secondNanos), elapsedSeconds,
      static_cast<long long>(measured), expected, ratio,
      static_cast<unsigned long long>(stream->getSampleRate()));

  check(measured > 0, "the device clock advanced");
  // A clock reported in its own ticks would advance by the ratio between the clock frequency
  // and the sample rate, which is a whole multiple on the endpoints that run it at 192 kHz.
  // The window is wide enough that the packet the position is reported in stays well inside
  // the tolerance, which is what keeps this a rate test rather than a latency test.
  check(std::abs(ratio - 1.0) <= 0.1, "the clock advanced at the rate of the stream");

  // The two ways of asking where the stream is have to agree: the timestamp is the frame
  // counter's own basis, not a second one.
  const auto consumed = stream->getFramesRead();
  const auto tolerance = static_cast<double>(stream->getSampleRate()) * 0.5;
  std::printf("       timestampFrames=%lld consumedFrames=%lld\n", static_cast<long long>(secondFrames),
      static_cast<long long>(consumed));
  check(std::abs(static_cast<double>(secondFrames) - static_cast<double>(consumed)) <= tolerance,
      "the timestamp and the frame counter report the same basis");

  // A flush resets the client, which restarts the device clock at zero. The position is part
  // of a monotonic timeline, so it has to keep growing across it instead of jumping back.
  const auto beforeFlushFrames = secondFrames;
  check(stream->flush() == Result::OK, "flushing the capture stream succeeded");
  check(stream->start() == Result::OK, "the flushed capture stream started again");
  sleepMilliseconds(100);

  int64_t afterFlushFrames = 0;
  int64_t afterFlushNanos = 0;
  check(stream->getTimestamp(AudioStream::ClockMode::Monotonic, &afterFlushFrames, &afterFlushNanos) == Result::OK,
      "the stream reports a timestamp after the flush");
  std::printf("       beforeFlush=%lld afterFlush=%lld\n", static_cast<long long>(beforeFlushFrames),
      static_cast<long long>(afterFlushFrames));
  check(afterFlushFrames >= beforeFlushFrames, "the position did not go backwards across a flush");

  check(stream->stop() == Result::OK, "stopping the capture stream succeeded");
  check(stream->close() == Result::OK, "closing the capture stream succeeded");
}

// ---------------------------------------------------------------------------
//  Releasing without closing
// ---------------------------------------------------------------------------

void testReleaseKeepsTheStreamUsable() {
  section("release() frees the device and leaves the object usable");

  const auto probe = std::make_shared<RenderProbe>();
  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Render)
                                       .setSampleRate(48000)
                                       .setChannelCount(2)
                                       .setFormat(AudioSampleFormat::kFloat)
                                       .setDataCallback(probe),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip("no playback endpoint is available");
    return;
  }

  check(stream->start() == Result::OK, "the stream started");
  const auto sampleRate = stream->getSampleRate();
  const auto format = stream->getFormat();
  check(stream->release() == Result::OK, "the device was released while running");
  check(stream->release() == Result::ErrorClosed, "releasing twice is reported");
  check(stream->open() == Result::OK, "the released stream can be opened again");
  check(stream->getState() == StreamState::Open, "the reopened stream is Open");
  check(stream->getSampleRate() == sampleRate, "the reopened stream described its device again");
  check(stream->getFormat() == format, "the reopened stream described its sample format again");
  check(stream->close() == Result::OK, "the reopened stream closed");
}

// ---------------------------------------------------------------------------
//  Closing from inside the callback
// ---------------------------------------------------------------------------

/**
 * Closes its stream from inside the data callback.
 *
 * This is the documented "stop and close on another thread" restriction, and it is
 * the case where the stream thread has to detach from itself instead of being
 * joined. Nothing may crash, and the stream has to end up closed.
 */
class SelfClosingProbe final : public AudioStreamDataCallback {
public:
  DataCallbackResult onAudioReady(AudioStream* stream, void* /* audioData */, AudioFrameCount /* numFrames */) override {
    if (mCallbacks.fetch_add(1) + 1 == kCloseOnCallback) {
      mCloseResult.store(stream->close());
    }
    return DataCallbackResult::Continue;
  }

  int callbacks() const {
    return mCallbacks.load();
  }

  Result closeResult() const {
    return mCloseResult.load();
  }

  static constexpr int kCloseOnCallback = 5;

private:
  std::atomic<int> mCallbacks{ 0 };
  std::atomic<Result> mCloseResult{ Result::ErrorInvalidState };
};

void testCloseFromCallback() {
  section("Closing the stream from inside its own data callback");

  const auto probe = std::make_shared<SelfClosingProbe>();
  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Render)
                                       .setSampleRate(48000)
                                       .setChannelCount(2)
                                       .setFormat(AudioSampleFormat::kFloat)
                                       .setDataCallback(probe),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip("no playback endpoint is available");
    return;
  }

  check(stream->start() == Result::OK, "the stream started");
  sleepMilliseconds(200);

  check(probe->callbacks() >= SelfClosingProbe::kCloseOnCallback, "the callback ran often enough to close");
  check(probe->closeResult() == Result::OK, "closing from the callback succeeded");
  check(stream->getState() == StreamState::Closed, "the stream is Closed");
  check(stream->close() == Result::ErrorClosed, "closing it again is reported");
}

/**
 * Closes a capture stream from inside its own data callback.
 *
 * The capture side drops the client lock for the callback exactly as the render side does,
 * so it has to survive the same teardown: the packet it is holding belongs to a client that
 * is gone, and the stream has to end up closed.
 */
void testCloseCaptureFromCallback() {
  section("Closing a capture stream from inside its own data callback");

  const auto probe = std::make_shared<SelfClosingProbe>();
  AudioStreamBuilder builder;
  std::shared_ptr<AudioStream> stream;
  const Result opened = openStream(builder.setDirection(Direction::Capture)
                                       .setSampleRate(48000)
                                       .setChannelCount(1)
                                       .setFormat(AudioSampleFormat::kFloat)
                                       .setDataCallback(probe),
      stream);
  if (opened != Result::OK || stream == nullptr) {
    skip(std::string("no recording endpoint is available (") + convertToText(opened) + ")");
    return;
  }

  check(stream->start() == Result::OK, "the capture stream started");
  sleepMilliseconds(300);

  check(probe->callbacks() >= SelfClosingProbe::kCloseOnCallback, "the callback ran often enough to close");
  check(probe->closeResult() == Result::OK, "closing from the callback succeeded");
  check(stream->getState() == StreamState::Closed, "the capture stream is Closed");
  check(stream->close() == Result::ErrorClosed, "closing it again is reported");
}

} // namespace anonymous

int main() {
  std::printf("AudioStreamWasapi tests\n");
  testRenderCallback();
  testCallbackFrameSize();
  testCaptureCallback();
  testRenderBlocking();
  testCaptureBlocking();
  testReleaseCaptureDevice();
  testCloseCaptureFromCallback();
  testDeviceClock();
  testReleaseKeepsTheStreamUsable();
  testCloseFromCallback();

  std::printf("\n%d checks, %d failures, %d skipped sections\n", gChecks, gFailures, gSkips);
  std::printf("%s\n", gFailures == 0 ? "tests_AudioStreamWasapi: PASS" : "tests_AudioStreamWasapi: FAIL");
  return gFailures == 0 ? 0 : 1;
}

#endif // _WIN32
