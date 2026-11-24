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

#include <coral/audio/AudioPlayerNode.h>

#include <coral/audio/AudioBuffer.h>
#include <core/time/SystemClock.h>

#include "private/graph/AudioMixingUtils.h"
#include "private/graph/AudioPlayerNodeImpl.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace coral {

AudioPlayerNode::AudioPlayerNode()
  : AudioNode("AudioPlayerNode", std::make_unique<AudioPlayerNodeImpl>()) {
  declareOutputBus("out");
}

void AudioPlayerNode::scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer, CompletionHandler completionHandler) {
  scheduleBuffer(std::move(buffer), std::nullopt, BufferOptions::kBufferInterruptsAtLoop,
    CompletionCallbackType::kPlayedBack, std::move(completionHandler));
}

void AudioPlayerNode::scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer,
  const CompletionCallbackType callbackType,
  CompletionHandler completionHandler) {
  scheduleBuffer(std::move(buffer), std::nullopt, BufferOptions::kBufferInterruptsAtLoop, callbackType,
    std::move(completionHandler));
}

void AudioPlayerNode::scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer,
  const std::optional<AudioTime>& when,
  const BufferOptions options,
  CompletionHandler completionHandler) {
  scheduleBuffer(std::move(buffer), when, options, CompletionCallbackType::kPlayedBack,
    std::move(completionHandler));
}

void AudioPlayerNode::scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer,
  const std::optional<AudioTime>& when,
  const BufferOptions options,
  const CompletionCallbackType callbackType,
  CompletionHandler completionHandler) {
  impl->asPlayerImpl()->scheduleBuffer(std::move(buffer), when, options, callbackType, std::move(completionHandler));
}

void AudioPlayerNode::scheduleFile(const std::shared_ptr<AudioFile>& file,
  const std::optional<AudioTime>& when,
  CompletionHandler completionHandler) {
  scheduleFile(file, when, CompletionCallbackType::kPlayedBack, std::move(completionHandler));
}

void AudioPlayerNode::scheduleFile(const std::shared_ptr<AudioFile>& file,
  const std::optional<AudioTime>& when,
  const CompletionCallbackType callbackType,
  CompletionHandler completionHandler) {
  impl->asPlayerImpl()->scheduleFile(file, when, callbackType, std::move(completionHandler));
}

void AudioPlayerNode::scheduleSegment(const std::shared_ptr<AudioFile>& file,
  const AudioFramePosition startFrame,
  const AudioFrameCount numberFrames,
  const std::optional<AudioTime>& when,
  CompletionHandler completionHandler) {
  scheduleSegment(file, startFrame, numberFrames, when,
    CompletionCallbackType::kPlayedBack, std::move(completionHandler));
}

void AudioPlayerNode::scheduleSegment(const std::shared_ptr<AudioFile>& file,
  const AudioFramePosition startFrame,
  const AudioFrameCount numberFrames,
  const std::optional<AudioTime>& when,
  const CompletionCallbackType callbackType,
  CompletionHandler completionHandler) {
  impl->asPlayerImpl()->scheduleSegment(file, startFrame, numberFrames, when, callbackType,
    std::move(completionHandler));
}

void AudioPlayerNode::prepare(const uint32_t frameCount) {
  impl->asPlayerImpl()->prepare(frameCount);
}

void AudioPlayerNode::play() {
  impl->asPlayerImpl()->playback(0);
}


void AudioPlayerNode::playAtTime(const uint64_t timeUs) {
  const auto [quot, rem] =
    std::div(static_cast<int64_t>(timeUs - SystemClock::CurrentTimeMicros()),
      Definitions::kMicrosPerMillisecond);
  impl->asPlayerImpl()->playback(quot);
}

void AudioPlayerNode::pause() {
  impl->asPlayerImpl()->pause();
}

void AudioPlayerNode::stop() {
  impl->asPlayerImpl()->stop();
}

std::optional<AudioTime>
AudioPlayerNode::nodeTimeForPlayerTime(const std::optional<AudioTime>& playerTime) const {
  return impl->asPlayerImpl()->nodeTimeForPlayerTime(playerTime);
}

std::optional<AudioTime>
AudioPlayerNode::playerTimeForNodeTime(const std::optional<AudioTime>& nodeTime) const {
  return impl->asPlayerImpl()->playerTimeForNodeTime(nodeTime);
}

bool AudioPlayerNode::isPlaying() const {
  return impl->asPlayerImpl()->isPlaying();
}

std::shared_ptr<AudioMixingDestination>
AudioPlayerNode::destinationForMixer(const std::shared_ptr<AudioNode>& mixer, const AudioNodeBus bus) {
  return AudioMixingUtils::destinationForMixer(engine(), shared_from_this(), mixer, bus);
}

} // namespace coral
