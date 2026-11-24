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

#ifndef CORAL_AUDIO_GRAPH_MANY_TO_MULTI_CONVERTER_H
#define CORAL_AUDIO_GRAPH_MANY_TO_MULTI_CONVERTER_H

#include "FlowGraphNode.h"

namespace coral {

/**
 * Combine multiple mono inputs into one interleaved multi-channel output.
 *
 * The ManyToMultiConverter supports supports many-to-one connection.
 */
class ManyToMultiConverter final : public FlowGraphNode {
public:
  explicit ManyToMultiConverter(int32_t channelCount) noexcept;

  int32_t onProcess(int32_t numFrames) override;

  AudioGraphPortFloatInput* getInput(uint32_t bus) const override;

  AudioGraphPortFloatOutput* getOutput() { return &output; }

  [[nodiscard]] std::string getName() const override {
    return "ManyToMultiConverter";
  }

private:
  std::vector<std::unique_ptr<AudioGraphPortFloatInput>> inputs;
  AudioGraphPortFloatOutput output;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_MANY_TO_MULTI_CONVERTER_H
