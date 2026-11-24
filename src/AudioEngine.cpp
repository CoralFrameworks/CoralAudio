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

#include <coral/audio/AudioEngine.h>

#include "private/graph/AudioEngineImpl.h"

#include <minilog/log.h>

#include <ranges>
#include <sstream>

namespace coral {

std::shared_ptr<AudioEngine> AudioEngine::Create() {
  auto engine = std::make_shared<AudioEngineImpl>();
  engine->init();
  return engine;
}

void AudioEngine::Destroy(std::shared_ptr<AudioEngine> engine) {
  if (engine) {
    engine->shutdown();
    engine.reset();
  }
}

AudioEngine::AudioEngine() = default;

void AudioEngine::shutdown() {
  reset();
}

void AudioEngine::prepare() {
  if (mStatus.load(std::memory_order_relaxed) == Status::kStatusPrepared) {
    return;
  }
  onPrepare();
}

void AudioEngine::onPrepare() {
  mStatus.store(Status::kStatusPrepared);
}

std::optional<Error> AudioEngine::start() {
  if (mStatus.load(std::memory_order_relaxed) == Status::kStatusStarted) {
    MINILOG_DEBUG("The AudioEngine is already started");
    return std::nullopt;
  }
  // If the AudioEngine is stopped, prepare before start
  if (mStatus.load(std::memory_order_relaxed) == Status::kStatusStopped) {
    prepare();
  }
  return onStart();
}

std::optional<Error> AudioEngine::onStart() {
  mStatus.store(Status::kStatusStarted);
  return std::nullopt;
}

void AudioEngine::pause() {
  if (mStatus.load(std::memory_order_relaxed) == Status::kStatusPaused) {
    MINILOG_DEBUG("The AudioEngine is already paused");
    return;
  }
  onPause();
}

void AudioEngine::onPause() {
  mStatus.store(Status::kStatusPaused);
}

void AudioEngine::reset() {
  if (mStatus.load(std::memory_order_relaxed) != Status::kStatusStopped) {
    stop();
  }
  onReset();
}

void AudioEngine::onReset() {
  mStatus.store(Status::kStatusStopped, std::memory_order_relaxed);
  mManualRenderingSampleTime = 0;
}

void AudioEngine::stop() {
  if (mStatus.load(std::memory_order_relaxed) == Status::kStatusStopped) {
    MINILOG_DEBUG("The AudioEngine is already stopped");
    return;
  }
  onStop();
}

void AudioEngine::onStop() {
  mStatus.store(Status::kStatusStopped);
}

const std::shared_ptr<AudioInputNode>& AudioEngine::inputNode() {
  mayPrepareInputNode();
  return mInputNode;
}

const std::shared_ptr<AudioOutputNode>& AudioEngine::outputNode() {
  mayPrepareOutputNode();
  return mOutputNode;
}

const std::shared_ptr<AudioMixerNode>& AudioEngine::mainMixerNode() {
  mayPrepareMixerNode();
  return mMainMixerNode;
}

bool AudioEngine::enableManualRenderingMode(const ManualRenderingMode mode,
  const std::optional<AudioFormat>& pcmFormat,
  const AudioFrameCount maximumFrameCount,
  Error** outError) {

  if (isRunning()) {
    MINILOG_ERROR("Failed to enable manual rendering mode: the AudioEngine is still running.");
    return false;
  }

  mManualRenderingModeEnable = true;
  mManualRenderingMode = mode;
  mManualRenderingFormat = pcmFormat;
  mManualRenderingMaximumFrameCount = maximumFrameCount;

  return handleManualRenderingMode();
}

void AudioEngine::disableManualRenderingMode() {
  mManualRenderingModeEnable = false;
  mManualRenderingFormat.reset();
  mManualRenderingMaximumFrameCount = 0;
  mManualRenderingSampleTime = 0;
}

AudioEngine::ManualRenderingStatus
AudioEngine::renderOffline(const AudioFrameCount numberOfFrames,
  std::unique_ptr<AudioPCMBuffer>& toBuffer,
  Error** outError) {
  return ManualRenderingStatus::Error;
}

std::string AudioEngine::toString() const {
  std::stringstream out;

  const auto audioNodes = topologicalOrder();

  out << "{\n";
  if (audioNodes.empty()) {
    out << "attach nodes: [],\n";
  } else {
    out << "attach nodes: [\n";
    for (const auto& node: audioNodes) {
      out << node->getName() << ":" << node->toString() << ",\n";
    }
    out << "],\n";
  }
  out << "}";

  return out.str();
}

} // namespace coral
