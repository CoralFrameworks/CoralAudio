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
//  tests_AudioEngine.cpp
//
//  Functional tests for the render graph of coral::AudioEngine, covering:
//    - the offline manual rendering mode: prepare, pull through renderOffline, the
//      render timeline, the tap blocks and the sample format conversion of the
//      output node
//    - the realtime manual rendering mode: the engine's own render thread pulls the
//      graph at the render format's rate and delivers the tap blocks
//    - the player node: scheduling a buffer or a file segment, its completion handlers,
//      pausing and resuming
//
//  Only the file playback test needs an asset, and it is skipped when the asset is not
//  part of the checkout.
// =============================================================

#include <coral/audio/AudioEngine.h>
#include <coral/audio/AudioFile.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioMixerNode.h>
#include <coral/audio/AudioNode.h>
#include <coral/audio/AudioPlayerNode.h>
#include <coral/audio/AudioSinkNode.h>
#include <coral/audio/AudioSourceNode.h>
#include <coral/audio/AudioUnitDelay.h>
#include <coral/audio/AudioUnitDistortion.h>
#include <coral/audio/AudioUnitEqualizer.h>
#include <coral/audio/AudioUnitReverb.h>
#include <coral/audio/AudioUnitTimePitch.h>
#include <coral/audio/AudioUnitVarispeed.h>
#include <core/time/SystemClock.h>

#include "private/graph/AudioEngineImpl.h"
#include "private/graph/AudioNodeImpl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace coral;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint64_t kSampleRate = 44100;
constexpr uint32_t kChannels = 2;
constexpr double kFrequency = 440.0;
constexpr float kAmplitude = 0.5f;

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

/**
 * Renders a sine tone, so that the graph has a source that actually produces samples.
 */
class ToneNodeImpl final : public AudioNodeImpl {
public:
  ToneNodeImpl(const double sampleRate, const double frequency) noexcept
    : mSampleRate(sampleRate),
      mFrequency(frequency) {
  }

protected:
  void renderBlock(const AudioBufferList* const* /* inputs */,
    uint32_t /* numInputs */,
    AudioBufferList* outputData,
    const AudioFrameCount numFrames,
    const AudioTime& /* when */) override {
    if (outputData == nullptr) {
      return;
    }
    const auto channelCount = outputData->numberOfBuffers();
    for (size_t c = 0; c < channelCount; ++c) {
      const auto* buffer = outputData->getBuffer(c);
      if (buffer == nullptr || buffer->mData == nullptr) {
        return;
      }
      auto* samples = static_cast<float*>(buffer->mData);
      for (AudioFrameCount f = 0; f < numFrames; ++f) {
        const auto phase = 2.0 * kPi * mFrequency * static_cast<double>(mPosition + f) / mSampleRate;
        samples[f] = kAmplitude * static_cast<float>(std::sin(phase));
      }
    }
    mPosition += numFrames;
  }

private:
  double mSampleRate;
  double mFrequency;
  AudioFramePosition mPosition = 0;
};

/**
 * A test source node, which renders a tone on its single output bus.
 */
class ToneNode final : public AudioNode {
public:
  ToneNode(const double sampleRate, const double frequency)
    : AudioNode("ToneNode", std::make_unique<ToneNodeImpl>(sampleRate, frequency)) {
    declareOutputBus("out");
  }
};

AudioFormat makeFloatFormat() {
  return AudioFormat::InitStandardFormatWithSampleRate(kSampleRate, kChannels).value();
}

AudioFormat makeInt16Format() {
  return AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, kSampleRate, kChannels, true).value();
}

void testOfflineManualRendering() {
  std::printf("manual rendering, offline\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto output = engine->outputNode();
  check(output->outputFormatForBus(0) == format, "the output node renders in the manual rendering format");

  const auto source = std::make_shared<ToneNode>(static_cast<double>(kSampleRate), kFrequency);
  engine->attachNode(source);
  engine->connect(source, mixer, format);

  const auto mainThread = std::this_thread::get_id();
  std::atomic_int callbacks{ 0 };
  std::atomic_int framesTapped{ 0 };
  std::atomic_int foreignThreads{ 0 };
  mixer->installTapOnBus(0, 512, format,
    [&](const std::unique_ptr<AudioPCMBuffer>& buffer, const std::optional<AudioTime>& /* when */) {
      if (std::this_thread::get_id() != mainThread) {
        ++foreignThreads;
      }
      ++callbacks;
      if (buffer != nullptr) {
        framesTapped += static_cast<int>(buffer->getFrameLength());
      }
    });

  check(!engine->start().has_value(), "the engine starts without an audio device");

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  const auto status = engine->renderOffline(1024, buffer, &error);
  check(status == AudioEngine::ManualRenderingStatus::Success, "renderOffline reports success");
  check(buffer->getFrameLength() == 1024, "the whole block was rendered");
  check(engine->manualRenderingSampleTime() == 1024, "the render timeline advanced by the rendered frames");

  const auto channels = buffer->floatChannelData();
  check(channels != nullptr, "the rendered buffer is float");
  check(channels != nullptr && std::abs(channels[0][0]) < 1e-6f, "the first sample of the tone is silent");
  const auto expected = kAmplitude * static_cast<float>(std::sin(2.0 * kPi * kFrequency * 10.0 / kSampleRate));
  check(channels != nullptr && std::abs(channels[0][10] - expected) < 1e-4f, "the tone reached the render buffer");
  check(channels != nullptr && std::abs(channels[1][10] - expected) < 1e-4f, "the tone reached every channel");

  check(callbacks == 2, "the tap fired once per filled tap buffer");
  check(framesTapped == 1024, "the tap received every rendered frame");
  check(foreignThreads == 0, "the tap block ran on the thread that rendered");

  // Resetting clears the processing state of the nodes, not the graph itself.
  engine->reset();
  check(engine->inputConnectionPointForNode(mixer, 0).has_value(),
    "resetting the engine kept the graph connections");
  check(!engine->start().has_value(), "the engine starts again after a reset");
  const auto secondStatus = engine->renderOffline(1024, buffer, &error);
  check(secondStatus == AudioEngine::ManualRenderingStatus::Success, "the engine renders again after a reset");
  check(engine->manualRenderingSampleTime() == 1024, "the render timeline restarted at zero and advanced again");

  engine->stop();
  AudioEngine::Destroy(engine);
}

void testOfflineFormatConversion() {
  std::printf("manual rendering, offline, int16 render format\n");

  const auto format = makeInt16Format();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 512, &error),
    "the engine accepts an int16 render format");

  const auto mixer = engine->mainMixerNode();
  const auto source = std::make_shared<ToneNode>(static_cast<double>(kSampleRate), kFrequency);
  engine->attachNode(source);
  engine->connect(source, mixer, makeFloatFormat());
  check(!engine->start().has_value(), "the engine starts without an audio device");

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 512);
  const auto status = engine->renderOffline(512, buffer, &error);
  check(status == AudioEngine::ManualRenderingStatus::Success, "renderOffline reports success");

  const auto channels = buffer->int16ChannelData();
  check(channels != nullptr, "the rendered buffer is int16");
  const auto expected =
    static_cast<int>(std::lround(kAmplitude * std::sin(2.0 * kPi * kFrequency * 20.0 / kSampleRate) * 32768.0));
  check(channels != nullptr && std::abs(channels[0][20 * 2] - expected) <= 2, "the graph converted the tone to int16");

  engine->stop();
  AudioEngine::Destroy(engine);
}

void testRealtimeManualRendering() {
  std::printf("manual rendering, realtime\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Realtime, format, 512, &error),
    "the engine accepts the realtime manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto source = std::make_shared<ToneNode>(static_cast<double>(kSampleRate), kFrequency);
  engine->attachNode(source);
  engine->connect(source, mixer, format);

  const auto mainThread = std::this_thread::get_id();
  std::atomic_int callbacks{ 0 };
  std::atomic<int64_t> framesTapped{ 0 };
  std::mutex threadLock;
  std::thread::id tapThread;
  mixer->installTapOnBus(0, 512, format,
    [&](const std::unique_ptr<AudioPCMBuffer>& buffer, const std::optional<AudioTime>& /* when */) {
      {
        const std::lock_guard<std::mutex> guard(threadLock);
        if (tapThread == std::thread::id{}) {
          tapThread = std::this_thread::get_id();
        }
      }
      ++callbacks;
      if (buffer != nullptr) {
        framesTapped += static_cast<int64_t>(buffer->getFrameLength());
      }
    });

  check(!engine->start().has_value(), "the engine starts without an audio device");
  check(engine->isRunning(), "the engine reports that it is running");

  // The engine's own render thread pulls the graph, so the tap is served without the
  // client ever calling renderOffline.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  while (callbacks.load() < 4 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  check(callbacks.load() >= 4, "the render thread delivered tap buffers");

  const auto elapsedFrames = framesTapped.load();
  check(elapsedFrames >= 2048, "the render thread pulled the graph");
  // Pacing: a render loop that ignored the sample clock would produce far more than two
  // seconds of audio within the 400 ms the test waits for.
  check(elapsedFrames < static_cast<int64_t>(kSampleRate) * 2, "the render loop is paced to the sample clock");
  {
    const std::lock_guard<std::mutex> guard(threadLock);
    check(tapThread != mainThread, "the tap block ran on the engine's render thread");
  }

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 512);
  check(engine->renderOffline(512, buffer, &error) == AudioEngine::ManualRenderingStatus::Error,
    "renderOffline is refused in the realtime manual rendering mode");
  check(error != nullptr, "the refused render reported an error");
  delete error;
  error = nullptr;

  engine->stop();
  const auto framesAtStop = framesTapped.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  check(framesTapped.load() == framesAtStop, "stopping the engine stopped the render thread");

  AudioEngine::Destroy(engine);
}

void testGraphMutation() {
  std::printf("graph connections\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 512, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto source = std::make_shared<ToneNode>(static_cast<double>(kSampleRate), kFrequency);
  engine->attachNode(source);
  engine->connect(source, mixer, format);

  check(engine->inputConnectionPointForNode(mixer, 0).has_value(), "the mixer reports its input connection");
  check(engine->outputConnectionPointsForNode(source, 0).size() == 1, "the source reports one output connection");

  engine->disconnectNodeInput(mixer, 0);
  check(!engine->inputConnectionPointForNode(mixer, 0).has_value(), "the input connection was removed");
  check(engine->outputConnectionPointsForNode(source, 0).empty(), "the output connection was removed");

  engine->connect(source, mixer, format);
  engine->detachNode(source);
  check(!engine->inputConnectionPointForNode(mixer, 0).has_value(), "detaching the source disconnected it");
  check(!engine->toString().empty(), "the engine describes the graph");

  AudioEngine::Destroy(engine);
}

/**
 * Fills a buffer with a constant value, so that the tests can tell the player's frames
 * apart from silence.
 */
std::unique_ptr<AudioPCMBuffer> makeConstantBuffer(const AudioFormat& format,
  const AudioFrameCount frames,
  const float value) {
  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, frames);
  if (buffer != nullptr) {
    auto* const* channels = buffer->floatChannelData();
    const auto channelCount = format.getChannelCount();
    const auto stride = buffer->getStride();
    for (uint32_t c = 0; c < channelCount; ++c) {
      for (AudioFrameCount f = 0; f < frames; ++f) {
        channels[c][static_cast<size_t>(f) * stride] = value;
      }
    }
    buffer->setFrameLength(frames);
  }
  return buffer;
}

