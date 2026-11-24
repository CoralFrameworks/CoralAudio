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

#include <coral/audio/AudioUnitEqualizer.h>

#include "private/graph/AudioUnitEqualizerImpl.h"

namespace coral {

AudioUnitEqualizer::AudioUnitEqualizer(uint32_t numberOfBands)
  : AudioUnitEffect("AudioUnitEqualizer", std::make_unique<AudioUnitEqualizerImpl>(numberOfBands)) {

}

std::shared_ptr<AudioUnitEqualizer>
AudioUnitEqualizer::Create(const uint32_t numberOfBands) {
  return std::shared_ptr<AudioUnitEqualizer>(new AudioUnitEqualizer(numberOfBands));
}

bool AudioUnitEqualizer::isBypass() const {
  return impl->asEqualizerImpl()->isBypass();
}

void AudioUnitEqualizer::setBypass(bool newBypass) {
  impl->asEqualizerImpl()->setBypass(newBypass);
}

uint32_t AudioUnitEqualizer::getNumberOfBands() const {
  return impl->asEqualizerImpl()->getNumberOfBands();
}

std::vector<AudioUnitEqualizer::FilterParameters> AudioUnitEqualizer::getBands() const {
  return impl->asEqualizerImpl()->getBands();
}

void AudioUnitEqualizer::setBandParameters(const uint32_t band, const FilterParameters& parameters) {
  impl->asEqualizerImpl()->setBandParameters(band, parameters);
}

void AudioUnitEqualizer::setBands(const std::vector<FilterParameters>& bands) {
  impl->asEqualizerImpl()->setBands(bands);
}

float AudioUnitEqualizer::getGlobalGain() const {
  return impl->asEqualizerImpl()->getGlobalGain();
}

void AudioUnitEqualizer::setGlobalGain(const float newGlobalGain) {
  impl->asEqualizerImpl()->setGlobalGain(newGlobalGain);
}

} // namespace coral
