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

#include <coral/audio/AudioUnitVarispeed.h>

#include "private/graph/AudioUnitVarispeedImpl.h"

namespace coral {

AudioUnitVarispeed::AudioUnitVarispeed()
  : AudioUnitTimeEffect("AudioUnitVarispeed", std::make_unique<AudioUnitVarispeedImpl>()) {

}

bool AudioUnitVarispeed::isBypass() const {
  return impl->asVarispeedImpl()->isBypass();
}

void AudioUnitVarispeed::setBypass(bool newBypass) {
  impl->asVarispeedImpl()->setBypass(newBypass);
}

float AudioUnitVarispeed::getRate() const {
  return impl->asVarispeedImpl()->getRate();
}

void AudioUnitVarispeed::setRate(const float newRate) {
  impl->asVarispeedImpl()->setRate(std::clamp(newRate, 0.25f, 4.0f));
}

} // namespace coral