void testPlayerNodeBufferPlayback() {
  std::printf("player node, scheduled buffer\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto player = std::make_shared<AudioPlayerNode>();
  engine->attachNode(player);
  engine->connect(player, mixer, format);

  // --- one pass of a constant buffer ---
  std::atomic_int renderedCompletions{ 0 };
  player->scheduleBuffer(makeConstantBuffer(format, 1024, 0.25f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferInterruptsAtLoop,
    AudioPlayerNode::CompletionCallbackType::kRendered,
    [&](const AudioPlayerNode::CompletionCallbackType type) {
      if (type == AudioPlayerNode::CompletionCallbackType::kRendered) {
        ++renderedCompletions;
      }
    });

  // Preparing before the engine does falls back to the source's own format, which the
  // engine's prepare then replaces with the graph's.
  player->prepare(1024);
  check(!engine->start().has_value(), "the engine starts with a player node attached");
  player->prepare(1024);
  check(!player->isPlaying(), "a prepared player does not report that it is playing");
  player->play();
  check(player->isPlaying(), "the player reports that it is playing");

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders a block while the player is playing");
  auto* const* channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.25f) < 1e-6f,
    "the scheduled buffer reached the render output");
  check(channels != nullptr && std::abs(channels[0][1023] - 0.25f) < 1e-6f,
    "the whole scheduled block was rendered");

  const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (renderedCompletions.load() == 0 && std::chrono::steady_clock::now() < completionDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  check(renderedCompletions.load() == 1, "the completion handler was called once the block was rendered");

  // The player is drained, so the graph hears silence from it.
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders another block");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0]) < 1e-6f, "a drained player renders silence");

  // --- pause keeps the buffered frames and the sample time ---
  player->stop();
  player->scheduleBuffer(makeConstantBuffer(format, 2048, 0.5f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferInterruptsAtLoop, AudioPlayerNode::CompletionCallbackType::kConsumed,
    nullptr);
  player->prepare(1024);
  player->play();
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the first half of the paused buffer");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.5f) < 1e-6f, "the second schedule reached the output");

  player->pause();
  check(!player->isPlaying(), "a paused player does not report that it is playing");
  check(!player->nodeTimeForPlayerTime(AudioTime(0, format.getSampleRate(), 0)).has_value(),
    "a paused player has no player time mapping");
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders while the player is paused");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0]) < 1e-6f, "a paused player renders silence");

  player->play();
  check(player->isPlaying(), "the player resumes");
  check(player->nodeTimeForPlayerTime(AudioTime(0, format.getSampleRate(), 0)).has_value(),
    "a playing player maps player time");
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders after the resume");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.5f) < 1e-6f, "the buffered frames survived the pause");

  player->stop();
  check(!player->isPlaying(), "a stopped player does not report that it is playing");

  // --- a scheduled start time is honoured with silence before it ---
  player->scheduleBuffer(makeConstantBuffer(format, 1024, 0.75f), AudioTime(1024, format.getSampleRate(), 0),
    AudioPlayerNode::BufferOptions::kBufferInterruptsAtLoop, AudioPlayerNode::CompletionCallbackType::kRendered,
    nullptr);
  player->prepare(2048);
  player->play();
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the scheduled delay");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0]) < 1e-6f, "a scheduled start time renders silence until it is due");
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the scheduled content");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.75f) < 1e-6f, "the content starts at its scheduled time");

  // --- a looping buffer keeps playing ---
  player->stop();
  player->scheduleBuffer(makeConstantBuffer(format, 512, 1.0f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferLoops, AudioPlayerNode::CompletionCallbackType::kRendered, nullptr);
  player->prepare(512);
  player->play();
  auto looped = true;
  for (auto block = 0; block < 3; ++block) {
    if (engine->renderOffline(1024, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
      looped = false;
      break;
    }
    channels = buffer->floatChannelData();
    if (channels == nullptr || std::abs(channels[0][0] - 1.0f) > 1e-6f ||
        std::abs(channels[0][1023] - 1.0f) > 1e-6f) {
      looped = false;
    }
  }
  check(looped, "a looping buffer keeps producing audio");
  player->stop();

  // --- a start time that is still ahead (playAtTime) ---
  player->scheduleBuffer(makeConstantBuffer(format, 1024, 0.25f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferInterruptsAtLoop, AudioPlayerNode::CompletionCallbackType::kRendered,
    nullptr);
  player->prepare(1024);
  player->playAtTime(SystemClock::CurrentTimeMicros() + 50000);
  check(!player->isPlaying(), "a player scheduled to start later is not playing yet");

  const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!player->isPlaying() && std::chrono::steady_clock::now() < startDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  check(player->isPlaying(), "the player starts when the time it was given arrives");
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders after the delayed start");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.25f) < 1e-6f,
    "the buffer that was scheduled ahead of time plays");
  player->stop();

  AudioEngine::Destroy(engine);
}

/**
 * Stopping a player must leave it in the state it starts in: no scheduled events, sample time
 * 0, and the same behaviour as a freshly created node when new content is scheduled.
 */
void testPlayerNodeStopReset() {
  std::printf("player node, stop resets to the initial state\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");
  const auto mixer = engine->mainMixerNode();
  const auto player = std::make_shared<AudioPlayerNode>();
  engine->attachNode(player);
  engine->connect(player, mixer, format);

  std::atomic_int completions{ 0 };
  player->scheduleBuffer(makeConstantBuffer(format, 4096, 0.5f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferInterruptsAtLoop, AudioPlayerNode::CompletionCallbackType::kRendered,
    [&](const AudioPlayerNode::CompletionCallbackType) { ++completions; });
  player->prepare(1024);
  check(!engine->start().has_value(), "the engine starts with the player attached");
  player->prepare(1024);
  player->play();

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (completions.load() == 0 && std::chrono::steady_clock::now() < completionDeadline) {
    engine->renderOffline(1024, buffer, &error);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  check(completions.load() == 1, "the scheduled buffer completed");
  check(!player->isPlaying(), "a completed player does not report that it is playing");
  check(player->lastRenderTime().has_value(), "the player reported a render time while it played");

  // --- stop returns the node to its initial state ---
  player->stop();
  check(!player->isPlaying(), "a stopped player does not report that it is playing");
  check(!player->nodeTimeForPlayerTime(AudioTime(0, format.getSampleRate(), 0)).has_value(),
    "a stopped player has no player-time mapping");
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders after the stop");
  check(!player->lastRenderTime().has_value(), "the stop reset the node's sample time");
  auto* const* channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0]) < 1e-6f, "a stopped player renders silence");

  // Playing without scheduling anything must not revive the cleared schedule.
  player->play();
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders after the stop was followed by a play");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0]) < 1e-6f,
    "the schedule that was cleared by the stop does not play again");
  player->stop();

  // New content starts at sample time 0, not where the stopped playback ended.
  completions.store(0);
  player->scheduleBuffer(makeConstantBuffer(format, 2048, 0.25f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferInterruptsAtLoop, AudioPlayerNode::CompletionCallbackType::kRendered,
    [&](const AudioPlayerNode::CompletionCallbackType) { ++completions; });
  player->prepare(1024);
  player->play();
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the content scheduled after the stop");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.25f) < 1e-6f,
    "the content scheduled after the stop plays");
  const auto renderTime = player->lastRenderTime();
  check(renderTime.has_value() && renderTime->getSampleTime() == 1024,
    "the new schedule starts at sample time 0");
  player->stop();

  // --- a stop reports the schedule it clears once, on an already reset player, and content
  //     that the handler schedules is kept ---
  std::atomic_int stoppedCompletions{ 0 };
  std::atomic_int replacementCompletions{ 0 };
  player->scheduleBuffer(makeConstantBuffer(format, 8192, 0.4f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferInterruptsAtLoop, AudioPlayerNode::CompletionCallbackType::kRendered,
    [&](const AudioPlayerNode::CompletionCallbackType) {
      ++stoppedCompletions;
      check(!player->isPlaying(), "the completion handler of a stop sees a stopped player");
      player->scheduleBuffer(makeConstantBuffer(format, 2048, 0.3f), std::nullopt,
        AudioPlayerNode::BufferOptions::kBufferInterruptsAtLoop,
        AudioPlayerNode::CompletionCallbackType::kRendered,
        [&](const AudioPlayerNode::CompletionCallbackType) { ++replacementCompletions; });
      player->prepare(1024);
      player->play();
    });
  player->prepare(1024);
  player->play();
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the schedule that will be stopped");
  player->stop();
  check(stoppedCompletions.load() == 1, "stopping a playing schedule reports its completion once");
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the content scheduled from the stop's handler");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.3f) < 1e-6f,
    "content scheduled from the stop's completion handler plays");
  player->stop();

  engine->stop();
  AudioEngine::Destroy(engine);
}

void testPlayerNodeFilePlayback() {
  std::printf("player node, scheduled file segment\n");

  const auto path = std::string(CORAL_ASSETS_DIR) + "/music/music.wav";
  std::error_code fileError;
  if (!std::filesystem::exists(path, fileError)) {
    std::printf("  skip (no asset at %s)\n", path.c_str());
    return;
  }

  Error* error = nullptr;
  const auto file = AudioFile::InitForReading(path, &error);
  check(file != nullptr, "the test asset was opened for reading");
  if (file == nullptr) {
    return;
  }

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto player = std::make_shared<AudioPlayerNode>();
  engine->attachNode(player);
  engine->connect(player, mixer, format);
  check(!engine->start().has_value(), "the engine starts with the player node attached");

  constexpr AudioFrameCount kSegmentFrames = 4096;
  std::atomic_int playedBack{ 0 };
  player->scheduleSegment(file, 1000, kSegmentFrames, std::nullopt,
    AudioPlayerNode::CompletionCallbackType::kPlayedBack,
    [&](const AudioPlayerNode::CompletionCallbackType type) {
      if (type == AudioPlayerNode::CompletionCallbackType::kPlayedBack) {
        ++playedBack;
      }
    });

  player->prepare(kSegmentFrames);
  player->play();

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  auto peak = 0.0f;
  auto renders = 0;
  for (auto block = 0; block < 8; ++block) {
    if (engine->renderOffline(1024, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
      break;
    }
    ++renders;
    auto* const* channels = buffer->floatChannelData();
    if (channels == nullptr) {
      break;
    }
    for (AudioFrameCount f = 0; f < buffer->getFrameLength(); ++f) {
      peak = std::max(peak, std::abs(channels[0][f]));
    }
  }
  check(renders == 8, "the engine rendered the file segment");
  check(peak > 0.0f, "the scheduled file segment produced audio");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (playedBack.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  check(playedBack.load() == 1, "the played back completion handler was called");

  player->stop();
  AudioEngine::Destroy(engine);
}

/** Fills every buffer of a float buffer list with a constant. */
void fillFloatBufferList(const std::unique_ptr<AudioBufferList>& list, const float value) {
  for (size_t b = 0; b < list->numberOfBuffers(); ++b) {
    auto* buffer = list->getBuffer(b);
    if (buffer == nullptr || buffer->mData == nullptr) {
      continue;
    }
    auto* data = static_cast<float*>(buffer->mData);
    std::fill_n(data, buffer->mDataByteSize / sizeof(float), value);
  }
}

/** Fills every buffer of a 16 bit buffer list with a constant. */
void fillInt16BufferList(const std::unique_ptr<AudioBufferList>& list, const float value) {
  const auto sample = static_cast<int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
  for (size_t b = 0; b < list->numberOfBuffers(); ++b) {
    auto* buffer = list->getBuffer(b);
    if (buffer == nullptr || buffer->mData == nullptr) {
      continue;
    }
    auto* data = static_cast<int16_t*>(buffer->mData);
    std::fill_n(data, buffer->mDataByteSize / sizeof(int16_t), sample);
  }
}

/** Returns the magnitude of one frequency in a block, used for the harmonic checks. */
double goertzelMagnitude(const float* samples, const size_t count, const double frequency,
  const double sampleRate) {
  const auto omega = 2.0 * M_PI * frequency / sampleRate;
  const auto coefficient = 2.0 * std::cos(omega);
  auto s1 = 0.0;
  auto s2 = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const auto s0 = static_cast<double>(samples[i]) + coefficient * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const auto real = s1 - s2 * std::cos(omega);
  const auto imag = s2 * std::sin(omega);
  return std::sqrt(real * real + imag * imag) / static_cast<double>(count);
}

void testSourceNodeRendering() {
  std::printf("source node\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto callerThread = std::this_thread::get_id();
  std::atomic_int calls{ 0 };
  std::atomic<AudioFrameCount> requestedFrames{ 0 };
  std::atomic_int foreignThreads{ 0 };
  auto owned = std::make_shared<std::vector<float>>(2048, 0.25f);

  const auto source = AudioSourceNode::InitWithCallback(
    [&](bool* isSilence, const std::optional<AudioTime>& /* timestamp */, const AudioFrameCount frameCount,
      std::unique_ptr<AudioBufferList>& outputData) -> Result {
      if (std::this_thread::get_id() != callerThread) {
        ++foreignThreads;
      }
      ++calls;
      requestedFrames.store(frameCount);
      *isSilence = false;

      const auto mode = calls.load();
      if (mode == 2) {
        // The node has to supply buffers whose byte size matches the frame count.
        return Result::OK;
      }
      if (mode == 3) {
        // A block may replace the buffers with memory it owns for the duration of the call.
        for (size_t b = 0; b < outputData->numberOfBuffers(); ++b) {
          auto* buffer = outputData->getBuffer(b);
          buffer->mData = owned->data();
          buffer->mDataByteSize = frameCount * static_cast<uint32_t>(sizeof(float));
        }
        return Result::OK;
      }
      if (mode == 4) {
        // A block that reports silence is not asked to fill anything.
        *isSilence = true;
        return Result::OK;
      }
      fillFloatBufferList(outputData, 0.5f);
      return Result::OK;
    });

  check(source != nullptr, "the source node was created with a render block");
  engine->attachNode(source);
  engine->connect(source, mixer, format);
  check(!engine->start().has_value(), "the engine starts with a source node attached");

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the source node");
  auto* const* channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.5f) < 1e-6f, "the render block filled the output");
  check(channels != nullptr && std::abs(channels[0][1023] - 0.5f) < 1e-6f, "the whole block came from the block");
  check(requestedFrames.load() == 1024, "the block was asked for the frames the graph needed");
  check(foreignThreads.load() == 0, "manual rendering calls the block on the thread that renders");

  // A block that does not touch the supplied buffers leaves silence behind.
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the second block");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0]) < 1e-6f, "an untouched buffer renders silence");

  // A block may replace the buffers, and whatever it points at is what reaches the graph.
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the third block");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.25f) < 1e-6f, "a replaced buffer reached the graph");

  // The silence flag is honoured.
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the fourth block");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0]) < 1e-6f, "the silence flag renders silence");

  engine->stop();
  AudioEngine::Destroy(engine);

  // --- a block format that differs from the graph is converted ---
  const auto convertedFormat = makeFloatFormat();
  const auto blockFormat = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, 8000, 2, true).value();
  auto convertedEngine = AudioEngine::Create();
  check(convertedEngine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, convertedFormat, 1024,
          &error),
    "the engine accepts the offline manual rendering mode for the converted source");

  const auto convertedMixer = convertedEngine->mainMixerNode();
  const auto convertedSource = AudioSourceNode::InitWithCallback(blockFormat,
    [](bool* isSilence, const std::optional<AudioTime>& /* timestamp */, const AudioFrameCount /* frameCount */,
      std::unique_ptr<AudioBufferList>& outputData) -> Result {
      *isSilence = false;
      fillInt16BufferList(outputData, 0.5f);
      return Result::OK;
    });
  convertedEngine->attachNode(convertedSource);
  convertedEngine->connect(convertedSource, convertedMixer, convertedFormat);
  check(!convertedEngine->start().has_value(), "the engine starts with the converted source attached");

  auto converted = AudioPCMBuffer::InitWithPCMFormat(convertedFormat, 1024);
  auto sawConverted = false;
  for (auto block = 0; block < 8 && !sawConverted; ++block) {
    if (convertedEngine->renderOffline(1024, converted, &error) != AudioEngine::ManualRenderingStatus::Success) {
      break;
    }
    auto* const* convertedChannels = converted->floatChannelData();
    if (convertedChannels != nullptr && std::abs(convertedChannels[0][1023] - 0.5f) < 0.02f) {
      sawConverted = true;
    }
  }
  check(sawConverted, "a 16 bit block at another sample rate was converted to the graph format");

  convertedEngine->stop();
  AudioEngine::Destroy(convertedEngine);
}

/**
 * Plays an asset whose rate differs from the rate of the graph, which is the case of every
 * 44100 file on a 48000 device: the player converts, and the whole segment has to reach the
 * output. This is what catches a producer that reserves room for the block it reads instead of
 * the block the conversion produces.
 */
void testPlayerNodeResampledFilePlayback() {
  std::printf("player node, a file at another sample rate\n");

  const auto path = std::string(CORAL_ASSETS_DIR) + "/music/music.wav";
  std::error_code fileError;
  if (!std::filesystem::exists(path, fileError)) {
    std::printf("  skip (no asset at %s)\n", path.c_str());
    return;
  }

  Error* error = nullptr;
  const auto file = AudioFile::InitForReading(path, &error);
  check(file != nullptr, "the test asset was opened for reading");
  if (file == nullptr) {
    return;
  }
  const auto fileFormat = file->getProcessingFormat();
  if (!fileFormat.has_value() || fileFormat->getSampleRate() != 44100.0) {
    std::printf("  skip (the asset is not a 44100 Hz file)\n");
    return;
  }

  // The graph renders at a rate well above the file's, so the block a conversion produces is
  // several times longer than the block that was read: a producer that reserves room for the
  // block it reads loses the difference. The realtime manual rendering mode pulls the graph at
  // the rate of a device, which is what the producer of the player follows.
  const auto format = AudioFormat::InitStandardFormatWithSampleRate(192000, 2).value();
  auto engine = AudioEngine::Create();
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Realtime, format, 1024, &error),
    "the engine accepts the realtime manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto player = std::make_shared<AudioPlayerNode>();
  engine->attachNode(player);
  engine->connect(player, mixer, fileFormat);

  std::atomic<int64_t> tappedFrames{ 0 };
  mixer->installTapOnBus(0, 1024, std::nullopt,
    [&tappedFrames](const std::unique_ptr<AudioPCMBuffer>& buffer, const std::optional<AudioTime>& /* when */) {
      if (buffer != nullptr) {
        tappedFrames.fetch_add(static_cast<int64_t>(buffer->getFrameLength()), std::memory_order_relaxed);
      }
    });

  check(!engine->start().has_value(), "the engine starts with the resampled player attached");

  // One second of the file, which is 48000 frames once converted to 48000.
  constexpr double kSegmentSeconds = 1.0;
  const auto segmentFrames = static_cast<AudioFrameCount>(kSegmentSeconds * fileFormat->getSampleRate());
  const auto expectedFrames = static_cast<int64_t>(kSegmentSeconds * format.getSampleRate());

  std::atomic_int playedBack{ 0 };
  std::atomic<int64_t> renderedWhenFinished{ 0 };
  player->scheduleSegment(file, 0, segmentFrames, std::nullopt, AudioPlayerNode::CompletionCallbackType::kPlayedBack,
    [&](const AudioPlayerNode::CompletionCallbackType type) {
      if (type == AudioPlayerNode::CompletionCallbackType::kPlayedBack) {
        if (const auto when = player->lastRenderTime(); when.has_value()) {
          renderedWhenFinished.store(when->getSampleTime(), std::memory_order_release);
        }
        ++playedBack;
      }
    });
  player->prepare(2048);
  player->play();

  // The engine's render thread pulls the graph, so the test only has to wait for realtime.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
  while (playedBack.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  check(playedBack.load(std::memory_order_acquire) > 0, "the resampled segment was reported as played back");
  const auto finishedAt = renderedWhenFinished.load(std::memory_order_acquire);
  std::printf("  info expected %lld frames at the graph rate, the player rendered %lld\n",
    static_cast<long long>(expectedFrames), static_cast<long long>(finishedAt));
  check(std::abs(finishedAt - expectedFrames) <= 64,
    "the whole segment reached the output at the rate of the graph");
  check(tappedFrames.load(std::memory_order_acquire) > 0, "the tapped graph rendered the resampled audio");

  player->stop();
  engine->stop();
  AudioEngine::Destroy(engine);
}

void testDelayUnit() {
  std::printf("delay unit\n");

  // A low sample rate keeps the delay test short: one second of delay is 8000 frames.
  const auto format = AudioFormat::InitStandardFormatWithSampleRate(8000, 2).value();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto source = AudioSourceNode::InitWithCallback(
    [](bool* isSilence, const std::optional<AudioTime>& /* timestamp */, const AudioFrameCount /* frameCount */,
      std::unique_ptr<AudioBufferList>& outputData) -> Result {
      *isSilence = false;
      fillFloatBufferList(outputData, 0.5f);
      return Result::OK;
    });
  const auto delay = std::make_shared<AudioUnitDelay>();
  engine->attachNode(source);
  engine->attachNode(delay);
  engine->connect(source, delay, format);
  engine->connect(delay, mixer, format);
  check(!engine->start().has_value(), "the engine starts with a delay unit in the graph");

  // Fully wet with no feedback: the output stays silent until the delayed signal arrives.
  delay->setDelayTime(1);
  delay->setFeedback(0.0f);
  delay->setWetDryMix(100.0f);

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  auto beforeDelay = 0.0f;
  auto afterDelay = 0.0f;
  for (auto block = 0; block < 9; ++block) {
    if (engine->renderOffline(1024, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
      break;
    }
    auto* const* channels = buffer->floatChannelData();
    if (channels == nullptr) {
      break;
    }
    if (block == 7) {
      // Frame 7168 is still inside the one second (8000 frame) delay.
      beforeDelay = channels[0][0];
    }
    if (block == 8) {
      // Frame 8192 is past it.
      afterDelay = channels[0][0];
    }
  }
  check(std::abs(beforeDelay) < 1e-6f, "a fully wet delay is silent before its delay time");
  check(std::abs(afterDelay - 0.5f) < 1e-6f, "the delayed signal arrives after its delay time");

  // All dry: the input passes through untouched.
  delay->setWetDryMix(0.0f);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the dry delay");
  auto* const* dryChannels = buffer->floatChannelData();
  check(dryChannels != nullptr && std::abs(dryChannels[0][0] - 0.5f) < 1e-6f, "an all dry delay passes the input");

  // Bypassed: the input passes through as well, whatever the wet/dry mix says.
  delay->setWetDryMix(100.0f);
  delay->setBypass(true);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the bypassed delay");
  auto* const* bypassedChannels = buffer->floatChannelData();
  check(bypassedChannels != nullptr && std::abs(bypassedChannels[0][0] - 0.5f) < 1e-6f,
    "a bypassed delay passes the input");
  delay->setBypass(false);

  check(delay->getDelayTime() == 1, "the delay time property reports what was set");
  check(std::abs(delay->getWetDryMix() - 100.0f) < 1e-6f, "the wet/dry property reports what was set");

  engine->stop();
  AudioEngine::Destroy(engine);
}

void testDistortionUnit() {
  std::printf("distortion unit\n");

  const auto format = makeFloatFormat();
  const auto sampleRate = format.getSampleRate();
  constexpr auto kToneHz = 1000.0;
  constexpr auto kToneAmplitude = 0.5f;
  constexpr AudioFrameCount kBlockFrames = 1024;

  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  // The same zero-based sine block every callback keeps the comparison between blocks simple.
  const auto source = AudioSourceNode::InitWithCallback(
    [sampleRate, kToneHz, kToneAmplitude](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
      const AudioFrameCount frameCount, std::unique_ptr<AudioBufferList>& outputData) -> Result {
      *isSilence = false;
      for (size_t b = 0; b < outputData->numberOfBuffers(); ++b) {
        auto* buffer = outputData->getBuffer(b);
        if (buffer == nullptr || buffer->mData == nullptr) {
          continue;
        }
        auto* data = static_cast<float*>(buffer->mData);
        for (AudioFrameCount f = 0; f < frameCount; ++f) {
          data[f] = kToneAmplitude * static_cast<float>(
            std::sin(2.0 * M_PI * kToneHz * static_cast<double>(f) / sampleRate));
        }
      }
      return Result::OK;
    });
  const auto distortion = std::make_shared<AudioUnitDistortion>();
  engine->attachNode(source);
  engine->attachNode(distortion);
  engine->connect(source, distortion, format);
  engine->connect(distortion, mixer, format);

  // Start all dry so that the first block is the untouched input.
  distortion->loadFactoryPreset(AudioUnitDistortion::DistortionPreset::kDrumsBitBrush);
  distortion->setPreGain(-6.0f);
  distortion->setWetDryMix(0.0f);
  check(!engine->start().has_value(), "the engine starts with a distortion unit in the graph");

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, kBlockFrames);
  auto renderBlock = [&] {
    return engine->renderOffline(kBlockFrames, buffer, &error) ==
           AudioEngine::ManualRenderingStatus::Success;
  };

  std::vector<float> expected(kBlockFrames);
  for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
    expected[f] = kToneAmplitude * static_cast<float>(
      std::sin(2.0 * M_PI * kToneHz * static_cast<double>(f) / sampleRate));
  }
  auto maxDifference = [](const float* first, const float* second, const size_t count) {
    auto difference = 0.0f;
    for (size_t i = 0; i < count; ++i) {
      difference = std::max(difference, std::abs(first[i] - second[i]));
    }
    return difference;
  };

  // --- the properties cover the AVFoundation ranges ---
  check(std::abs(distortion->getPreGain() - -6.0f) < 1e-6f, "the default pre gain is -6 dB");
  check(std::abs(distortion->getWetDryMix() - 0.0f) < 1e-6f, "the wet/dry property reports what was set");
  distortion->setPreGain(100.0f);
  check(std::abs(distortion->getPreGain() - 20.0f) < 1e-6f, "the pre gain is clamped to 20 dB");
  distortion->setPreGain(-100.0f);
  check(std::abs(distortion->getPreGain() - -80.0f) < 1e-6f, "the pre gain is clamped to -80 dB");
  distortion->setPreGain(std::numeric_limits<float>::quiet_NaN());
  check(std::abs(distortion->getPreGain() - -6.0f) < 1e-6f, "a NaN pre gain falls back to the default");
  distortion->setWetDryMix(150.0f);
  check(std::abs(distortion->getWetDryMix() - 100.0f) < 1e-6f, "the wet/dry mix is clamped to 100%");
  distortion->setWetDryMix(-10.0f);
  check(std::abs(distortion->getWetDryMix()) < 1e-6f, "the wet/dry mix is clamped to 0%");
  distortion->setWetDryMix(std::numeric_limits<float>::quiet_NaN());
  check(std::abs(distortion->getWetDryMix() - 50.0f) < 1e-6f, "a NaN wet/dry mix falls back to the default");
  distortion->setPreGain(-6.0f);
  distortion->setWetDryMix(0.0f);

  // --- all dry passes the input through ---
  check(renderBlock(), "the engine renders the dry distortion");
  auto* const* channels = buffer->floatChannelData();
  check(channels != nullptr && maxDifference(channels[0], expected.data(), kBlockFrames) < 1e-6f,
    "an all dry distortion passes the input untouched");

  // --- a bypassed unit passes the input through whatever the mix says ---
  distortion->setWetDryMix(100.0f);
  distortion->setBypass(true);
  check(renderBlock(), "the engine renders the bypassed distortion");
  channels = buffer->floatChannelData();
  check(channels != nullptr && maxDifference(channels[0], expected.data(), kBlockFrames) < 1e-6f,
    "a bypassed distortion passes the input untouched");
  distortion->setBypass(false);

  // --- fully wet, the shaper adds harmonics ---
  distortion->setPreGain(6.0f);
  distortion->loadFactoryPreset(AudioUnitDistortion::DistortionPreset::kMultiDistortedFunk);
  for (auto block = 0; block < 3; ++block) {
    if (!renderBlock()) {
      break;
    }
  }
  channels = buffer->floatChannelData();
  check(channels != nullptr, "the engine renders the wet distortion");
  if (channels != nullptr) {
    const auto fundamental = goertzelMagnitude(channels[0], kBlockFrames, kToneHz, sampleRate);
    const auto thirdHarmonic = goertzelMagnitude(channels[0], kBlockFrames, kToneHz * 3.0, sampleRate);
    std::printf("  info distortion fundamental %.4f, third harmonic %.4f (%.1f%%)\n", fundamental,
      thirdHarmonic, fundamental > 0.0 ? 100.0 * thirdHarmonic / fundamental : 0.0);
    check(fundamental > 0.05, "the fully wet distortion is audible");
    check(thirdHarmonic > fundamental * 0.02, "the distortion adds a third harmonic");
    check(maxDifference(channels[0], expected.data(), kBlockFrames) > 0.01f,
      "the distorted signal is not the dry signal");
    auto finite = true;
    auto bounded = true;
    for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
      finite = finite && std::isfinite(channels[0][f]);
      bounded = bounded && std::abs(channels[0][f]) <= 1.0001f;
    }
    check(finite, "the distortion output is finite");
    check(bounded, "the distortion output stays in the valid range");
  }

  // --- every factory preset renders a finite, bounded, audible block ---
  const AudioUnitDistortion::DistortionPreset presets[] = {
    AudioUnitDistortion::DistortionPreset::kDrumsBitBrush,
    AudioUnitDistortion::DistortionPreset::kDrumsBufferBeats,
    AudioUnitDistortion::DistortionPreset::kDrumsLoFi,
    AudioUnitDistortion::DistortionPreset::kMultiBrokenSpeaker,
    AudioUnitDistortion::DistortionPreset::kMultiCellphoneConcert,
    AudioUnitDistortion::DistortionPreset::kMultiDecimated1,
    AudioUnitDistortion::DistortionPreset::kMultiDecimated2,
    AudioUnitDistortion::DistortionPreset::kMultiDecimated3,
    AudioUnitDistortion::DistortionPreset::kMultiDecimated4,
    AudioUnitDistortion::DistortionPreset::kMultiDistortedFunk,
    AudioUnitDistortion::DistortionPreset::kMultiDistortedCubed,
    AudioUnitDistortion::DistortionPreset::kMultiDistortedSquared,
    AudioUnitDistortion::DistortionPreset::kMultiEcho1,
    AudioUnitDistortion::DistortionPreset::kMultiEcho2,
    AudioUnitDistortion::DistortionPreset::kMultiEchoTight1,
    AudioUnitDistortion::DistortionPreset::kMultiEchoTight2,
    AudioUnitDistortion::DistortionPreset::kMultiEverythingIsBroken,
    AudioUnitDistortion::DistortionPreset::kSpeechAlienChatter,
    AudioUnitDistortion::DistortionPreset::kSpeechCosmicInterference,
    AudioUnitDistortion::DistortionPreset::kSpeechGoldenPi,
    AudioUnitDistortion::DistortionPreset::kSpeechRadioTower,
    AudioUnitDistortion::DistortionPreset::kSpeechWaves,
  };
  check(static_cast<uint32_t>(AudioUnitDistortion::DistortionPreset::kSpeechWaves) == 21,
    "the preset raw values match AVFoundation");

  std::vector<std::vector<float>> presetBlocks(std::size(presets), std::vector<float>(kBlockFrames));
  auto presetOk = true;
  for (size_t i = 0; i < std::size(presets); ++i) {
    distortion->loadFactoryPreset(presets[i]);
    distortion->setPreGain(-6.0f);
    distortion->setWetDryMix(100.0f);
    // The first block after a preset change clears the previous tail; the second one is the
    // settled output the check looks at.
    if (!renderBlock() || !renderBlock()) {
      presetOk = false;
      break;
    }
    channels = buffer->floatChannelData();
    if (channels == nullptr) {
      presetOk = false;
      break;
    }
    auto peak = 0.0f;
    for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
      if (!std::isfinite(channels[0][f]) || std::abs(channels[0][f]) > 1.0001f) {
        presetOk = false;
      }
      peak = std::max(peak, std::abs(channels[0][f]));
    }
    if (peak < 1e-4f) {
      presetOk = false;
    }
    std::copy_n(channels[0], kBlockFrames, presetBlocks[i].begin());
  }
  check(presetOk, "every distortion preset renders a finite, bounded, audible block");
  check(maxDifference(presetBlocks[0].data(), presetBlocks[8].data(), kBlockFrames) > 1e-3f,
    "different presets produce different audio");
  check(maxDifference(presetBlocks[12].data(), presetBlocks[14].data(), kBlockFrames) > 1e-3f,
    "the echo presets differ in their echo time");

  engine->stop();
  AudioEngine::Destroy(engine);
}

