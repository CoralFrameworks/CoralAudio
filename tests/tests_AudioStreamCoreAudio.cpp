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
//  tests_AudioStreamCoreAudio.cpp
//
//  Exercises coral::AudioStreamCoreAudio against the devices the Mac provides:
//    - a callback render stream opens, reports the accepted device format and runs the callback on
//      Core Audio's IO thread
//    - the sample format the caller asked for is the one the callback receives (the player and the
//      graph rely on that)
//    - pause/resume/stop move the state machine and really stop the callback
//    - the blocking path moves frames through write() and read()
//    - the capture path delivers frames
//    - xruns, the device buffer, the timestamp and the latency are reported
//    - a released stream can be opened again
//
//  A Mac without an output (or input) device has no way to pass the corresponding section: the
//  first open decides that and the section is reported as SKIP while the run still succeeds.
//  CORAL_COREAUDIO_DEVICE=<name or UID> runs against a specific device.
// =============================================================

#include "private/audio/AudioStream.h"
#include "private/audio/AudioStreamBuilder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
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

bool waitUntil(const std::function<bool()>& predicate, const int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return predicate();
}

/** Fills the callback buffer with silence, counts the calls and records the largest callback. */
class SilenceProbe final : public AudioStreamDataCallback {
public:
  SilenceProbe() = default;

  DataCallbackResult onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) override {
    mCalls.fetch_add(1);
    mFrames.fetch_add(numFrames);
    int largest = mLargestCallback.load();
    const int frames = static_cast<int>(numFrames);
    while (frames > largest && !mLargestCallback.compare_exchange_weak(largest, frames)) {
    }
    if (audioData != nullptr) {
      std::memset(audioData, 0, static_cast<size_t>(numFrames) * stream->getBytesPerFrame());
    }
    return DataCallbackResult::Continue;
  }

  [[nodiscard]] int calls() const {
    return mCalls.load();
  }

  [[nodiscard]] int64_t frames() const {
    return mFrames.load();
  }

  [[nodiscard]] int largestCallback() const {
    return mLargestCallback.load();
  }

private:
  std::atomic<int> mCalls{ 0 };
  std::atomic<int64_t> mFrames{ 0 };
  std::atomic<int> mLargestCallback{ 0 };
};

std::shared_ptr<AudioStream> openStream(const Direction direction,
  const AudioSampleFormat format,
  const std::shared_ptr<AudioStreamDataCallback>& callback) {
  AudioStreamBuilder builder;
  builder.setDirection(direction)
    .setChannelCount(2)
    .setSampleRate(48000)
    .setFormat(format)
    .setFramesPerDataCallback(480)
    .setBufferCapacityInFrames(1920);
  if (callback != nullptr) {
    builder.setDataCallback(callback);
  }

  std::shared_ptr<AudioStream> stream;
  const Result result = builder.openStream(stream);
  if (result != Result::OK || stream == nullptr) {
    std::printf("  (open failed: %s)\n", convertToText(result));
    return nullptr;
  }
  return stream;
}

} // namespace anonymous

