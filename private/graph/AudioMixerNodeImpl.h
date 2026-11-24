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

#ifndef CORAL_AUDIO_GRAPH_MIXER_NODE_H
#define CORAL_AUDIO_GRAPH_MIXER_NODE_H

#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioConverter.h>

#include <memory>
#include <optional>
#include <vector>

namespace coral {

class AudioMixing;

/**
 * The implementation of the AudioMixerNode
 */
class AudioMixerNodeImpl final : public AudioNodeImpl {
public:
  AudioMixerNodeImpl();

  void setOutputVolume(float newOutputVolume);

  [[nodiscard]] float getOutputVolume() const {
    return outputVolume.load(std::memory_order_relaxed);
  }

  std::optional<AudioFormat> inputFormatForBus(AudioNodeBus bus) const override;

  AudioNodeBus nextAvailableInputBus();

  AudioMixerNodeImpl* asMixerImpl() override {
    return this;
  }
  bool setInputConnection(const AudioNode* inputNode,
    AudioNodeBus inputBus,
    const std::shared_ptr<AudioNode>& sourceNode,
    AudioNodeBus sourceBus,
    const std::optional<AudioFormat>& format) override;

  void onInputConnectionRemoved(AudioNodeBus bus) override;

protected:
  void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) override;

  bool supportsMultiInputs() const override {
    return true;
  }
private:

  /** Per input bus conversion state. */
  struct InputBusState {
    // std::atomic<float> volume{1.0f};
    // std::atomic<float> pan{0.0f};
    std::optional<AudioFormat> format;
    std::unique_ptr<AudioConverter> srcConverter;
    //!< The node on the other end of the connection, and its mixing interface when it has one.
    //!< Both are resolved when the connection is made, so the render path never runs RTTI.
    const AudioNode* sourceNode = nullptr;
    const AudioMixing* mixing = nullptr;
  };

  std::vector<InputBusState> mInputStates;
  mutable std::vector<float> mTempBuffer;
  std::vector<float> busGains_;

  std::atomic<float> outputVolume = 1.0f;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_MIXER_NODE_H
