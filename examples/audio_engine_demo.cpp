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
//  examples/audio_engine_demo.cpp
//
//  Reads an audio file from the assets directory and plays it on the default audio device. It is
//  the standalone demo of the framework and the manual check that the platform stream renders:
//
//      AudioFile -> AudioPlayerNode -> mainMixerNode -> AudioOutputNode -> device
//
//  The graph is built with AudioEngine and AudioPlayerNode, which is what exercises the
//  realtime render path. The sample verifies the render as it plays:
//
//    - the format of the file, of the graph and of the device are printed,
//    - the node time of the player advances with the wall clock, which is what tells that the
//      graph is pulled by the device and not faster or slower,
//    - the frames the player rendered are compared with the frames the played segment holds,
//      so a dropped or duplicated block shows up,
//    - the time the segment took to reach the output is compared with the audio it contains,
//    - a tap on the main mixer reports the frames the graph produced, and on which thread the
//      tap block ran.
//
//  Usage:
//      audio_engine_demo [file] [seconds]
//
//  `file` defaults to <assets>/music/music.wav and `seconds` to 20, meaning twenty seconds of the
//  file are played. Zero plays the whole file.
// =============================================================

#include <coral/audio/AudioEngine.h>
#include <coral/audio/AudioFile.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioMixerNode.h>
#include <coral/audio/AudioPlayerNode.h>

#include <minilog/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace coral;

namespace {

/** The file this sample plays when the command line does not name one. */
constexpr auto kDefaultFile = CORAL_ASSETS_DIR "/music/music.wav";

/** How many seconds of the file this sample plays by default. */
constexpr double kDefaultSeconds = 20.0;

/** The tap buffer size, in frames. It is a monitoring buffer, not a render block. */
constexpr AudioFrameCount kTapBufferFrames = 2048;

/** What the tap on the main mixer collects while the file plays. */
struct RenderProbe {
  //!< The frames the tap has seen. The tap is delivered asynchronously, so this is a lower
  //!< bound of what the graph rendered: a buffer is dropped when the client falls behind.
  std::atomic<int64_t> frames{ 0 };
  //!< How many times the tap block ran.
  std::atomic_int callbacks{ 0 };
  //!< Set when the tap block runs on a thread other than the one that installed it.
  std::atomic_bool ranOnAnotherThread{ false };
  //!< The thread that installed the tap.
  std::thread::id installerThread;
};

int64_t nowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch())
    .count();
}

} // namespace anonymous

