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

#include <coral/audio/AudioUnitDistortion.h>

#include "private/graph/AudioUnitDistortionImpl.h"

#include <algorithm>

namespace coral {

AudioUnitDistortion::AudioUnitDistortion()
  : AudioUnitEffect("AudioUnitDistortion", std::make_unique<AudioUnitDistortionImpl>()) {

}

void AudioUnitDistortion::loadFactoryPreset(const DistortionPreset preset) {
  impl->asDistortionImpl()->loadFactoryPreset(preset);
}

bool AudioUnitDistortion::isBypass() const {
  return impl->asDistortionImpl()->isBypass();
}

void AudioUnitDistortion::setBypass(const bool newBypass) {
  impl->asDistortionImpl()->setBypass(newBypass);
}

float AudioUnitDistortion::getPreGain() const {
  return impl->asDistortionImpl()->getPreGain();
}

void AudioUnitDistortion::setPreGain(const float newPreGain) {
  impl->asDistortionImpl()->setPreGain(newPreGain);
}

float AudioUnitDistortion::getWetDryMix() const {
  return impl->asDistortionImpl()->getWetDryMix();
}

void AudioUnitDistortion::setWetDryMix(const float newWetDryMix) {
  impl->asDistortionImpl()->setWetDryMix(newWetDryMix);
}

} // namespace coral
