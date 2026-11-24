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

#include <coral/audio/AudioSourceNode.h>

#include "private/graph/AudioMixingUtils.h"
#include "private/graph/AudioSourceNodeImpl.h"

#include <algorithm>

namespace coral {

AudioSourceNode::AudioSourceNode(const std::optional<AudioFormat>& format, const RenderBlock& block)
  : AudioNode("AudioSourceNode", std::make_unique<AudioSourceNodeImpl>(format, block)) {
  declareOutputBus("out");
}

std::shared_ptr<AudioSourceNode>
AudioSourceNode::InitWithCallback(const RenderBlock& block) {
  return std::shared_ptr<AudioSourceNode>(new AudioSourceNode(std::nullopt, block));
}

std::shared_ptr<AudioSourceNode>
AudioSourceNode::InitWithCallback(const std::optional<AudioFormat>& format, const RenderBlock& block) {
  return std::shared_ptr<AudioSourceNode>(new AudioSourceNode(format, block));
}

std::shared_ptr<AudioMixingDestination>
AudioSourceNode::destinationForMixer(const std::shared_ptr<AudioNode>& mixer, const AudioNodeBus bus) {
  return AudioMixingUtils::destinationForMixer(engine(), shared_from_this(), mixer, bus);
}

} // namespace coral
