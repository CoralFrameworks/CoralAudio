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

#include <coral/audio/AudioIONode.h>

#include <utility>

#include "private/graph/AudioInputNodeImpl.h"
#include "private/graph/AudioMixingUtils.h"
#include "private/graph/AudioOutputNodeImpl.h"

namespace coral {

AudioInputNode::AudioInputNode()
  : AudioIONode("AudioInputNode", std::make_unique<AudioInputNodeImpl>()) {
  declareOutputBus("out");
}

AudioInputNodeImpl* AudioInputNode::getImpl() const {
  return impl->asInputImpl();
}

void AudioInputNode::setManualRenderingInputPCMFormat(std::optional<AudioFormat> format, InputBlock block) const {
  impl->asInputImpl()->setManualRenderingInputPCMFormat(std::move(format), std::move(block));
}

std::shared_ptr<AudioMixingDestination>
AudioInputNode::destinationForMixer(const std::shared_ptr<AudioNode>& mixer, const AudioNodeBus bus) {
  return AudioMixingUtils::destinationForMixer(engine(), shared_from_this(), mixer, bus);
}

int64_t AudioInputNode::getPresentationLatency() const {
  return impl->asInputImpl()->getPresentationLatency();
}

AudioOutputNode::AudioOutputNode()
  : AudioIONode("AudioOutputNode", std::make_unique<AudioOutputNodeImpl>()) {
  declareInputBus("in");
}

AudioOutputNodeImpl* AudioOutputNode::getImpl() const {
  return impl->asOutputImpl();
}

int64_t AudioOutputNode::getPresentationLatency() const {
  return impl->asOutputImpl()->getPresentationLatency();
}

} // namespace coral