void testEqualizerUnit() {
  std::printf("equalizer unit\n");

  const auto format = makeFloatFormat();
  const auto sampleRate = format.getSampleRate();
  constexpr auto kLowHz = 200.0;
  constexpr auto kHighHz = 3000.0;
  constexpr auto kAmplitude = 0.2f;
  constexpr AudioFrameCount kBlockFrames = 1024;

  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  // The band tests look at the response at these two frequencies, so the source carries both.
  const auto source = AudioSourceNode::InitWithCallback(
    [sampleRate, kLowHz, kHighHz, kAmplitude](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
      const AudioFrameCount frameCount, std::unique_ptr<AudioBufferList>& outputData) -> Result {
      *isSilence = false;
      for (size_t b = 0; b < outputData->numberOfBuffers(); ++b) {
        auto* buffer = outputData->getBuffer(b);
        if (buffer == nullptr || buffer->mData == nullptr) {
          continue;
        }
        auto* data = static_cast<float*>(buffer->mData);
        for (AudioFrameCount f = 0; f < frameCount; ++f) {
          const auto time = static_cast<double>(f) / sampleRate;
          data[f] = kAmplitude * static_cast<float>(std::sin(2.0 * M_PI * kLowHz * time) +
                                                    std::sin(2.0 * M_PI * kHighHz * time));
        }
      }
      return Result::OK;
    });
  const auto equalizer = AudioUnitEqualizer::Create(2);
  engine->attachNode(source);
  engine->attachNode(equalizer);
  engine->connect(source, equalizer, format);
  engine->connect(equalizer, mixer, format);

  // --- the defaults match AVFoundation ---
  check(equalizer->getNumberOfBands() == 2, "the equalizer reports its number of bands");
  const auto defaults = equalizer->getBands();
  check(defaults.size() == 2, "the equalizer has one parameter object per band");
  check(defaults[0].bypass, "a band is bypassed by default");
  check(defaults[0].filterType == AudioUnitEqualizer::FilterType::kFilterTypeParametric,
    "a band is parametric by default");
  check(std::abs(defaults[0].frequency - 1000.0f) < 1e-3f, "a band starts at 1000 Hz");
  check(std::abs(defaults[0].bandwidth - 1.0f) < 1e-6f, "a band starts with one octave of bandwidth");
  check(std::abs(defaults[0].gain) < 1e-6f, "a band starts at 0 dB");
  check(std::abs(equalizer->getGlobalGain()) < 1e-6f, "the global gain starts at 0 dB");

  AudioUnitEqualizer::FilterParameters band;
  band.filterType = AudioUnitEqualizer::FilterType::kFilterTypeParametric;
  band.frequency = static_cast<float>(kLowHz);
  band.bandwidth = 1.0f;
  band.gain = 12.0f;
  band.bypass = false;
  equalizer->setBandParameters(0, band);
  check(!engine->start().has_value(), "the engine starts with an equalizer in the graph");

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, kBlockFrames);
  auto renderBlocks = [&](const int count) {
    for (auto i = 0; i < count; ++i) {
      if (engine->renderOffline(kBlockFrames, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
        return false;
      }
    }
    return true;
  };

  std::vector<float> expected(kBlockFrames);
  for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
    const auto time = static_cast<double>(f) / sampleRate;
    expected[f] = kAmplitude * static_cast<float>(std::sin(2.0 * M_PI * kLowHz * time) +
                                                  std::sin(2.0 * M_PI * kHighHz * time));
  }
  const auto inputLow = goertzelMagnitude(expected.data(), kBlockFrames, kLowHz, sampleRate);
  const auto inputHigh = goertzelMagnitude(expected.data(), kBlockFrames, kHighHz, sampleRate);
  auto gainAtLow = [&](const float* samples) {
    return inputLow > 0.0 ? goertzelMagnitude(samples, kBlockFrames, kLowHz, sampleRate) / inputLow : 0.0;
  };
  auto gainAtHigh = [&](const float* samples) {
    return inputHigh > 0.0 ? goertzelMagnitude(samples, kBlockFrames, kHighHz, sampleRate) / inputHigh : 0.0;
  };

  // --- a parametric band boosts its frequency and leaves the far one alone ---
  check(renderBlocks(4), "the engine renders the parametric equalizer");
  auto* const* channels = buffer->floatChannelData();
  check(channels != nullptr, "the engine rendered the parametric equalizer output");
  if (channels != nullptr) {
    const auto boostLow = gainAtLow(channels[0]);
    const auto boostHigh = gainAtHigh(channels[0]);
    std::printf("  info parametric 200 Hz +12 dB: 200 Hz gain %.2f, 3 kHz gain %.2f\n", boostLow, boostHigh);
    check(boostLow > 2.5 && boostLow < 6.0, "the parametric band boosts the frequency it is tuned to");
    check(boostHigh > 0.85 && boostHigh < 1.15, "the parametric band leaves a far frequency alone");
  }

  // --- a low pass band cuts above its cutoff and keeps the content below it ---
  band.filterType = AudioUnitEqualizer::FilterType::kFilterTypeLowPass;
  band.frequency = 500.0f;
  band.gain = 0.0f;
  equalizer->setBandParameters(0, band);
  check(renderBlocks(4), "the engine renders the low pass equalizer");
  channels = buffer->floatChannelData();
  if (channels != nullptr) {
    const auto low = gainAtLow(channels[0]);
    const auto high = gainAtHigh(channels[0]);
    std::printf("  info low pass 500 Hz: 200 Hz gain %.2f, 3 kHz gain %.2f\n", low, high);
    check(low > 0.7, "the low pass band keeps the content below its cutoff");
    check(high < 0.5, "the low pass band cuts the content above its cutoff");
  }

  // --- a high pass band cuts below its cutoff and keeps the content above it ---
  band.filterType = AudioUnitEqualizer::FilterType::kFilterTypeHighPass;
  band.frequency = 1000.0f;
  equalizer->setBandParameters(0, band);
  check(renderBlocks(4), "the engine renders the high pass equalizer");
  channels = buffer->floatChannelData();
  if (channels != nullptr) {
    const auto low = gainAtLow(channels[0]);
    const auto high = gainAtHigh(channels[0]);
    std::printf("  info high pass 1 kHz: 200 Hz gain %.2f, 3 kHz gain %.2f\n", low, high);
    check(low < 0.5, "the high pass band cuts the content below its cutoff");
    check(high > 0.7, "the high pass band keeps the content above its cutoff");
  }

  // --- the global gain scales the output ---
  band.filterType = AudioUnitEqualizer::FilterType::kFilterTypeParametric;
  band.frequency = static_cast<float>(kLowHz);
  band.gain = 0.0f;
  equalizer->setBandParameters(0, band);
  equalizer->setGlobalGain(-6.0f);
  check(renderBlocks(4), "the engine renders the equalizer global gain");
  channels = buffer->floatChannelData();
  if (channels != nullptr) {
    const auto low = gainAtLow(channels[0]);
    check(low > 0.4 && low < 0.65, "the global gain scales the output");
  }
  equalizer->setGlobalGain(0.0f);

  // --- a bypassed band does not filter, and a bypassed unit passes the input through ---
  band.bypass = true;
  equalizer->setBandParameters(0, band);
  check(renderBlocks(4), "the engine renders the bypassed band");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(gainAtLow(channels[0]) - 1.0) < 0.05 &&
          std::abs(gainAtHigh(channels[0]) - 1.0) < 0.05,
    "a bypassed band passes the signal through");
  equalizer->setBypass(true);
  check(renderBlocks(1), "the engine renders the bypassed equalizer");
  channels = buffer->floatChannelData();
  auto maxDifference = [](const float* first, const float* second, const size_t count) {
    auto difference = 0.0f;
    for (size_t i = 0; i < count; ++i) {
      difference = std::max(difference, std::abs(first[i] - second[i]));
    }
    return difference;
  };
  check(channels != nullptr && maxDifference(channels[0], expected.data(), kBlockFrames) < 1e-6f,
    "a bypassed equalizer passes the input untouched");
  equalizer->setBypass(false);

  // --- every filter type is stable and audible ---
  auto typesOk = true;
  for (uint32_t type = 0; type <= 10; ++type) {
    AudioUnitEqualizer::FilterParameters parameters;
    parameters.filterType = static_cast<AudioUnitEqualizer::FilterType>(type);
    parameters.frequency = static_cast<float>(kLowHz);
    parameters.bandwidth = 1.0f;
    parameters.gain = 6.0f;
    parameters.bypass = false;
    equalizer->setBandParameters(0, parameters);
    if (!renderBlocks(3)) {
      typesOk = false;
      break;
    }
    channels = buffer->floatChannelData();
    if (channels == nullptr) {
      typesOk = false;
      break;
    }
    auto peak = 0.0f;
    for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
      if (!std::isfinite(channels[0][f]) || std::abs(channels[0][f]) > 100.0f) {
        typesOk = false;
      }
      peak = std::max(peak, std::abs(channels[0][f]));
    }
    if (peak < 1e-4f) {
      typesOk = false;
    }
  }
  check(typesOk, "every equalizer filter type renders finite, audible audio");

  // --- the band properties are clamped and round-trip through setBands ---
  AudioUnitEqualizer::FilterParameters clamped;
  clamped.frequency = 5.0f;
  clamped.bandwidth = 10.0f;
  clamped.gain = 100.0f;
  clamped.filterType = static_cast<AudioUnitEqualizer::FilterType>(99);
  clamped.bypass = false;
  equalizer->setBandParameters(0, clamped);
  const auto sanitized = equalizer->getBands();
  check(std::abs(sanitized[0].frequency - 20.0f) < 1e-3f, "the band frequency is clamped to 20 Hz");
  check(std::abs(sanitized[0].bandwidth - 5.0f) < 1e-3f, "the band bandwidth is clamped to 5 octaves");
  check(std::abs(sanitized[0].gain - 24.0f) < 1e-3f, "the band gain is clamped to 24 dB");
  check(sanitized[0].filterType == AudioUnitEqualizer::FilterType::kFilterTypeParametric,
    "an unknown filter type falls back to parametric");

  std::vector<AudioUnitEqualizer::FilterParameters> replacement(2);
  replacement[0].filterType = AudioUnitEqualizer::FilterType::kFilterTypeHighShelf;
  replacement[0].frequency = 4000.0f;
  replacement[0].gain = 6.0f;
  replacement[0].bypass = false;
  replacement[1].filterType = AudioUnitEqualizer::FilterType::kFilterTypeLowShelf;
  replacement[1].frequency = 120.0f;
  replacement[1].gain = -6.0f;
  replacement[1].bypass = false;
  equalizer->setBands(replacement);
  const auto roundTrip = equalizer->getBands();
  check(roundTrip.size() == 2, "setBands keeps the number of bands");
  check(roundTrip[0].filterType == AudioUnitEqualizer::FilterType::kFilterTypeHighShelf &&
          std::abs(roundTrip[0].frequency - 4000.0f) < 1e-3f &&
          std::abs(roundTrip[0].gain - 6.0f) < 1e-6f && !roundTrip[0].bypass,
    "the first band round-trips through setBands");
  check(roundTrip[1].filterType == AudioUnitEqualizer::FilterType::kFilterTypeLowShelf &&
          std::abs(roundTrip[1].frequency - 120.0f) < 1e-3f &&
          std::abs(roundTrip[1].gain + 6.0f) < 1e-6f && !roundTrip[1].bypass,
    "the second band round-trips through setBands");

  engine->stop();
  AudioEngine::Destroy(engine);
}

