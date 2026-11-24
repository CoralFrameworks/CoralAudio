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

#include "private/graph/AudioMixingUtils.h"

#include <coral/audio/AudioEngine.h>
#include <coral/audio/AudioNode.h>

namespace coral {

std::shared_ptr<AudioMixingDestination>
AudioMixing::destinationForMixer(const std::shared_ptr<AudioNode>& mixer, AudioNodeBus bus) {
  // Do nothing
  return nullptr;
}

std::shared_ptr<AudioMixingDestination>
AudioMixingDestination::destinationForMixer(const std::shared_ptr<AudioNode>& mixer, const AudioNodeBus bus) {
  if (!mixer) {
    return nullptr;
  }
  if (connectionPoint &&
      connectionPoint->bus() == bus &&
      connectionPoint->node() == mixer) {
    return shared_from_this();
  }
  return nullptr;
}

std::shared_ptr<AudioMixingDestination>
AudioMixingUtils::destinationForMixer(const std::shared_ptr<AudioEngine>& engine,
  const std::shared_ptr<AudioNode>& sourceNode,
  const std::shared_ptr<AudioNode>& mixerNode,
  const AudioNodeBus mixerInputBus) {
  if (!engine || !sourceNode || !mixerNode) {
    return nullptr;
  }
  auto connectionPoint = engine->inputConnectionPointForNode(mixerNode, mixerInputBus);
  if (!connectionPoint ||
      !connectionPoint->node() ||
      (connectionPoint->node() != sourceNode)) {
    return nullptr;
  }
  return std::make_shared<AudioMixingDestination>(connectionPoint);
}

} // namespace coral
