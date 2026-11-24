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

#ifndef CORAL_AUDIO_GRAPH_SOURCE_INT24_H
#define CORAL_AUDIO_GRAPH_SOURCE_INT24_H

#include "FlowGraphNode.h"

namespace coral {

/**
 * AudioGraphSource that reads a block of pre-defined 24-bit packed integer
 * data.
 */
class SourceInt24 final : public FlowGraphSourceBuffered {
public:
  explicit SourceInt24(int32_t channelCount) noexcept;

  int32_t onProcess(int32_t numFrames) override;

  [[nodiscard]] std::string getName() const override {
    return "AudioSourceInt24";
  }

private:
  static constexpr int kBytesPerI24Packed = 3;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_SOURCE_INT24_H
