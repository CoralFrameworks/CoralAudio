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

#ifndef CORAL_AUDIO_MIXER_NODE_H
#define CORAL_AUDIO_MIXER_NODE_H

#include <coral/audio/AudioMixing.h>
#include <coral/audio/AudioNode.h>

namespace coral {

/**
 * A node that mixes its inputs to a single output.
 *
 * @discussion
 * Mixers may have any number of inputs.
 *
 * The mixer accepts input at any sample rate and efficiently combines sample rate
 * conversions. It also accepts any channel count and will correctly upmix or downmix
 * to the output channel count.
 */
class AudioMixerNode : public AudioNode, public AudioMixing {
  friend class AudioEngine;
public:
  AudioMixerNode();

  /**
   * Sets the mixer's output volume.
   *
   * @param newOutputVolume the specified output volume(0.0-1.0, inclusive).
   */
  void setOutputVolume(float newOutputVolume);

  /**
   * Returns the mixer's output volume.
   *
   * @discussion
   * This accesses the mixer's output volume (0.0-1.0, inclusive).
   */
  [[nodiscard]] float getOutputVolume() const;

  /**
   * Find an unused input bus.
   *
   * @discussion
   * This will find and return the first input bus to which no other node is connected.
   */
  AudioNodeBus nextAvailableInputBus();

  std::shared_ptr<AudioMixingDestination>
  destinationForMixer(const std::shared_ptr<AudioNode>& mixer, AudioNodeBus bus) override;

};

} // namespace coral

#endif // CORAL_AUDIO_MIXER_NODE_H
