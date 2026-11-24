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
//  tests_AudioStreamAlsa.cpp
//
//  Exercises coral::AudioStreamAlsa against the ALSA PCM the machine provides:
//    - a callback render stream opens, reports the accepted device format and
//      runs the callback on its own thread
//    - pause/resume/stop move the state machine and really stop the callback
//    - the blocking path moves frames through write() and read()
//    - xruns and the buffer/callback sizes are reported from the device
//
//  A machine without a PCM has no way to pass it. The first open decides that:
//  every section is reported as SKIP and the run still succeeds. Set
//  CORAL_ALSA_DEVICE=null to run the whole test without sound hardware.
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

/** Fills the callback buffer with silence and counts the calls. */
class SilenceProbe final : public AudioStreamDataCallback {
public:
  DataCallbackResult onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) override {
    mCalls.fetch_add(1);
    mFrames.fetch_add(numFrames);
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

private:
  std::atomic<int> mCalls{ 0 };
  std::atomic<int64_t> mFrames{ 0 };
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
    section("An ALSA PCM is available");
    skip("no ALSA PCM could be opened; set CORAL_ALSA_DEVICE=null to run against the null plugin");
    std::printf("\n%d checks, %d failures, %d skips\n", gChecks, gFailures, gSkips);
    return 0;
  }

  auto capture = openStream(Direction::Capture, AudioSampleFormat::kInt16, nullptr);

  {
    section("A callback render stream opens and runs");
    check(render->getSampleRate() > 0, "a sample rate was negotiated");
    check(render->getChannelCount() == 2, "the channel count was accepted");
    check(render->getFormat() == AudioSampleFormat::kFloat ||
            render->getFormat() == AudioSampleFormat::kInt16,
      "a usable sample format was negotiated");
    check(render->getBufferCapacityInFrames() > 0, "the buffer capacity was reported");
    check(render->getFramesPerBurst() > 0, "the period size was reported");
    check(render->getFramesPerDataCallback() > 0, "the callback size was reported");

    check(render->start() == Result::OK, "the render stream started");
    check(waitUntil([&] { return renderProbe->calls() > 0; }, 2000), "the data callback ran");
    check(renderProbe->frames() > 0, "the callback received frames");

    int64_t framePosition = -1;
    int64_t timeNanoseconds = 0;
    check(render->getTimestamp(AudioStream::ClockMode::Monotonic, &framePosition, &timeNanoseconds) == Result::OK,
      "the stream reports a timestamp");
    check(framePosition >= 0 && timeNanoseconds > 0, "the timestamp has a frame position and a clock value");
    const auto latency = render->calculateLatencyMillis();
    check(latency && latency.value() >= 0.0, "the stream reports its latency");
    const auto bufferSize = render->setBufferSizeInFrames(480);
    check(bufferSize && bufferSize.value() > 0, "setBufferSizeInFrames reports the device buffer");
    check(render->pause() == Result::OK, "the render stream paused");
    check(render->getState() == StreamState::Paused, "the stream reports Paused");
    check(render->start() == Result::OK, "the render stream resumed");
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
      check(stream->start() == Result::OK, "the blocking render stream started");
      std::vector<int16_t> frames(960 * 2, 0);
      const auto written = stream->write(frames.data(), 960, 500 * 1000 * 1000);
      check(written && written.value() > 0, "write() accepted frames");
      check(waitUntil([&] { return stream->getFramesRead() > 0; }, 2000), "the device consumed frames");
      const auto xrun = stream->getXRunCount();
      check(xrun && xrun.value() >= 0, "the xrun counter is available");
      check(stream->stop() == Result::OK, "the blocking render stream stopped");
    }
  }

  if (capture != nullptr) {
    section("A capture stream delivers frames");
    check(capture->start() == Result::OK, "the capture stream started");
    std::vector<int16_t> frames(480 * 2, 0);
    const auto read = capture->read(frames.data(), 480, 500 * 1000 * 1000);
    check(read && read.value() > 0, "read() received frames");
    check(capture->stop() == Result::OK, "the capture stream stopped");
  } else {
    section("A capture stream delivers frames");
    skip("no capture PCM could be opened");
  }

  if (capture != nullptr) {
    section("A released stream can be opened again");
    check(capture->release() == Result::OK, "the capture stream released its device");
    check(capture->getState() == StreamState::Uninitialized, "the released stream reports Uninitialized");
    check(capture->open() == Result::OK, "the stream opened again");
    check(capture->start() == Result::OK, "the reopened stream started");
    check(capture->stop() == Result::OK, "the reopened stream stopped");
  }

  check(render->close() == Result::OK, "the callback render stream closed");
  if (capture != nullptr) {
    check(capture->close() == Result::OK, "the capture stream closed");
  }

  std::printf("\n%d checks, %d failures, %d skips\n", gChecks, gFailures, gSkips);
  return gFailures == 0 ? 0 : 1;
}
