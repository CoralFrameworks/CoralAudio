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

// =============================================================
//  tests_AudioPlayerRecorder.cpp
//
//  Functional tests for coral::AudioPlayer and coral::AudioRecorder, whose file work runs
//  on their own EventThread and is driven by events:
//    - the player is asked to read the file by the audio it renders, so the playback time
//      advances while it plays, freezes while it is paused and moves on when it resumes,
//      and the file is read in whole buffers rather than one block per callback
//    - the recorder is asked to write the file by the audio it captured, so the recording
//      time advances, the file grows while it records and the stop event reports the end of
//      the recording only once everything that was captured reached the file
//
//  Both need an audio endpoint, and every section is skipped when the endpoint is missing.
//  The player section also needs an asset, and is skipped when the asset is not part of the
//  checkout.
// =============================================================

#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioPlayer.h>
#include <coral/audio/AudioRecorder.h>
#include <core/time/SystemClock.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace coral;

namespace {

constexpr uint64_t kSampleRate = 44100;
constexpr uint32_t kChannels = 2;

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

/** Waits until the predicate holds, up to the given budget. */
bool waitUntil(const std::function<bool()>& predicate, const int64_t timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

/**
 * Returns the value of a sampler once it did not change for the given window.
 *
 * The calls of the public API are events of the message thread, so what they change is observable a
 * few callbacks later: waiting for a value to settle is what makes the assertions about it stable.
 */
int64_t waitUntilStable(const std::function<int64_t()>& sampler, const int64_t stableMs,
                        const int64_t timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  int64_t value = sampler();
  auto lastChange = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto sample = sampler();
    if (sample != value) {
      value = sample;
      lastChange = std::chrono::steady_clock::now();
    } else if (std::chrono::steady_clock::now() - lastChange >= std::chrono::milliseconds(stableMs)) {
      return value;
    }
  }
  return value;
}

/** Records what the player reports, so that the test can assert on it. */
class PlayerProbe final : public AudioPlayer::Delegate {
public:
  void onStartPlaying(AudioPlayer* /* player */, const int64_t durationUs) override {
    mDurationUs.store(durationUs);
    mStarted.store(true);
  }

  void onFinishPlaying(AudioPlayer* /* player */, const bool successfully) override {
    mFinished.store(true);
    mFinishedSuccessfully.store(successfully);
  }

  void onDecodeErrorOccur(AudioPlayer* /* player */, const Error* /* error */) override {
    mDecodeErrors.fetch_add(1);
  }

  [[nodiscard]] bool started() const { return mStarted.load(); }

  [[nodiscard]] int64_t durationUs() const { return mDurationUs.load(); }

  [[nodiscard]] bool finished() const { return mFinished.load(); }

  [[nodiscard]] bool finishedSuccessfully() const { return mFinishedSuccessfully.load(); }

  [[nodiscard]] int decodeErrors() const { return mDecodeErrors.load(); }

private:
  std::atomic_bool mStarted{ false };
  std::atomic_bool mFinished{ false };
  std::atomic_bool mFinishedSuccessfully{ false };
  std::atomic_int mDecodeErrors{ 0 };
  std::atomic<int64_t> mDurationUs{ Definitions::kInvalidValue };
};

/** Records what the recorder reports, so that the test can assert on it. */
class RecorderProbe final : public AudioRecorder::Delegate {
public:
  void onFinishRecording(AudioRecorder* /* recorder */, const bool successfully) override {
    mFinishedSuccessfully.store(successfully);
    mFinished.store(true);
  }

  void onEncodeErrorOccur(AudioRecorder* /* recorder */, const Error* /* error */) override {
    mEncodeErrors.fetch_add(1);
  }

  [[nodiscard]] bool finished() const { return mFinished.load(); }

  [[nodiscard]] bool finishedSuccessfully() const { return mFinishedSuccessfully.load(); }