void testReverbUnit() {
  std::printf("reverb unit\n");

  const auto format = makeFloatFormat();
  const auto sampleRate = format.getSampleRate();
  constexpr auto kToneHz = 200.0;
  constexpr auto kToneAmplitude = 0.25f;
  constexpr AudioFrameCount kBlockFrames = 1024;

  // --- the tail of an impulse, with the default preset ---
  {
    auto engine = AudioEngine::Create();
    Error* error = nullptr;
    check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
      "the engine accepts the offline manual rendering mode");

    const auto mixer = engine->mainMixerNode();
    std::atomic_int calls{ 0 };
    const auto source = AudioSourceNode::InitWithCallback(
      [&calls](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
        const AudioFrameCount frameCount, std::unique_ptr<AudioBufferList>& outputData) -> Result {
        *isSilence = false;
        const auto call = calls.fetch_add(1, std::memory_order_relaxed);
        for (size_t b = 0; b < outputData->numberOfBuffers(); ++b) {
          auto* buffer = outputData->getBuffer(b);
          if (buffer == nullptr || buffer->mData == nullptr) {
            continue;
          }
          auto* data = static_cast<float*>(buffer->mData);
          std::fill_n(data, frameCount, 0.0f);
          if (call == 0) {
            data[0] = 0.5f;
          }
        }
        return Result::OK;
      });
    const auto reverb = std::make_shared<AudioUnitReverb>();
    engine->attachNode(source);
    engine->attachNode(reverb);
    engine->connect(source, reverb, format);
    engine->connect(reverb, mixer, format);

    check(std::abs(reverb->getWetDryMix() - 100.0f) < 1e-6f, "the default wet/dry mix is 100%");
    check(!engine->start().has_value(), "the engine starts with a reverb in the graph");

    auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, kBlockFrames);
    std::vector<float> peaks;
    for (auto block = 0; block < 12; ++block) {
      if (engine->renderOffline(kBlockFrames, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
        peaks.push_back(0.0f);
        continue;
      }
      auto* const* channels = buffer->floatChannelData();
      auto peak = 0.0f;
      for (AudioFrameCount f = 0; channels != nullptr && f < kBlockFrames; ++f) {
        peak = std::max(peak, std::abs(channels[0][f]));
      }
      peaks.push_back(peak);
    }
    std::printf("  info reverb tail peaks:");
    for (const auto peak: peaks) {
      std::printf(" %.5f", peak);
    }
    std::printf("\n");

    const auto early = std::max(std::max(peaks[2], peaks[3]), std::max(peaks[4], peaks[5]));
    const auto late = peaks[11];
    check(early > 1e-4f, "the impulse produced a reverb tail");
    check(late > 0.0f && late < early, "the reverb tail decays");

    engine->stop();
    AudioEngine::Destroy(engine);
  }

  // --- the properties, and how they blend with a continuous signal ---
  {
    auto engine = AudioEngine::Create();
    Error* error = nullptr;
    check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
      "the engine accepts the offline manual rendering mode");

    const auto mixer = engine->mainMixerNode();
    const auto source = AudioSourceNode::InitWithCallback(
      [sampleRate, kToneHz, kToneAmplitude](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
        const AudioFrameCount frameCount, std::unique_ptr<AudioBufferList>& outputData) -> Result {
        *isSilence = false;
        for (size_t b = 0; b < outputData->numberOfBuffers(); ++b) {
          auto* buffer = outputData->getBuffer(b);
          if (buffer == nullptr || buffer->mData == nullptr) {
            continue;
          }
          auto* data = static_cast<float*>(buffer->mData);
          for (AudioFrameCount f = 0; f < frameCount; ++f) {
            data[f] = kToneAmplitude * static_cast<float>(
              std::sin(2.0 * M_PI * kToneHz * static_cast<double>(f) / sampleRate));
          }
        }
        return Result::OK;
      });
    const auto reverb = std::make_shared<AudioUnitReverb>();
    engine->attachNode(source);
    engine->attachNode(reverb);
    engine->connect(source, reverb, format);
    engine->connect(reverb, mixer, format);

    // Starting all dry makes the first block the untouched input.
    reverb->setWetDryMix(0.0f);
    check(!engine->start().has_value(), "the engine starts the dry reverb");

    auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, kBlockFrames);
    auto renderBlock = [&] {
      return engine->renderOffline(kBlockFrames, buffer, &error) ==
             AudioEngine::ManualRenderingStatus::Success;
    };
    std::vector<float> expected(kBlockFrames);
    for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
      expected[f] = kToneAmplitude * static_cast<float>(
        std::sin(2.0 * M_PI * kToneHz * static_cast<double>(f) / sampleRate));
    }
    auto maxDifference = [](const float* first, const float* second, const size_t count) {
      auto difference = 0.0f;
      for (size_t i = 0; i < count; ++i) {
        difference = std::max(difference, std::abs(first[i] - second[i]));
      }
      return difference;
    };

    check(renderBlock(), "the engine renders the dry reverb");
    auto* const* channels = buffer->floatChannelData();
    check(channels != nullptr && maxDifference(channels[0], expected.data(), kBlockFrames) < 1e-6f,
      "an all dry reverb passes the input untouched");

    reverb->setWetDryMix(100.0f);
    reverb->setBypass(true);
    check(renderBlock(), "the engine renders the bypassed reverb");
    channels = buffer->floatChannelData();
    check(channels != nullptr && maxDifference(channels[0], expected.data(), kBlockFrames) < 1e-6f,
      "a bypassed reverb passes the input untouched");
    reverb->setBypass(false);

    check(renderBlock() && renderBlock() && renderBlock() && renderBlock(),
      "the engine renders the wet reverb");
    channels = buffer->floatChannelData();
    check(channels != nullptr, "the engine rendered the wet reverb output");
    if (channels != nullptr) {
      auto peak = 0.0f;
      auto finite = true;
      auto bounded = true;
      for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
        peak = std::max(peak, std::abs(channels[0][f]));
        finite = finite && std::isfinite(channels[0][f]);
        bounded = bounded && std::abs(channels[0][f]) <= 1.0001f;
      }
      std::printf("  info reverb wet peak %.4f\n", peak);
      check(peak > 1e-4f, "the wet reverb is audible");
      check(finite, "the reverb output is finite");
      check(bounded, "the reverb output stays in the valid range");
    }

    // --- every factory preset renders a finite, bounded, audible block ---
    const AudioUnitReverb::ReverbPreset presets[] = {
      AudioUnitReverb::ReverbPreset::kSmallRoom,
      AudioUnitReverb::ReverbPreset::kMediumRoom,
      AudioUnitReverb::ReverbPreset::kLargeRoom,
      AudioUnitReverb::ReverbPreset::kMediumHall,
      AudioUnitReverb::ReverbPreset::kLargeHall,
      AudioUnitReverb::ReverbPreset::kPlate,
      AudioUnitReverb::ReverbPreset::kMediumChamber,
      AudioUnitReverb::ReverbPreset::kLargeChamber,
      AudioUnitReverb::ReverbPreset::kCathedral,
      AudioUnitReverb::ReverbPreset::kLargeRoom2,
      AudioUnitReverb::ReverbPreset::kMediumHall2,
      AudioUnitReverb::ReverbPreset::kMediumHall3,
      AudioUnitReverb::ReverbPreset::kLargeHall2,
    };
    check(static_cast<uint32_t>(AudioUnitReverb::ReverbPreset::kLargeHall2) == 12,
      "the reverb preset raw values match AVFoundation");

    std::vector<std::vector<float>> presetBlocks(std::size(presets), std::vector<float>(kBlockFrames));
    auto presetsOk = true;
    for (size_t i = 0; i < std::size(presets); ++i) {
      reverb->loadFactoryPreset(presets[i]);
      reverb->setWetDryMix(100.0f);
      // The pre-delay and the comb delays of the larger presets are longer than one block,
      // so four blocks are rendered before the settled tail is measured.
      auto rendered = true;
      for (auto block = 0; block < 4; ++block) {
        rendered = rendered && renderBlock();
      }
      if (!rendered) {
        presetsOk = false;
        break;
      }
      channels = buffer->floatChannelData();
      if (channels == nullptr) {
        presetsOk = false;
        break;
      }
      auto peak = 0.0f;
      for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
        if (!std::isfinite(channels[0][f]) || std::abs(channels[0][f]) > 1.0001f) {
          presetsOk = false;
        }
        peak = std::max(peak, std::abs(channels[0][f]));
      }
      if (peak < 1e-4f) {
        presetsOk = false;
      }
      std::copy_n(channels[0], kBlockFrames, presetBlocks[i].begin());
    }
    check(presetsOk, "every reverb preset renders a finite, bounded, audible block");
    check(maxDifference(presetBlocks[0].data(), presetBlocks[8].data(), kBlockFrames) > 1e-3f,
      "different reverb presets produce different tails");

    // --- the property is a percentage and clamps ---
    reverb->setWetDryMix(150.0f);
    check(std::abs(reverb->getWetDryMix() - 100.0f) < 1e-6f, "the wet/dry mix is clamped to 100%");
    reverb->setWetDryMix(-10.0f);
    check(std::abs(reverb->getWetDryMix()) < 1e-6f, "the wet/dry mix is clamped to 0%");
    reverb->setWetDryMix(std::numeric_limits<float>::quiet_NaN());
    check(std::abs(reverb->getWetDryMix() - 100.0f) < 1e-6f, "a NaN wet/dry mix falls back to 100%");
    reverb->setWetDryMix(100.0f);

    engine->stop();
    AudioEngine::Destroy(engine);
  }
}