int main() {
  auto renderProbe = std::make_shared<SilenceProbe>();

  auto render = openStream(Direction::Render, AudioSampleFormat::kFloat, renderProbe);
  if (render == nullptr) {
    section("A Core Audio output device is available");
    skip("no output device could be opened; set CORAL_COREAUDIO_DEVICE to select one");
    std::printf("\n%d checks, %d failures, %d skips\n", gChecks, gFailures, gSkips);
    return 0;
  }

  auto capture = openStream(Direction::Capture, AudioSampleFormat::kInt16, nullptr);

  {
    section("A callback render stream opens and runs");
    check(render->getSampleRate() == 48000, "the requested sample rate was accepted");
    check(render->getChannelCount() == 2, "the channel count was accepted");
    check(render->getFormat() == AudioSampleFormat::kFloat, "the requested sample format was accepted");
    check(render->getBufferCapacityInFrames() > 0, "the buffer capacity was reported");
    check(render->getFramesPerBurst() > 0, "the device buffer size was reported");
    check(render->getFramesPerDataCallback() > 0, "the callback size was reported");
    check(render->isXRunCountSupported(), "xrun counting is supported");

    check(render->start() == Result::OK, "the render stream started");
    check(waitUntil([&] { return renderProbe->calls() > 0; }, 2000), "the data callback ran");
    check(renderProbe->frames() > 0, "the callback received frames");

    int64_t framePosition = -1;
    int64_t timeNanoseconds = 0;
    check(render->getTimestamp(AudioStream::ClockMode::Monotonic, &framePosition, &timeNanoseconds) == Result::OK,
      "the stream reports a timestamp");
    check(framePosition >= 0 && timeNanoseconds > 0, "the timestamp has a frame position and a clock value");
    int64_t realtimeNanoseconds = 0;
    check(render->getTimestamp(AudioStream::ClockMode::Realtime, &framePosition, &realtimeNanoseconds) == Result::OK,
      "the stream reports a realtime timestamp");
    check(realtimeNanoseconds >= timeNanoseconds - 1000000000LL,
      "the realtime timestamp is not behind the monotonic one");
    const auto latency = render->calculateLatencyMillis();
    check(latency && latency.value() >= 0.0, "the stream reports its latency");
    const auto bufferSize = render->setBufferSizeInFrames(480);
    check(bufferSize && bufferSize.value() > 0, "setBufferSizeInFrames reports the device buffer");

    check(render->pause() == Result::OK, "the render stream paused");
    check(render->getState() == StreamState::Paused, "the stream reports Paused");
    const int callsAtPause = renderProbe->calls();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(renderProbe->calls() == callsAtPause, "the data callback stopped while paused");

    check(render->start() == Result::OK, "the render stream resumed");
    check(waitUntil([&] { return renderProbe->calls() > callsAtPause; }, 2000),
      "the data callback ran again after the resume");
    check(render->flush() == Result::OK, "the render stream flushed");
    check(render->getState() == StreamState::Flushed, "the stream reports Flushed");
    check(render->stop() == Result::OK, "the render stream stopped");
    check(render->getState() == StreamState::Stopped, "the stream reports Stopped");
  }

  {
    section("The blocking path moves frames through the ring buffer");
    AudioStreamBuilder builder;
    std::shared_ptr<AudioStream> stream;
    const Result result = builder.setDirection(Direction::Render)
                            .setChannelCount(2)
                            .setSampleRate(48000)
                            .setFormat(AudioSampleFormat::kInt16)
                            .setFramesPerDataCallback(480)
                            .setBufferCapacityInFrames(1920)
                            .openStream(stream);
    check(result == Result::OK && stream != nullptr, "the blocking render stream opened");
    if (stream != nullptr) {
      check(stream->getFormat() == AudioSampleFormat::kInt16,
        "the stream kept the int16 client format the player needs");
      check(stream->start() == Result::OK, "the blocking render stream started");
      std::vector<int16_t> frames(960 * 2, 0);
      const auto written = stream->write(frames.data(), 960, 500 * 1000 * 1000);
      check(written && written.value() > 0, "write() accepted frames");
      check(waitUntil([&] { return stream->getFramesRead() > 0; }, 2000), "the device consumed frames");
      const auto xrun = stream->getXRunCount();
      check(xrun && xrun.value() >= 0, "the xrun counter is available");
      check(stream->stop() == Result::OK, "the blocking render stream stopped");
      check(stream->close() == Result::OK, "the blocking render stream closed");
    }
  }

  {
    section("The requested callback size is honored");
    auto probe = std::make_shared<SilenceProbe>();
    AudioStreamBuilder builder;
    std::shared_ptr<AudioStream> stream;
    const Result result = builder.setDirection(Direction::Render)
                            .setChannelCount(2)
                            .setSampleRate(48000)
                            .setFormat(AudioSampleFormat::kInt16)
                            .setFramesPerDataCallback(240)
                            .setBufferCapacityInFrames(1920)
                            .setDataCallback(probe)
                            .openStream(stream);
    check(result == Result::OK && stream != nullptr, "the callback stream opened");
    if (stream != nullptr) {
      check(stream->getFramesPerDataCallback() <= 240,
        "the callback size is bounded by the requested one");
      check(stream->start() == Result::OK, "the callback stream started");
      check(waitUntil([&] { return probe->calls() > 4; }, 2000), "the split callback ran several times");
      check(probe->largestCallback() > 0 && probe->largestCallback() <= 240,
        "no callback carried more frames than requested");
      check(stream->stop() == Result::OK, "the callback stream stopped");
      check(stream->close() == Result::OK, "the callback stream closed");
    }
  }

  {
    section("Every client sample format is accepted and reported");
    struct FormatCase {
      AudioSampleFormat format;
      const char* name;
    };
    const FormatCase cases[] = {
      { AudioSampleFormat::kFloat, "float32" },
      { AudioSampleFormat::kInt16, "int16" },
      { AudioSampleFormat::kInt24, "int24" },
      { AudioSampleFormat::kInt32, "int32" },
    };
    for (const auto& formatCase : cases) {
      AudioStreamBuilder builder;
      std::shared_ptr<AudioStream> stream;
      const Result result = builder.setDirection(Direction::Render)
                              .setChannelCount(2)
                              .setSampleRate(48000)
                              .setFormat(formatCase.format)
                              .setFramesPerDataCallback(480)
                              .setBufferCapacityInFrames(1920)
                              .openStream(stream);
      check(result == Result::OK && stream != nullptr,
        std::string("a ") + formatCase.name + " render stream opened");
      if (stream == nullptr) {
        continue;
      }
      check(stream->getFormat() == formatCase.format,
        std::string("the ") + formatCase.name + " format is reported");
      check(stream->start() == Result::OK, std::string("the ") + formatCase.name + " stream started");
      std::vector<uint8_t> frames(static_cast<size_t>(960) * stream->getBytesPerFrame(), 0);
      const auto written = stream->write(frames.data(), 960, 500 * 1000 * 1000);
      check(written && written.value() > 0, std::string("write() accepted ") + formatCase.name + " frames");
      check(stream->stop() == Result::OK, std::string("the ") + formatCase.name + " stream stopped");
      check(stream->close() == Result::OK, std::string("the ") + formatCase.name + " stream closed");
    }
  }

  if (capture != nullptr) {
    section("A capture stream delivers frames");
    check(capture->getFormat() == AudioSampleFormat::kInt16, "the capture stream kept the int16 format");
    check(capture->start() == Result::OK, "the capture stream started");
    std::vector<int16_t> frames(480 * 2, 0);
    const auto read = capture->read(frames.data(), 480, 500 * 1000 * 1000);
    check(read && read.value() > 0, "read() received frames");
    check(capture->stop() == Result::OK, "the capture stream stopped");
  } else {
    section("A capture stream delivers frames");
    skip("no input device could be opened; set CORAL_COREAUDIO_DEVICE to select one");
  }

  if (capture != nullptr) {
    section("A released stream can be opened again");
    check(capture->release() == Result::OK, "the capture stream released its device");
    check(capture->getState() == StreamState::Uninitialized, "the released stream reports Uninitialized");
    check(capture->open() == Result::OK, "the stream opened again");
    check(capture->start() == Result::OK, "the reopened stream started");
    check(capture->stop() == Result::OK, "the reopened stream stopped");
  }

  {
    section("The client format can differ from the device format");
    auto probe = std::make_shared<SilenceProbe>();
    AudioStreamBuilder builder;
    std::shared_ptr<AudioStream> stream;
    const Result result = builder.setDirection(Direction::Render)
                            .setChannelCount(1)
                            .setSampleRate(44100)
                            .setFormat(AudioSampleFormat::kFloat)
                            .setFramesPerDataCallback(441)
                            .setBufferCapacityInFrames(1764)
                            .setDataCallback(probe)
                            .openStream(stream);
    check(result == Result::OK && stream != nullptr,
      "a 44.1 kHz mono stream opened on a device that may run at another rate");
    if (stream != nullptr) {
      check(stream->getSampleRate() == 44100, "the stream reports the client sample rate");
      check(stream->getChannelCount() == 1, "the stream reports the client channel count");
      check(stream->start() == Result::OK, "the converted stream started");
      check(waitUntil([&] { return probe->calls() > 0; }, 2000),
        "Core Audio converted the client format and ran the callback");
      check(probe->largestCallback() <= 441, "the converted stream honors the callback size");
      check(stream->stop() == Result::OK, "the converted stream stopped");
      check(stream->close() == Result::OK, "the converted stream closed");
    }
  }

  check(render->close() == Result::OK, "the callback render stream closed");
  if (capture != nullptr) {
    check(capture->close() == Result::OK, "the capture stream closed");
  }

  std::printf("\n%d checks, %d failures, %d skips\n", gChecks, gFailures, gSkips);
  return gFailures == 0 ? 0 : 1;
}
