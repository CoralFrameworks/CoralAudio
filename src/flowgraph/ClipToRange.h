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

#ifndef CORAL_AUDIO_RANGE_FILTER_H
#define CORAL_AUDIO_RANGE_FILTER_H

#include "FlowGraphNode.h"

namespace coral {

class ClipToRange final : public FlowGraphFilter {
public:
  explicit ClipToRange(int32_t channelCount) noexcept;

  float getMinimum() const { return mMinimum; }

  void setMinimum(float min) { mMinimum = min; }

  float getMaximum() const { return mMaximum; }

  void setMaximum(float max) { mMaximum = max; }

  int32_t onProcess(int32_t numFrames) override;

  [[nodiscard]] std::string getName() const override {
    return "AudioRange";
  }

private:
  // This is 3 dB, (10^(3/20)), to match the maximum headroom for float data.
  // It is designed to allow occasional transient peaks.
  static constexpr float kDefaultMaxHeadroom = 1.41253754f;
  static constexpr float kDefaultMinHeadroom = -kDefaultMaxHeadroom;

  float mMinimum = kDefaultMinHeadroom;
  float mMaximum = kDefaultMaxHeadroom;
};

} // namespace coral

#endif // CORAL_AUDIO_RANGE_FILTER_H