void testTimePitchUnit() {
  std::printf("time pitch unit\n");

  const auto format = makeFloatFormat();
  const auto sampleRate = format.getSampleRate();
  constexpr auto kToneHz = 1000.0;
  constexpr auto kToneAmplitude = 0.5f;
  constexpr AudioFrameCount kBlockFrames = 1024;

  auto zeroCrossingHz = [sampleRate](const float* samples) {
    auto crossings = 0;
    for (AudioFrameCount f = 1; f < kBlockFrames; ++f) {
      if ((samples[f - 1] < 0.0f) != (samples[f] < 0.0f)) {
        ++crossings;
      }
    }
    return crossings * sampleRate / (2.0 * kBlockFrames);
  };
  auto blockRms = [](const float* samples) {
    double sum = 0.0;
    for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
      sum += static_cast<double>(samples[f]) * samples[f];
    }
    return std::sqrt(sum / kBlockFrames);
  };
  auto blockJump = [](const float* samples) {
    auto jump = 0.0f;
    for (AudioFrameCount f = 1; f < kBlockFrames; ++f) {
      jump = std::max(jump, std::abs(samples[f] - samples[f - 1]));
    }
    return jump;
  };
  auto maxDifference = [](const float* first, const float* second, const size_t count) {
    auto difference = 0.0f;
    for (size_t i = 0; i < count; ++i) {
      difference = std::max(difference, std::abs(first[i] - second[i]));
    }
    return difference;
  };

  // --- the parameters, and the frequency each one produces ---
  {
    auto engine = AudioEngine::Create();
    Error* error = nullptr;
    check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
      "the engine accepts the offline manual rendering mode");
    const auto mixer = engine->mainMixerNode();
    // The source must keep its phase across callbacks: a phase that restarts on every block is
    // heard by the vocoder as a stream of clicks, not as a tone.
    const auto phase = std::make_shared<double>(0.0);
    const auto source = AudioSourceNode::InitWithCallback(
      [phase, sampleRate](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
        const AudioFrameCount frameCount, std::unique_ptr<AudioBufferList>& outputData) -> Result {
        *isSilence = false;
        for (size_t b = 0; b < outputData->numberOfBuffers(); ++b) {
          auto* buffer = outputData->getBuffer(b);
          if (buffer == nullptr || buffer->mData == nullptr) {
            continue;
          }
          auto* data = static_cast<float*>(buffer->mData);
          for (AudioFrameCount f = 0; f < frameCount; ++f) {
            data[f] = kToneAmplitude * static_cast<float>(
              std::sin(2.0 * M_PI * kToneHz * static_cast<double>(f) / sampleRate + *phase));
          }
        }
        *phase += 2.0 * M_PI * kToneHz * frameCount / sampleRate;
        *phase = std::fmod(*phase, 2.0 * M_PI);
        return Result::OK;
      });
    const auto timePitch = std::make_shared<AudioUnitTimePitch>();
    engine->attachNode(source);
    engine->attachNode(timePitch);
    engine->connect(source, timePitch, format);
    engine->connect(timePitch, mixer, format);

    check(std::abs(timePitch->getPitch()) < 1e-6f, "the default pitch is 0 cents");
    check(std::abs(timePitch->getRate() - 1.0f) < 1e-6f, "the default rate is 1.0");
    check(std::abs(timePitch->getOverlap() - 8.0f) < 1e-6f, "the default overlap is 8");
    check(!timePitch->isBypass(), "the time pitch unit starts unbypassed");

    // A bypassed time effect passes the block through untouched.
    timePitch->setBypass(true);
    check(!engine->start().has_value(), "the engine starts with the time pitch unit");
    auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, kBlockFrames);
    auto render = [&] {
      return engine->renderOffline(kBlockFrames, buffer, &error) ==
             AudioEngine::ManualRenderingStatus::Success;
    };
    std::vector<float> expected(kBlockFrames);
    for (AudioFrameCount f = 0; f < kBlockFrames; ++f) {
      expected[f] = kToneAmplitude * static_cast<float>(
        std::sin(2.0 * M_PI * kToneHz * static_cast<double>(f) / sampleRate));
    }
    check(render(), "the engine renders the bypassed time pitch unit");
    auto* const* channels = buffer->floatChannelData();
    check(channels != nullptr && maxDifference(channels[0], expected.data(), kBlockFrames) < 1e-6f,
      "a bypassed time pitch unit passes the input untouched");

    timePitch->setBypass(false);
    for (auto block = 0; block < 10; ++block) {
      check(render(), "the engine renders the time pitch unit");
    }
    channels = buffer->floatChannelData();
    check(channels != nullptr, "the time pitch unit produced a block");
    if (channels != nullptr) {
      const auto frequency = zeroCrossingHz(channels[0]);
      const auto rms = blockRms(channels[0]);
      std::printf("  info rate 1 pitch 0: %.0f Hz, rms %.4f\n", frequency, rms);
      check(std::abs(frequency - kToneHz) < 80.0, "a rate and pitch of 1 keeps the input frequency");
      check(std::abs(rms - 0.3536) < 0.06, "a rate and pitch of 1 keeps the input level");
    }

    // --- pitch up an octave ---
    timePitch->setPitch(1200.0f);
    for (auto block = 0; block < 12; ++block) {
      check(render(), "the engine renders the pitch shifted unit");
    }
    channels = buffer->floatChannelData();
    if (channels != nullptr) {
      const auto frequency = zeroCrossingHz(channels[0]);
      const auto rms = blockRms(channels[0]);
      std::printf("  info pitch +1200: %.0f Hz, rms %.4f\n", frequency, rms);
      check(std::abs(frequency - 2.0 * kToneHz) < 100.0, "a pitch of +1200 shifts an octave up");
      check(rms > 0.25, "the pitch shifted output is audible");
    }

    // --- pitch down an octave ---
    timePitch->setPitch(-1200.0f);
    for (auto block = 0; block < 12; ++block) {
      check(render(), "the engine renders the pitch shifted unit");
    }
    channels = buffer->floatChannelData();
    if (channels != nullptr) {
      const auto frequency = zeroCrossingHz(channels[0]);
      const auto rms = blockRms(channels[0]);
      std::printf("  info pitch -1200: %.0f Hz, rms %.4f\n", frequency, rms);
      check(std::abs(frequency - kToneHz / 2.0) < 70.0, "a pitch of -1200 shifts an octave down");
      check(rms > 0.25, "the pitched down output is audible");
    }

    // --- the rate changes the duration, not the pitch ---
    timePitch->setPitch(0.0f);
    timePitch->setRate(2.0f);
    for (auto block = 0; block < 12; ++block) {
      check(render(), "the engine renders the rate changed unit");
    }
    channels = buffer->floatChannelData();
    if (channels != nullptr) {
      const auto frequency = zeroCrossingHz(channels[0]);
      const auto rms = blockRms(channels[0]);
      std::printf("  info rate 2: %.0f Hz, rms %.4f\n", frequency, rms);
      check(std::abs(frequency - kToneHz) < 80.0, "a rate of 2 keeps the input pitch");
      // The rate above 1.0 must keep the FIFO primed: alternating full and partial blocks are
      // the stutter the buffering has to avoid.
      auto stable = true;
      for (auto block = 0; block < 4; ++block) {
        check(render(), "the engine renders the rate changed unit");
        channels = buffer->floatChannelData();
        stable = stable && channels != nullptr && std::abs(blockRms(channels[0]) - 0.3536) < 0.05;
      }
      check(stable, "a rate of 2 renders every block at a stable level");
    }

    timePitch->setRate(0.5f);
    for (auto block = 0; block < 12; ++block) {
      check(render(), "the engine renders the rate changed unit");
    }
    channels = buffer->floatChannelData();
    if (channels != nullptr) {
      const auto frequency = zeroCrossingHz(channels[0]);
      const auto rms = blockRms(channels[0]);
      std::printf("  info rate 0.5: %.0f Hz, rms %.4f\n", frequency, rms);
      check(std::abs(frequency - kToneHz) < 80.0, "a rate below 1 keeps the input pitch");
      check(rms > 0.25, "the slowed output is audible");
    }

    // --- a live rate change is smoothed, not a hard splice ---
    timePitch->setRate(1.0f);
    timePitch->setPitch(0.0f);
    for (auto block = 0; block < 4; ++block) {
      check(render(), "the engine renders before the live rate change");
    }
    timePitch->setRate(2.0f);
    auto rateChangeOk = true;
    for (auto block = 0; block < 8; ++block) {
      check(render(), "the engine renders the live rate change");
      channels = buffer->floatChannelData();
      rateChangeOk = rateChangeOk && channels != nullptr && blockRms(channels[0]) > 0.12 &&
                     blockJump(channels[0]) < 0.4;
    }
    check(rateChangeOk, "a live rate change keeps rendering without a splice");
    channels = buffer->floatChannelData();
    check(channels != nullptr && std::abs(zeroCrossingHz(channels[0]) - kToneHz) < 80.0,
      "the smoothed rate reaches its target pitch");

    // --- a live pitch change is smoothed as well ---
    timePitch->setRate(1.0f);
    for (auto block = 0; block < 4; ++block) {
      check(render(), "the engine renders before the live pitch change");
    }
    timePitch->setPitch(1200.0f);
    auto pitchChangeOk = true;
    for (auto block = 0; block < 8; ++block) {
      check(render(), "the engine renders the live pitch change");
      channels = buffer->floatChannelData();
      pitchChangeOk = pitchChangeOk && channels != nullptr && blockRms(channels[0]) > 0.12 &&
                      blockJump(channels[0]) < 0.5;
    }
    check(pitchChangeOk, "a live pitch change keeps rendering without a splice");
    channels = buffer->floatChannelData();
    check(channels != nullptr && std::abs(zeroCrossingHz(channels[0]) - 2.0 * kToneHz) < 120.0,
      "the smoothed pitch reaches its target");
    timePitch->setPitch(0.0f);
    for (auto block = 0; block < 6; ++block) {
      check(render(), "the engine renders after the live pitch change");
    }

    // --- the overlap changes the analysis hop, and stays stable ---
    timePitch->setRate(1.0f);
    for (const auto overlap : { 4.0f, 32.0f }) {
      timePitch->setOverlap(overlap);
      auto silentBlocks = 0;
      auto clicks = false;
      auto audibleAfterwards = false;
      for (auto block = 0; block < 10; ++block) {
        check(render(), "the engine renders the overlap changed unit");
        channels = buffer->floatChannelData();
        if (channels == nullptr) {
          continue;
        }
        const auto rms = blockRms(channels[0]);
        if (rms < 0.02) {
          ++silentBlocks;
        } else if (blockJump(channels[0]) >= 0.5) {
          clicks = true;
        }
        if (block >= 7 && rms > 0.25) {
          audibleAfterwards = true;
        }
      }
      check(silentBlocks <= 2, "an overlap change resets briefly, not for the rest of the stream");
      check(!clicks, "the overlap change does not splice a click into the output");
      check(audibleAfterwards, "the overlap changed unit is audible again after the reset");
    }

    // --- the extreme parameter values stay finite ---
    timePitch->setOverlap(8.0f);
    timePitch->setRate(32.0f);
    timePitch->setPitch(2400.0f);
    for (auto block = 0; block < 6; ++block) {
      check(render(), "the engine renders the extreme time pitch settings");
    }
    channels = buffer->floatChannelData();
    auto finite = true;
    for (AudioFrameCount f = 0; channels != nullptr && f < kBlockFrames; ++f) {
      finite = finite && std::isfinite(channels[0][f]);
    }
    check(finite, "a rate of 32 and a pitch of +2400 stay finite");

    timePitch->setRate(1.0f / 32.0f);
    timePitch->setPitch(-2400.0f);
    for (auto block = 0; block < 6; ++block) {
      check(render(), "the engine renders the extreme time pitch settings");
    }
    channels = buffer->floatChannelData();
    finite = true;
    for (AudioFrameCount f = 0; channels != nullptr && f < kBlockFrames; ++f) {
      finite = finite && std::isfinite(channels[0][f]);
    }
    check(finite, "a rate of 1/32 and a pitch of -2400 stay finite");

    // --- the documented ranges ---
    timePitch->setPitch(10000.0f);
    check(std::abs(timePitch->getPitch() - 2400.0f) < 1e-6f, "the pitch is clamped to +2400 cents");
    timePitch->setPitch(-10000.0f);
    check(std::abs(timePitch->getPitch() + 2400.0f) < 1e-6f, "the pitch is clamped to -2400 cents");
    timePitch->setPitch(std::numeric_limits<float>::quiet_NaN());
    check(std::abs(timePitch->getPitch()) < 1e-6f, "a NaN pitch falls back to 0");
    timePitch->setRate(100.0f);
    check(std::abs(timePitch->getRate() - 32.0f) < 1e-6f, "the rate is clamped to 32");
    timePitch->setRate(0.0f);
    check(std::abs(timePitch->getRate() - 1.0f / 32.0f) < 1e-6f, "the rate is clamped to 1/32");
    timePitch->setRate(std::numeric_limits<float>::quiet_NaN());
    check(std::abs(timePitch->getRate() - 1.0f) < 1e-6f, "a NaN rate falls back to 1");
    timePitch->setOverlap(1.0f);
    check(std::abs(timePitch->getOverlap() - 3.0f) < 1e-6f, "the overlap is clamped to 3");
    timePitch->setOverlap(100.0f);
    check(std::abs(timePitch->getOverlap() - 32.0f) < 1e-6f, "the overlap is clamped to 32");
    timePitch->setOverlap(std::numeric_limits<float>::quiet_NaN());
    check(std::abs(timePitch->getOverlap() - 8.0f) < 1e-6f, "a NaN overlap falls back to 8");

    engine->stop();
    AudioEngine::Destroy(engine);
  }

  // --- a finite source: the rate must change the rendered duration ---
  auto countAudibleBlocks = [&](const float rate) {
    auto engine = AudioEngine::Create();
    Error* error = nullptr;
    engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error);
    const auto mixer = engine->mainMixerNode();
    const auto position = std::make_shared<AudioFramePosition>(0);
    const auto source = AudioSourceNode::InitWithCallback(
      [position, sampleRate](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
        const AudioFrameCount frameCount, std::unique_ptr<AudioBufferList>& outputData) -> Result {
        *isSilence = false;
        for (size_t b = 0; b < outputData->numberOfBuffers(); ++b) {
          auto* buffer = outputData->getBuffer(b);
          if (buffer == nullptr || buffer->mData == nullptr) {
            continue;
          }
          auto* data = static_cast<float*>(buffer->mData);
          for (AudioFrameCount f = 0; f < frameCount; ++f) {
            const auto frame = *position + static_cast<AudioFramePosition>(f);
            data[f] = frame < static_cast<AudioFramePosition>(sampleRate)
                        ? kToneAmplitude * static_cast<float>(
                            std::sin(2.0 * M_PI * kToneHz * static_cast<double>(frame) / sampleRate))
                        : 0.0f;
          }
        }
        *position += static_cast<AudioFramePosition>(frameCount);
        return Result::OK;
      });
    const auto timePitch = std::make_shared<AudioUnitTimePitch>();
    engine->attachNode(source);
    engine->attachNode(timePitch);
    engine->connect(source, timePitch, format);
    engine->connect(timePitch, mixer, format);
    timePitch->setRate(rate);
    engine->start();

    auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, kBlockFrames);
    auto blocks = 0;
    for (auto block = 0; block < 120; ++block) {
      if (engine->renderOffline(kBlockFrames, buffer, &error) !=
          AudioEngine::ManualRenderingStatus::Success) {
        break;
      }
      auto* const* channels = buffer->floatChannelData();
      if (channels != nullptr && blockRms(channels[0]) > 0.01) {
        ++blocks;
      }
    }
    engine->stop();
    AudioEngine::Destroy(engine);
    return blocks;
  };

  const auto atNormalRate = countAudibleBlocks(1.0f);
  const auto atDoubleRate = countAudibleBlocks(2.0f);
  const auto atHalfRate = countAudibleBlocks(0.5f);
  std::printf("  info audible blocks: rate 1 -> %d, rate 2 -> %d, rate 0.5 -> %d\n",
    atNormalRate, atDoubleRate, atHalfRate);
  check(atNormalRate > 30, "the one second source renders for about one second");
  check(atDoubleRate < atNormalRate * 0.75 && atDoubleRate > atNormalRate * 0.25,
    "a rate of 2 halves the rendered duration");
  check(atHalfRate > atNormalRate * 1.4, "a rate of 0.5 doubles the rendered duration");
}

