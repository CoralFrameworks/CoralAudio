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

#ifndef CORAL_AUDIO_GRAPH_CHANNEL_COUNT_CONVERTER_H
#define CORAL_AUDIO_GRAPH_CHANNEL_COUNT_CONVERTER_H

#include "FlowGraphNode.h"

namespace coral {

/**
 * Change the number of channels without mixing.
 * When increasing the channel count, duplicate input channels.
 * When decreasing the channel count, drop input channels.
 *
 * The ChannelCountConverter supports one-to-one connection
 */
class ChannelCountConverter final : public FlowGraphNode {
public:
  explicit ChannelCountConverter(int32_t inputChannels,
      int32_t outputChannels) noexcept;

  int32_t onProcess(int32_t numFrames) override;

  [[nodiscard]] std::string getName() const override {
    return "ChannelCountConverter";
  }

  AudioGraphPortFloatInput* getInput(uint32_t bus) const override;

  AudioGraphPortFloatOutput* getOutput() { return &output; }

private:
  std::unique_ptr<AudioGraphPortFloatInput> input;
  AudioGraphPortFloatOutput output;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_CHANNEL_COUNT_CONVERTER_H
