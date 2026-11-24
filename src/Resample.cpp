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

#include <coral/audio/Resample.h>

#include "GenericResample.h"

namespace coral {
namespace {
class BuiltinResampleFactory final : public ResampleFactory {
public:
  BuiltinResampleFactory() = default;

  ~BuiltinResampleFactory() override = default;

  [[nodiscard]] std::unique_ptr<Resample>
  createResample(const AudioConfiguration& inputConfig, const AudioConfiguration& outputConfig) override {
    // use generic resample
    if (auto resample = std::make_unique<GenericResample>(); resample->init(inputConfig, outputConfig)) {
      return resample;
    } else {
      resample.reset();
    }
    return nullptr;
  }

  std::unique_ptr<Resample> createResample(const AudioFormat& inputFormat, const AudioFormat& outputFormat) override {
    if (!inputFormat.isValid() || !outputFormat.isValid()) {
      return nullptr;
    }
    const auto inputConfig = AudioConfiguration(inputFormat.getSampleRate(), inputFormat.getChannelCount(),
      inputFormat.getSampleFormat());
    const auto outputConfig = AudioConfiguration(outputFormat.getSampleRate(), outputFormat.getChannelCount(),
      outputFormat.getSampleFormat());
    return createResample(inputConfig, outputConfig);
  }
};
} // namespace anonymous

std::unique_ptr<ResampleFactory> CreateBuiltinResampleFactory() {
  return std::make_unique<BuiltinResampleFactory>();
}

} // namespace coral