void testVarispeedUnit() {
  std::printf("varispeed unit\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  // A tone is a friendlier input for the time domain processor than a constant.
  auto phase = std::make_shared<double>(0.0);
  const auto source = AudioSourceNode::InitWithCallback(
    [phase, sampleRate = format.getSampleRate()](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
      const AudioFrameCount frameCount, std::unique_ptr<AudioBufferList>& outputData) -> Result {
      *isSilence = false;
      auto* buffer = outputData->getBuffer(0);
      if (buffer == nullptr || buffer->mData == nullptr) {
        return Result::ErrorInvalidState;
      }
      auto* data = static_cast<float*>(buffer->mData);
      const auto stride = outputData->numberOfBuffers() > 1 ? 1 : buffer->mNumberChannels;
      for (AudioFrameCount f = 0; f < frameCount; ++f) {
        const auto value = 0.5 * std::sin(2.0 * kPi * 440.0 * (*phase + f) / sampleRate);
        for (uint32_t c = 0; c < buffer->mNumberChannels; ++c) {
          data[static_cast<size_t>(f) * stride + c] = value;
        }
      }
      *phase += frameCount;
      return Result::OK;
    });
  const auto varispeed = std::make_shared<AudioUnitVarispeed>();
  engine->attachNode(source);
  engine->attachNode(varispeed);
  engine->connect(source, varispeed, format);
  engine->connect(varispeed, mixer, format);
  check(!engine->start().has_value(), "the engine starts with a varispeed unit in the graph");

  check(std::abs(varispeed->getRate() - 1.0f) < 1e-6f, "the varispeed unit starts at a rate of 1.0");
  varispeed->setRate(0.1f);
  check(std::abs(varispeed->getRate() - 0.25f) < 1e-6f, "a rate below the range is clamped to 0.25");
  varispeed->setRate(10.0f);
  check(std::abs(varispeed->getRate() - 4.0f) < 1e-6f, "a rate above the range is clamped to 4.0");
  varispeed->setRate(1.0f);

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  auto peak = 0.0f;
  for (auto block = 0; block < 12; ++block) {
    if (engine->renderOffline(1024, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
      break;
    }
    auto* const* channels = buffer->floatChannelData();
    if (channels == nullptr) {
      break;
    }
    for (AudioFrameCount f = 0; f < buffer->getFrameLength(); ++f) {
      peak = std::max(peak, std::abs(channels[0][f]));
    }
  }
  check(peak > 0.1f, "the varispeed unit passed the resampled audio through");
  check(peak <= 0.6f, "the varispeed unit did not amplify the signal");

  // A rate change is applied without a re-prepare.
  varispeed->setRate(2.0f);
  auto sawAudio = false;
  for (auto block = 0; block < 12 && !sawAudio; ++block) {
    if (engine->renderOffline(1024, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
      break;
    }
    auto* const* channels = buffer->floatChannelData();
    for (AudioFrameCount f = 0; f < buffer->getFrameLength() && channels != nullptr; ++f) {
      if (std::abs(channels[0][f]) > 0.1f) {
        sawAudio = true;
        break;
      }
    }
  }
  check(sawAudio, "the varispeed unit keeps producing audio after a rate change");

  // Bypassing passes the input straight through.
  varispeed->setBypass(true);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders the bypassed varispeed unit");
  auto* const* bypassedChannels = buffer->floatChannelData();
  auto bypassPeak = 0.0f;
  if (bypassedChannels != nullptr) {
    for (AudioFrameCount f = 0; f < buffer->getFrameLength(); ++f) {
      bypassPeak = std::max(bypassPeak, std::abs(bypassedChannels[0][f]));
    }
  }
  check(bypassPeak > 0.1f, "a bypassed varispeed unit passes its input");
  varispeed->setBypass(false);

  engine->stop();
  AudioEngine::Destroy(engine);
}

/** The frequency of a tone, measured from its positive-going zero crossings. */
double measureToneFrequency(const std::vector<float>& samples, const double sampleRate) {
  if (samples.size() < 2) {
    return 0.0;
  }
  auto crossings = size_t{ 0 };
  for (size_t i = 1; i < samples.size(); ++i) {
    if (samples[i - 1] <= 0.0f && samples[i] > 0.0f) {
      ++crossings;
    }
  }
  return static_cast<double>(crossings) * sampleRate / static_cast<double>(samples.size());
}

/** The longest run of exactly silent frames. */
size_t longestSilentRun(const std::vector<float>& samples) {
  auto longest = size_t{ 0 };
  auto run = size_t{ 0 };
  for (const auto sample: samples) {
    if (sample == 0.0f) {
      longest = std::max(longest, ++run);
    } else {
      run = 0;
    }
  }
  return longest;
}

/**
 * A varispeed unit at a rate other than 1.0 consumes a different number of input frames than
 * the block it renders. The rate has to be followed by pulling that many frames from its input:
 * otherwise the graph asks for more frames than the unit produced and the difference is
 * silence, which is heard as the audio dropping out on every block.
 */
void testVarispeedRateContinuity() {
  std::printf("varispeed, the output is continuous at every rate\n");

  const auto format = makeFloatFormat();
  constexpr double kToneFrequency = 440.0;

  for (const auto speed: { 1.0f, 2.0f, 4.0f, 0.5f }) {
    auto engine = AudioEngine::Create();
    Error* error = nullptr;
    check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
      "the engine accepts the offline manual rendering mode for rate " + std::to_string(speed));

    const auto mixer = engine->mainMixerNode();
    auto phase = std::make_shared<double>(0.0);
    const auto source = AudioSourceNode::InitWithCallback(
      [phase, sampleRate = format.getSampleRate()](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
        const AudioFrameCount frameCount, std::unique_ptr<AudioBufferList>& outputData) -> Result {
        *isSilence = false;
        auto* buffer = outputData->getBuffer(0);
        if (buffer == nullptr || buffer->mData == nullptr) {
          return Result::ErrorInvalidState;
        }
        auto* data = static_cast<float*>(buffer->mData);
        const auto stride = outputData->numberOfBuffers() > 1 ? 1 : buffer->mNumberChannels;
        for (AudioFrameCount f = 0; f < frameCount; ++f) {
          const auto value = 0.5 * std::sin(2.0 * kPi * kToneFrequency * (*phase + f) / sampleRate);
          for (uint32_t c = 0; c < buffer->mNumberChannels; ++c) {
            data[static_cast<size_t>(f) * stride + c] = value;
          }
        }
        *phase += frameCount;
        return Result::OK;
      });
    const auto varispeed = std::make_shared<AudioUnitVarispeed>();
    varispeed->setRate(speed);
    engine->attachNode(source);
    engine->attachNode(varispeed);
    engine->connect(source, varispeed, format);
    engine->connect(varispeed, mixer, format);
    check(!engine->start().has_value(), "the engine starts with a rate of " + std::to_string(speed));

    auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
    // The sonic stream buffers a little input before it produces its first frames.
    auto rendered = true;
    for (auto block = 0; block < 4 && rendered; ++block) {
      rendered = engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success;
    }

    std::vector<float> collected;
    for (auto block = 0; block < 8 && rendered; ++block) {
      if (engine->renderOffline(1024, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
        rendered = false;
        break;
      }
      auto* const* channels = buffer->floatChannelData();
      if (channels == nullptr) {
        rendered = false;
        break;
      }
      collected.insert(collected.end(), channels[0], channels[0] + buffer->getFrameLength());
    }
    check(rendered, "the engine rendered every block at rate " + std::to_string(speed));

    const auto measured = measureToneFrequency(collected, format.getSampleRate());
    const auto expected = kToneFrequency * static_cast<double>(speed);
    std::printf("  info rate %.2f: %.1f Hz measured, %.1f Hz expected\n", speed, measured, expected);
    check(std::abs(measured - expected) <= expected * 0.05,
      "a rate of " + std::to_string(speed) + " renders the tone at the resampled frequency");
    check(longestSilentRun(collected) <= 8,
      "a rate of " + std::to_string(speed) + " leaves no silent gaps in the output");

    engine->stop();
    AudioEngine::Destroy(engine);
  }
}

/** The amplitude of one sine in a block, measured with a Hann windowed DFT. */
double windowedToneAmplitude(const std::vector<float>& samples, const double frequency, const double sampleRate) {
  if (samples.size() < 4) {
    return 0.0;
  }
  auto real = 0.0;
  auto imag = 0.0;
  auto windowSum = 0.0;
  const auto n = samples.size();
  for (size_t i = 0; i < n; ++i) {
    const auto window = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
    const auto angle = 2.0 * kPi * frequency * static_cast<double>(i) / sampleRate;
    real += samples[i] * window * std::cos(angle);
    imag += samples[i] * window * std::sin(angle);
    windowSum += window;
  }
  return 2.0 * std::sqrt(real * real + imag * imag) / windowSum;
}

/**
 * The reported graph: two players, one of them through a varispeed at a rate above 1.0, both
 * into the same mixer. Every branch is pulled with the frame count its own rate asks for, so the
 * fast branch must not turn into silence and must reach the mixer at its resampled pitch.
 */
void testVarispeedBranchesIntoSingleMixer() {
  std::printf("varispeed, a fast branch and a normal branch into one mixer\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");
  const auto mixer = engine->mainMixerNode();

  const auto makeToneSource = [&](const double frequency) {
    auto phase = std::make_shared<double>(0.0);
    return AudioSourceNode::InitWithCallback(
      [phase, frequency, sampleRate = format.getSampleRate()](bool* isSilence,
        const std::optional<AudioTime>& /* timestamp */, const AudioFrameCount frameCount,
        std::unique_ptr<AudioBufferList>& outputData) -> Result {
        *isSilence = false;
        auto* buffer = outputData->getBuffer(0);
        if (buffer == nullptr || buffer->mData == nullptr) {
          return Result::ErrorInvalidState;
        }
        auto* data = static_cast<float*>(buffer->mData);
        const auto stride = outputData->numberOfBuffers() > 1 ? 1 : buffer->mNumberChannels;
        for (AudioFrameCount f = 0; f < frameCount; ++f) {
          const auto value = 0.5 * std::sin(2.0 * kPi * frequency * (*phase + f) / sampleRate);
          for (uint32_t c = 0; c < buffer->mNumberChannels; ++c) {
            data[static_cast<size_t>(f) * stride + c] = value;
          }
        }
        *phase += frameCount;
        return Result::OK;
      });
  };

  const auto normalSource = makeToneSource(440.0);
  const auto fastSource = makeToneSource(660.0);
  const auto normalVarispeed = std::make_shared<AudioUnitVarispeed>();
  const auto fastVarispeed = std::make_shared<AudioUnitVarispeed>();
  normalVarispeed->setRate(1.0f);
  fastVarispeed->setRate(2.0f);
  engine->attachNode(normalSource);
  engine->attachNode(fastSource);
  engine->attachNode(normalVarispeed);
  engine->attachNode(fastVarispeed);
  engine->connect(normalSource, normalVarispeed, format);
  engine->connect(normalVarispeed, mixer, format);
  engine->connect(fastSource, fastVarispeed, format);
  engine->connect(fastVarispeed, mixer, format);
  check(!engine->start().has_value(), "the engine starts with both branches");

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  auto rendered = true;
  for (auto block = 0; block < 4 && rendered; ++block) {
    rendered = engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success;
  }

  std::vector<float> collected;
  for (auto block = 0; block < 8 && rendered; ++block) {
    if (engine->renderOffline(1024, buffer, &error) != AudioEngine::ManualRenderingStatus::Success) {
      rendered = false;
      break;
    }
    auto* const* channels = buffer->floatChannelData();
    if (channels == nullptr) {
      rendered = false;
      break;
    }
    collected.insert(collected.end(), channels[0], channels[0] + buffer->getFrameLength());
  }
  check(rendered, "the engine rendered every block of the mixed graph");
  check(longestSilentRun(collected) <= 8, "the fast branch leaves no silent gaps in the mix");

  const auto sampleRate = format.getSampleRate();
  const auto normal = windowedToneAmplitude(collected, 440.0, sampleRate);
  const auto fast = windowedToneAmplitude(collected, 1320.0, sampleRate);
  std::printf("  info branch amplitudes: %.3f at 440 Hz, %.3f at 1320 Hz\n", normal, fast);
  check(normal > 0.2, "the branch at rate 1.0 reaches the mixer at its own pitch");
  check(fast > 0.2, "the branch at rate 2.0 reaches the mixer at the resampled pitch");

  engine->stop();
  AudioEngine::Destroy(engine);
}

/**
 * The mixer scales a branch by the input volume of the node that feeds it: a node that
 * conforms to the AudioMixing protocol is read before the samples are summed.
 */
void testMixerInputVolume() {
  std::printf("mixer, the input volume of an AudioMixing node\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto makeConstantSource = [](const float value) {
    return AudioSourceNode::InitWithCallback(
      [value](bool* isSilence, const std::optional<AudioTime>& /* timestamp */,
        const AudioFrameCount /* frameCount */, std::unique_ptr<AudioBufferList>& outputData) -> Result {
        *isSilence = false;
        fillFloatBufferList(outputData, value);
        return Result::OK;
      });
  };

  const auto first = makeConstantSource(0.5f);
  const auto second = makeConstantSource(0.5f);
  engine->attachNode(first);
  engine->attachNode(second);
  engine->connect(first, mixer, format);
  engine->connect(second, mixer, format);

  // The property reports the documented range.
  check(std::abs(first->getVolume() - 1.0f) < 1e-6f, "an AudioMixing node starts at unity volume");
  first->setVolume(0.5f);
  check(std::abs(first->getVolume() - 0.5f) < 1e-6f, "the volume reports what was set");
  first->setVolume(2.0f);
  check(std::abs(first->getVolume() - 1.0f) < 1e-6f, "a volume above the range is clamped to 1.0");
  first->setVolume(-1.0f);
  check(std::abs(first->getVolume()) < 1e-6f, "a volume below the range is clamped to 0.0");
  first->setVolume(0.5f);

  check(!engine->start().has_value(), "the engine starts with the two sources");
  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders a block of the mix");

  // 0.5 from the first branch scaled by 0.5, plus 0.5 from the second branch.
  auto* const* channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.75f) < 1e-5f,
    "the mixer scaled the input by its volume before summing");
  check(channels != nullptr && std::abs(channels[1][1023] - 0.75f) < 1e-5f,
    "the scaled input reached the end of every output channel");

  // The volume applies to one connection only.
  first->setVolume(0.0f);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders after the input was muted");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.5f) < 1e-5f,
    "a muted input leaves only the other branch in the mix");

  // The input volume and the mixer's output volume compose.
  first->setVolume(0.5f);
  mixer->setOutputVolume(0.5f);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders with the mixer output volume");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.375f) < 1e-5f,
    "the input volume and the output volume both apply");
  mixer->setOutputVolume(1.0f);

  // A disconnected bus drops the interface the mixer cached with the connection.
  engine->stop();
  engine->disconnectNodeInput(mixer, 0);
  check(!engine->start().has_value(), "the engine restarts after the input was disconnected");
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders with a disconnected input bus");
  channels = buffer->floatChannelData();
  check(channels != nullptr && std::abs(channels[0][0] - 0.5f) < 1e-5f,
    "a disconnected branch no longer contributes to the mix");

  engine->stop();
  AudioEngine::Destroy(engine);
}

