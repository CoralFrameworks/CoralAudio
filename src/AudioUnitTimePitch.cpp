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

#include <coral/audio/AudioUnitTimePitch.h>

#include "private/graph/AudioUnitTimePitchImpl.h"

namespace coral {

AudioUnitTimePitch::AudioUnitTimePitch()
  : AudioUnitTimeEffect("AudioUnitTimePitch", std::make_unique<AudioUnitTimePitchImpl>()) {

}

bool AudioUnitTimePitch::isBypass() const {
  return impl->asTimePitchImpl()->isBypass();
}

void AudioUnitTimePitch::setBypass(const bool newBypass) {
  impl->asTimePitchImpl()->setBypass(newBypass);
}

float AudioUnitTimePitch::getPitch() const {
  return impl->asTimePitchImpl()->getPitch();
}

void AudioUnitTimePitch::setPitch(const float newPitch) {
  impl->asTimePitchImpl()->setPitch(newPitch);
}

float AudioUnitTimePitch::getRate() const {
  return impl->asTimePitchImpl()->getRate();
}

void AudioUnitTimePitch::setRate(const float newRate) {
  impl->asTimePitchImpl()->setRate(newRate);
}


float AudioUnitTimePitch::getOverlap() const {
  return impl->asTimePitchImpl()->getOverlap();
}

void AudioUnitTimePitch::setOverlap(const float newOverlap) {
  impl->asTimePitchImpl()->setOverlap(newOverlap);
}

} // namespace coral