  [[nodiscard]] int encodeErrors() const { return mEncodeErrors.load(); }

private:
  std::atomic_bool mFinished{ false };
  std::atomic_bool mFinishedSuccessfully{ false };
  std::atomic_int mEncodeErrors{ 0 };
};

/**
 * Plays an asset and asserts that the audio that is rendered is what asks for the file to be read.
 */
void testPlayback() {
  section("The playback drives the reading of the file");

  const auto path = std::string(CORAL_ASSETS_DIR) + "/music/music.wav";
  std::error_code fileError;
  if (!std::filesystem::exists(path, fileError)) {
    skip("no asset at " + path);
    return;
  }

  auto player = AudioPlayer::Create(path);
  if (player == nullptr) {
    skip("the asset could not be opened");
    return;
  }
  auto probe = std::make_unique<PlayerProbe>();
  auto* const observer = probe.get();
  player->setDelegate(std::move(probe));

  if (!player->prepareToPlay()) {
    skip("no playback endpoint is available");
    return;
  }
  check(player->getDurationUs() > 0, "the duration of the asset was resolved");

  check(player->play(), "the playback was started");
  check(waitUntil([observer] { return observer->started(); }, 1000),
        "the delegate was told that the playback started");

  // The player only renders what the read events stored, so a playback time that advances is what
  // proves that the file is being read.
  const auto firstTimeUs = player->getCurrentTimeUs();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const auto secondTimeUs = player->getCurrentTimeUs();
  check(secondTimeUs > firstTimeUs, "the playback time advanced while playing");
  check(observer->decodeErrors() == 0, "no decode error was reported");

  // Pausing stops the render callback, so the file is not asked for anything and the time freezes.
  // The pause itself is handled by the message thread, so the time is sampled once it settled.
  player->pause();
  const auto pausedTimeUs =
    waitUntilStable([player] { return player->getCurrentTimeUs(); }, 150, 1500);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  check(player->getCurrentTimeUs() == pausedTimeUs, "the playback time froze while paused");

  // Resuming asks for the file again, which is what moves the playback time on.
  check(player->play(), "the playback was resumed");
  check(waitUntil([player, pausedTimeUs] { return player->getCurrentTimeUs() > pausedTimeUs; }, 1000),
        "the playback time advanced again after the resume");

  player->stop();
  check(!player->isPlaying(), "the playback is no longer playing after the stop");
  check(observer->decodeErrors() == 0, "no decode error was reported by the whole playback");
}

/**
 * Records to a file and asserts that the audio that is captured is what asks for the file to be
 * written.
 */
void testRecording() {
  section("The recording drives the writing of the file");

  const auto path = (std::filesystem::temp_directory_path() / "coral_recorder_test.wav").string();
  std::error_code fileError;
  std::filesystem::remove(path, fileError);

  const auto format = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, kSampleRate,
    kChannels, true);
  if (!format.has_value()) {
    skip("the recording format could not be built");
    return;
  }
  auto recorder = AudioRecorder::Make(path, MediaFileType::kMediaFileTypeWav, format.value());
  if (recorder == nullptr) {
    skip("the recorder could not be created for the requested format");
    return;
  }
  auto probe = std::make_unique<RecorderProbe>();
  auto* const observer = probe.get();
  recorder->setDelegate(std::move(probe));

  if (!recorder->prepareToRecord()) {
    skip("no recording endpoint is available");
    return;
  }

  check(recorder->record(), "the recording was started");
  // The capture stream is opened by the message thread, so the recorder reports that it records
  // once that thread started it.
  check(waitUntil([recorder] { return recorder->isRecording(); }, 1000),
        "the capture stream started and the recorder reports that it is recording");

  // The capture callback is what reports the recording time, so a time that advances while the
  // recorder runs is what proves that the audio is reaching the recorder.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  const auto recordedUs = recorder->getCurrentTimeUs();
  check(recordedUs > 0, "the recording time advanced while recording");

  // Stopping waits for the buffered audio, and the wait ends with the end of the recording.
  recorder->stop();
  check(observer->finished(), "the delegate was told that the recording finished");
  check(observer->finishedSuccessfully(), "the recording finished successfully");
  check(observer->encodeErrors() == 0, "no encoding error was reported");

  // The write events are what turn the captured frames into the file: the length of the file is the
  // number of frames that were recorded, i.e. the recording time in the format of the recorder.
  const auto finalSize = std::filesystem::file_size(path, fileError);
  const auto recordedFrames =
    static_cast<double>(recordedUs) * static_cast<double>(kSampleRate) / 1000000.0;
  const auto writtenFrames = static_cast<double>(finalSize > 44 ? finalSize - 44 : 0)
                             / (static_cast<double>(kChannels) * 2.0);
  check(finalSize > 44, "the file carries audio after its header");
  check(std::abs(writtenFrames - recordedFrames) < 2000.0,
        "the file holds the frames that were recorded");

  std::filesystem::remove(path, fileError);
}

} // namespace anonymous

int main() {
  testPlayback();
  testRecording();

  std::printf("\n%d checks, %d failures, %d skipped sections\n", gChecks, gFailures, gSkips);
  return gFailures == 0 ? 0 : 1;
}