/**
 * Feeds the input node from the client's block in manual rendering mode.
 *
 * @discussion
 * This is the one path that renders an input node without a capture device, and it covers the
 * conversion a block format that differs from the graph format asks for.
 */
void testInputNodeManualRendering() {
  std::printf("input node, manual rendering\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  const auto mixer = engine->mainMixerNode();
  const auto input = engine->inputNode();
  check(input != nullptr, "the engine has an input node");
  engine->connect(input, mixer, format);
  check(!engine->start().has_value(), "the engine starts with the input node connected");

  // --- a block in the format of the graph ---
  constexpr AudioFrameCount kBlockFrames = 1024;
  auto samples = std::make_shared<std::vector<float>>(static_cast<size_t>(kBlockFrames) * kChannels, 0.25f);
  auto list = std::make_shared<AudioBufferList>(kChannels);
  for (uint32_t c = 0; c < kChannels; ++c) {
    auto* buffer = list->getBuffer(c);
    buffer->mNumberChannels = 1;
    buffer->mData = samples->data() + static_cast<size_t>(c) * kBlockFrames;
    buffer->mDataByteSize = kBlockFrames * static_cast<uint32_t>(sizeof(float));
  }
  std::atomic_int blocks{ 0 };
  input->setManualRenderingInputPCMFormat(format,
    [&](const uint32_t /* frames */) -> const AudioBufferList* {
      ++blocks;
      return list.get();
    });

  auto buffer = AudioPCMBuffer::InitWithPCMFormat(format, 1024);
  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders a block from the input node");
  auto* const* channels = buffer->floatChannelData();
  check(blocks.load() == 1, "the input block was called once");
  check(channels != nullptr && std::abs(channels[0][0] - 0.25f) < 1e-6f, "the block filled the first frame");
  check(channels != nullptr && std::abs(channels[0][1023] - 0.25f) < 1e-6f, "the block filled the whole render buffer");
  const auto inputTime = input->lastRenderTime();
  check(inputTime.has_value() && inputTime->getSampleTime() == 1024, "the input node counted the frames it rendered");

  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders a second block from the input node");
  check(blocks.load() == 2, "the input block was called again");
  const auto secondInputTime = input->lastRenderTime();
  check(secondInputTime.has_value() && secondInputTime->getSampleTime() == 2048, "the input node counted both blocks");

  // --- a block format that the graph converts: 16 bit interleaved ---
  const auto blockFormat = AudioFormat::InitWithCommonFormat(AudioSampleFormat::kInt16, kSampleRate, kChannels,
    true).value();
  auto shortSamples = std::make_shared<std::vector<int16_t>>(
    static_cast<size_t>(kBlockFrames) * kChannels, static_cast<int16_t>(16384));
  auto shortList = std::make_shared<AudioBufferList>(1);
  shortList->getBuffer(0)->mNumberChannels = kChannels;
  shortList->getBuffer(0)->mData = shortSamples->data();
  shortList->getBuffer(0)->mDataByteSize = kBlockFrames * kChannels * static_cast<uint32_t>(sizeof(int16_t));
  input->setManualRenderingInputPCMFormat(blockFormat,
    [&](const uint32_t /* frames */) -> const AudioBufferList* {
      return shortList.get();
    });

  check(engine->renderOffline(1024, buffer, &error) == AudioEngine::ManualRenderingStatus::Success,
    "the engine renders a 16 bit input block");
  channels = buffer->floatChannelData();
  const auto expected = 16384.0f / 32768.0f;
  check(channels != nullptr && std::abs(channels[0][0] - expected) < 1e-4f,
    "the 16 bit block was converted to the graph format");
  check(channels != nullptr && std::abs(channels[1][1023] - expected) < 1e-4f,
    "the conversion filled every channel to the end of the block");

  engine->stop();
  AudioEngine::Destroy(engine);
}

/**
 * Drives a chain that ends in a sink node without an audio device.
 *
 * @discussion
 * The sink is the one node the graph hands a null output view, and the documented way to run
 * it is a device rendering the graph. This section covers the same render block on a machine
 * without an endpoint by pulling the engine's sink root the way the device path does, and
 * checks the two things a node now gets from the render cycle: views of its input and the time
 * of the cycle.
 */
void testSinkNodeManualRendering() {
  std::printf("sink node, pulled without a device\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  Error* error = nullptr;
  check(engine->enableManualRenderingMode(AudioEngine::ManualRenderingMode::Offline, format, 1024, &error),
    "the engine accepts the offline manual rendering mode");

  std::atomic_int calls{ 0 };
  std::atomic<int64_t> framesReceived{ 0 };
  std::atomic<AudioFramePosition> timestampSeen{ -1 };
  std::atomic_bool byteSizesConsistent{ true };
  std::atomic_bool samplesReached{ true };
  const auto sink = AudioSinkNode::InitWithCallback(
    [&](const std::optional<AudioTime>& timestamp, const AudioFrameCount frameCount,
      const std::unique_ptr<AudioBufferList>& inputData) -> Result {
      ++calls;
      framesReceived += static_cast<int64_t>(frameCount);
      timestampSeen.store(timestamp.has_value() ? timestamp->getSampleTime() : -1);
      for (size_t b = 0; b < inputData->numberOfBuffers(); ++b) {
        const auto* buffer = inputData->getBuffer(b);
        if (buffer == nullptr || buffer->mData == nullptr ||
            buffer->mDataByteSize != frameCount * buffer->mNumberChannels * static_cast<uint32_t>(sizeof(float))) {
          byteSizesConsistent.store(false);
          continue;
        }
        const auto* samples = static_cast<const float*>(buffer->mData);
        for (AudioFrameCount f = 0; f < frameCount; ++f) {
          if (std::abs(samples[f] - 0.5f) > 1e-6f) {
            samplesReached.store(false);
            break;
          }
        }
      }
      return Result::OK;
    });

  // The chain that feeds the sink has to be disjoint from the output chain: the engine renders
  // each chain from one root.
  const auto source = AudioSourceNode::InitWithCallback(
    [](bool* isSilence, const std::optional<AudioTime>& /* timestamp */, const AudioFrameCount /* frameCount */,
      std::unique_ptr<AudioBufferList>& outputData) -> Result {
      *isSilence = false;
      fillFloatBufferList(outputData, 0.5f);
      return Result::OK;
    });
  engine->attachNode(source);
  engine->attachNode(sink);
  engine->connect(source, sink, format);
  check(!engine->start().has_value(), "the engine starts with a sink chain attached");

  // Neither manual rendering mode pulls a chain that ends in a sink, so the test pulls it the
  // way the output node's render pass does when a device drives the graph.
  const auto impl = std::static_pointer_cast<AudioEngineImpl>(engine);
  impl->renderSinkChains(1, 1024, AudioTime(0, format.getSampleRate(), 0));
  check(calls.load() == 1, "the receiver block was called once");
  check(framesReceived.load() == 1024, "the receiver block was handed the frames of the block");
  check(byteSizesConsistent.load(), "the buffers the block received match the frame count");
  check(samplesReached.load(), "the samples of the input reached the block");
  check(timestampSeen.load() == 0, "the block was timed with the cycle it ran in");

  // The node's own timeline is separate from the cycle: it counts the frames it received.
  const auto firstTime = sink->lastRenderTime();
  check(firstTime.has_value() && firstTime->getSampleTime() == 1024,
    "the sink node counted the frames it received");
  check(firstTime.has_value() && firstTime->getSampleRate() == format.getSampleRate(),
    "the sink node timed its frames at the rate of the render cycle");

  // A second cycle at another position is timed with that position, and the sink keeps counting.
  impl->renderSinkChains(2, 1024, AudioTime(4096, format.getSampleRate(), 0));
  check(calls.load() == 2, "the receiver block was called for the second cycle");
  check(timestampSeen.load() == 4096, "the block was timed with the position of the second cycle");
  const auto secondTime = sink->lastRenderTime();
  check(secondTime.has_value() && secondTime->getSampleTime() == 2048,
    "the sink node counted both blocks");

  engine->stop();
  AudioEngine::Destroy(engine);
}

/**
 * The sink node only runs while an audio device renders the graph, so this test needs an
 * output endpoint and is skipped when the engine cannot start one.
 */
void testSinkNodeRendering() {
  std::printf("sink node\n");

  const auto format = makeFloatFormat();
  auto engine = AudioEngine::Create();
  const auto mixer = engine->mainMixerNode();
  const auto output = engine->outputNode();
  const auto callerThread = std::this_thread::get_id();

  std::atomic_int calls{ 0 };
  std::atomic<int64_t> framesReceived{ 0 };
  std::atomic_bool byteSizesConsistent{ true };
  std::atomic_int foreignThreads{ 0 };
  const auto sink = AudioSinkNode::InitWithCallback(
    [&](const std::optional<AudioTime>& /* timestamp */, const AudioFrameCount frameCount,
      const std::unique_ptr<AudioBufferList>& inputData) -> Result {
      if (std::this_thread::get_id() != callerThread) {
        ++foreignThreads;
      }
      ++calls;
      framesReceived += static_cast<int64_t>(frameCount);
      for (size_t b = 0; b < inputData->numberOfBuffers(); ++b) {
        const auto* buffer = inputData->getBuffer(b);
        if (buffer == nullptr ||
            buffer->mDataByteSize != frameCount * buffer->mNumberChannels * static_cast<uint32_t>(sizeof(float))) {
          byteSizesConsistent.store(false);
        }
      }
      return Result::OK;
    });

  // The chain that feeds the sink has to be disjoint from the output chain: the engine renders
  // each chain from one root.
  const auto sinkSource = std::make_shared<AudioPlayerNode>();
  const auto outputSource = std::make_shared<AudioPlayerNode>();
  engine->attachNode(sinkSource);
  engine->attachNode(outputSource);
  engine->attachNode(sink);
  engine->connect(sinkSource, sink, format);
  engine->connect(outputSource, mixer, format);
  check(output != nullptr, "the engine has an output node");

  // Silence is scheduled on purpose: this test opens a real audio device, so it should not
  // make a sound while it checks that the receiver block runs.
  sinkSource->scheduleBuffer(makeConstantBuffer(format, 1024, 0.0f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferLoops, AudioPlayerNode::CompletionCallbackType::kRendered, nullptr);
  sinkSource->prepare(1024);
  sinkSource->play();
  outputSource->scheduleBuffer(makeConstantBuffer(format, 1024, 0.0f), std::nullopt,
    AudioPlayerNode::BufferOptions::kBufferLoops, AudioPlayerNode::CompletionCallbackType::kRendered, nullptr);
  outputSource->prepare(1024);
  outputSource->play();

  // The engine has to reach an audio endpoint, which a headless machine may not have.
  if (const auto startError = engine->start(); startError.has_value()) {
    std::printf("  skip (the engine could not start an audio device: %s)\n",
      startError->failureReason.c_str());
    AudioEngine::Destroy(engine);
    return;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (calls.load() == 0) {
    std::printf("  skip (no audio device rendered the graph)\n");
    engine->stop();
    AudioEngine::Destroy(engine);
    return;
  }

  check(framesReceived.load() > 0, "the receiver block received frames");
  check(byteSizesConsistent.load(), "the buffers the block received match the frame count");
  check(foreignThreads.load() > 0, "the receiver block ran on the audio render thread");

  sinkSource->stop();
  outputSource->stop();
  engine->stop();
  AudioEngine::Destroy(engine);
}

} // namespace anonymous

int main(int, char**) {
  // Unbuffered, so a failure still shows which check it happened after.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  testOfflineManualRendering();
  testOfflineFormatConversion();
  testRealtimeManualRendering();
  testGraphMutation();
  testPlayerNodeBufferPlayback();
  testPlayerNodeStopReset();
  testPlayerNodeFilePlayback();
  testSourceNodeRendering();
  testPlayerNodeResampledFilePlayback();
  testDelayUnit();
  testDistortionUnit();
  testEqualizerUnit();
  testReverbUnit();
  testTimePitchUnit();
  testVarispeedUnit();
  testVarispeedRateContinuity();
  testVarispeedBranchesIntoSingleMixer();
  testMixerInputVolume();
  testInputNodeManualRendering();
  testSinkNodeManualRendering();
  testSinkNodeRendering();

  std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}


