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

#include <coral/audio/AudioMixerNode.h>

#include "private/graph/AudioMixerNodeImpl.h"
#include "private/graph/AudioMixingUtils.h"

#include <algorithm>

namespace coral {

AudioMixerNode::AudioMixerNode()
  : AudioNode("AudioMixerNode", std::make_unique<AudioMixerNodeImpl>()) {
  declareOutputBus("main out");
}

void AudioMixerNode::setOutputVolume(const float newOutputVolume) {
  impl->asMixerImpl()->setOutputVolume(newOutputVolume);
}

float AudioMixerNode::getOutputVolume() const {
  return impl->asMixerImpl()->getOutputVolume();
}

AudioNodeBus AudioMixerNode::nextAvailableInputBus() {
  return impl->asMixerImpl()->nextAvailableInputBus();
}

std::shared_ptr<AudioMixingDestination>
AudioMixerNode::destinationForMixer(const std::shared_ptr<AudioNode>& mixer, const AudioNodeBus bus) {
  return AudioMixingUtils::destinationForMixer(engine(), shared_from_this(), mixer, bus);
}

} // namespace coral
