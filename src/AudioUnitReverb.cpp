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

#include <coral/audio/AudioUnitReverb.h>

#include "private/graph/AudioUnitReverbImpl.h"

#include <algorithm>

namespace coral {

AudioUnitReverb::AudioUnitReverb()
  : AudioUnitEffect("AudioUnitReverb", std::make_unique<AudioUnitReverbImpl>()) {

}

void AudioUnitReverb::loadFactoryPreset(const ReverbPreset preset) {
  impl->asReverbImpl()->loadFactoryPreset(preset);
}

bool AudioUnitReverb::isBypass() const {
  return impl->asReverbImpl()->isBypass();
}

void AudioUnitReverb::setBypass(bool newBypass) {
  impl->asReverbImpl()->setBypass(newBypass);
}

float AudioUnitReverb::getWetDryMix() const {
  return impl->asReverbImpl()->getWetDryMix();
}

void AudioUnitReverb::setWetDryMix(const float wetDryMix) {
  impl->asReverbImpl()->setWetDryMix(wetDryMix);
}

} // namespace coral