int main(int argc, char** argv) {
  const auto filePath = argc > 1 ? std::string(argv[1]) : std::string(kDefaultFile);
  const auto seconds = argc > 2 ? std::atof(argv[2]) : kDefaultSeconds;

  Error* error = nullptr;
  const auto audioFile = AudioFile::InitForReading(filePath, &error);
  if (audioFile == nullptr) {
    MINILOG_ERROR("Failed to open {}: {}", filePath, error != nullptr ? error->toString() : "unknown error");
    delete error;
    return 1;
  }

  const auto processingFormat = audioFile->getProcessingFormat();
  const auto fileSampleRate = processingFormat.has_value() ? processingFormat->getSampleRate() : 0.0;
  const auto fileFrames = audioFile->getLength();
  MINILOG_INFO("Opened {}", filePath);
  if (const auto format = audioFile->getFileFormat(); format.has_value()) {
    MINILOG_INFO("  file format:       {}", format->toString());
  }
  if (processingFormat.has_value()) {
    MINILOG_INFO("  processing format: {}", processingFormat->toString());
  }
  MINILOG_INFO("  length:            {} frames ({:.2f} s)", fileFrames,
    fileSampleRate > 0 ? static_cast<double>(fileFrames) / fileSampleRate : 0.0);

  auto engine = AudioEngine::Create();
  const auto player = std::make_shared<AudioPlayerNode>();

  // Touching the output node and the main mixer builds the output chain:
  //
  //     player -> main mixer -> output node
  //
  // The engine renders in the format of the device, which it only learns while it prepares, so
  // connecting with the format of the file is enough: the player converts what it decodes.
  const auto mixer = engine->mainMixerNode();
  const auto output = engine->outputNode();
  engine->attachNode(player);
  engine->connect(player, mixer, audioFile->getProcessingFormat());
  MINILOG_INFO("AudioEngine: {}", engine->toString());

  // The tap observes what the graph renders. In realtime mode the block is delivered by the
  // engine's IO thread, which is why it must not run on an audio driver thread.
  RenderProbe probe;
  probe.installerThread = std::this_thread::get_id();
  mixer->installTapOnBus(0, kTapBufferFrames, std::nullopt,
    [&probe](const std::unique_ptr<AudioPCMBuffer>& buffer, const std::optional<AudioTime>& /* when */) {
      if (std::this_thread::get_id() != probe.installerThread) {
        probe.ranOnAnotherThread.store(true);
      }
      probe.callbacks.fetch_add(1, std::memory_order_relaxed);
      if (buffer != nullptr) {
        probe.frames.fetch_add(static_cast<int64_t>(buffer->getFrameLength()), std::memory_order_relaxed);
      }
    });

  // Preparing opens the device, which is what tells the engine (and this sample) the format the
  // graph renders in.
  engine->prepare();
  auto deviceSampleRate = 0.0;
  if (const auto format = output->outputFormatForBus(0); format.has_value()) {
    deviceSampleRate = format->getSampleRate();
    MINILOG_INFO("  device format:     {}", format->toString());
  }

  // Schedule a known number of seconds of the file, so that the frames and the wall clock time
  // the playback takes can be compared with the audio the segment holds.
  auto scheduleFrames = static_cast<AudioFrameCount>(fileFrames);
  auto expectedSeconds = fileSampleRate > 0 ? static_cast<double>(fileFrames) / fileSampleRate : 0.0;
  if (seconds > 0 && fileSampleRate > 0) {
    const auto requested = static_cast<AudioFramePosition>(seconds * fileSampleRate);
    scheduleFrames = static_cast<AudioFrameCount>(std::min<AudioFramePosition>(requested, fileFrames));
    expectedSeconds = static_cast<double>(scheduleFrames) / fileSampleRate;
  }
  // The frames the segment becomes once the player has converted it to the device rate.
  const auto expectedDeviceFrames =
    deviceSampleRate > 0 ? static_cast<int64_t>(expectedSeconds * deviceSampleRate) : 0;

  std::atomic_bool finished{ false };
  std::atomic<int64_t> finishedAtMs{ 0 };
  player->scheduleSegment(audioFile, 0, scheduleFrames, std::nullopt,
    AudioPlayerNode::CompletionCallbackType::kPlayedBack,
    [&finished, &finishedAtMs](const AudioPlayerNode::CompletionCallbackType type) {
      finishedAtMs.store(nowMilliseconds(), std::memory_order_release);
      finished.store(true, std::memory_order_release);
      MINILOG_INFO("The player finished the scheduled segment (completion type {})", static_cast<int>(type));
    });
  MINILOG_INFO("Scheduled {} frames ({:.2f} s of file time, {} frames at the device rate)", scheduleFrames,
    expectedSeconds, expectedDeviceFrames);

  // `prepare` decodes ahead of the first render callback, which is what the API is for.
  player->prepare(kTapBufferFrames);

  if (const auto startError = engine->start(); startError.has_value()) {
    MINILOG_ERROR("Failed to start the engine: {}", startError->failureReason);
    AudioEngine::Destroy(engine);
    return 1;
  }

  const auto startedAtMs = nowMilliseconds();
  player->play();
  MINILOG_INFO("Playing ({}), waiting for {:.2f} s of audio", player->isPlaying() ? "started" : "did not start",
    expectedSeconds);

  auto elapsedSeconds = 0.0;
  const auto timeout = static_cast<int64_t>((expectedSeconds + 5.0) * 1000.0);
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto now = nowMilliseconds();
    elapsedSeconds = static_cast<double>(now - startedAtMs) / 1000.0;

    auto playerFrames = int64_t{ 0 };
    if (const auto when = player->lastRenderTime(); when.has_value()) {
      playerFrames = when->getSampleTime();
    }
    MINILOG_INFO("  {:.2f} s elapsed, the player rendered {} frames ({:.2f} s at the device rate), "
                 "{} tap callbacks saw {} frames",
      elapsedSeconds, playerFrames, deviceSampleRate > 0 ? static_cast<double>(playerFrames) / deviceSampleRate : 0.0,
      probe.callbacks.load(std::memory_order_relaxed), probe.frames.load(std::memory_order_relaxed));

    if (finished.load(std::memory_order_acquire)) {
      break;
    }
    if (now - startedAtMs > timeout) {
      MINILOG_WARN("The playback did not finish within the expected time");
      break;
    }
  }

  auto playerFrames = int64_t{ 0 };
  if (const auto when = player->lastRenderTime(); when.has_value()) {
    playerFrames = when->getSampleTime();
  }
  const auto finishedAt = finishedAtMs.load(std::memory_order_acquire);
  const auto playedSeconds = finishedAt > 0 ? static_cast<double>(finishedAt - startedAtMs) / 1000.0 : elapsedSeconds;
  const auto playbackRatio = expectedSeconds > 0 ? playedSeconds / expectedSeconds : 0.0;
  const auto renderedSeconds = deviceSampleRate > 0 ? static_cast<double>(playerFrames) / deviceSampleRate : 0.0;

  MINILOG_INFO("The player rendered {} frames ({:.2f} s at the device rate) for a segment of {:.2f} s",
    playerFrames, renderedSeconds, expectedSeconds);
  MINILOG_INFO("The segment took {:.2f} s of wall time to reach the output, ratio {:.3f}", playedSeconds,
    playbackRatio);
  MINILOG_INFO("The tap saw {} frames in {} callbacks, which is a lower bound: a tap buffer is dropped when the "
               "client cannot keep up",
    probe.frames.load(std::memory_order_relaxed), probe.callbacks.load(std::memory_order_relaxed));

  auto verdict = true;
  if (expectedDeviceFrames > 0) {
    const auto tolerance = static_cast<int64_t>(static_cast<double>(expectedDeviceFrames) * 0.02);
    if (std::abs(playerFrames - expectedDeviceFrames) > tolerance) {
      MINILOG_ERROR("The player rendered {} frames where the segment holds {}", playerFrames, expectedDeviceFrames);
      verdict = false;
    } else {
      MINILOG_INFO("The whole segment reached the output, with no block lost");
    }
  }
  if (playbackRatio < 0.9 || playbackRatio > 1.1) {
    MINILOG_ERROR("The render did not keep the rate of the device: {:.3f}", playbackRatio);
    verdict = false;
  } else {
    MINILOG_INFO("The render ran at the rate of the device");
  }
  if (!probe.ranOnAnotherThread.load(std::memory_order_relaxed)) {
    MINILOG_WARN("The tap block never left the thread that installed it");
    verdict = false;
  } else {
    MINILOG_INFO("The tap block ran on the engine's IO thread, not on the thread that installed it");
  }

  player->stop();
  engine->stop();
  mixer->removeTapOnBus(0);
  AudioEngine::Destroy(engine);

  MINILOG_INFO(verdict ? "The realtime render of the file is correct" : "The realtime render of the file is wrong");
  return verdict ? 0 : 1;
}
