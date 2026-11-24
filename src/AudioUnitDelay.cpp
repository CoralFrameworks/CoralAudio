/*
 * Copyright (C) 2024 The Coral Project
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

#include <coral/audio/AudioUnitDelay.h>

#include "private/graph/AudioUnitDelayImpl.h"

#include <algorithm>

namespace coral {

AudioUnitDelay::AudioUnitDelay()
  : AudioUnitEffect("AudioUnitDelay", std::make_unique<AudioUnitDelayImpl>()) {

}

void AudioUnitDelay::setDelayTime(const uint64_t newDelayTime) {
  impl->asDelayImpl()->setDelayTime(newDelayTime);
}

bool AudioUnitDelay::isBypass() const {
  return impl->asDelayImpl()->isBypass();
}

uint64_t AudioUnitDelay::getDelayTime() const {
  return impl->asDelayImpl()->getDelayTime();
}

float AudioUnitDelay::getFeedback() const {
  return impl->asDelayImpl()->getFeedback();
}

float AudioUnitDelay::getLowPassCutoff() const {
  return impl->asDelayImpl()->getLowPassCutoff();
}

float AudioUnitDelay::getWetDryMix() const {
  return impl->asDelayImpl()->getWetDryMix();
}

void AudioUnitDelay::setBypass(const bool newBypass) {
  impl->asDelayImpl()->setBypass(newBypass);
}

void AudioUnitDelay::setFeedback(const float newFeedback) {
  impl->asDelayImpl()->setFeedback(std::clamp(newFeedback, -100.0f, 100.0f));
}

void AudioUnitDelay::setLowPassCutoff(const float newLowPassCutoff) {
  impl->asDelayImpl()->setLowPassCutoff(newLowPassCutoff);
}

void AudioUnitDelay::setWetDryMix(const float newWetDryMix) {
  impl->asDelayImpl()->setWetDryMix(newWetDryMix);
}

} // namespace coral
