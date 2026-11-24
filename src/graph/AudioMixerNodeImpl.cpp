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

#include "private/graph/AudioMixerNodeImpl.h"

#include "private/graph/AudioGraphUtils.h"

#include <coral/audio/AudioMixing.h>
#include <coral/audio/AudioNode.h>

#include <algorithm>
#include <cmath> // for std::isnan

namespace coral {

AudioMixerNodeImpl::AudioMixerNodeImpl() = default;

void AudioMixerNodeImpl::setOutputVolume(float newOutputVolume) {
  outputVolume = std::clamp(newOutputVolume, 0.0f, 1.0f);
}

std::optional<AudioFormat>
AudioMixerNodeImpl::inputFormatForBus(const AudioNodeBus bus) const {
  if (bus < mInputStates.size()) {
    return mInputStates[bus].format;
  }
  return std::nullopt;
}

AudioNodeBus AudioMixerNodeImpl::nextAvailableInputBus() {
  for (auto bus = 0; bus < mInputStates.size(); ++bus) {
    if (!mInputBuses[bus].upstream.valid()) {
      return bus;
    }
  }
  return static_cast<AudioNodeBus>(mInputStates.size());
}

bool AudioMixerNodeImpl::setInputConnection(const AudioNode* inputNode,
  const AudioNodeBus inputBus,
  const std::shared_ptr<AudioNode>& sourceNode,
  const AudioNodeBus sourceBus,
  const std::optional<AudioFormat>& format) {

  // 1) Grow the input bus state up to the bus being connected.
  while (mInputStates.size() <= inputBus) {
    InputBusState st;
    st.format = (sourceNode && sourceBus < sourceNode->numberOfOutputs())
                  ? sourceNode->outputFormatForBus(sourceBus)
                  : std::nullopt;
    st.srcConverter = nullptr;
    mInputStates.push_back(std::move(st));
  }

  // 2) Remember the format the bus receives.
  if (sourceNode && sourceBus < sourceNode->numberOfOutputs()) {
    mInputStates[inputBus].format = sourceNode->outputFormatForBus(sourceBus);
  }

  // 3) Resolve the mixing interface of the source once, here, instead of running RTTI on the
  //    render thread for every block. The node stays alive while the connection exists, and
  //    onInputConnectionRemoved() clears the pointer when it is dropped.
  mInputStates[inputBus].sourceNode = sourceNode.get();
  mInputStates[inputBus].mixing = dynamic_cast<const AudioMixing*>(sourceNode.get());

  // 4) Drop any converter of the previous connection. A sample rate converter per bus is not
  //    built yet: the graph renders every node in the same format.
  mInputStates[inputBus].srcConverter.reset();

  return AudioNodeImpl::setInputConnection(inputNode, inputBus, sourceNode, sourceBus, format);
}

void AudioMixerNodeImpl::onInputConnectionRemoved(const AudioNodeBus inputBus) {
  if (inputBus < mInputStates.size()) {
    mInputStates[inputBus].sourceNode = nullptr;
    mInputStates[inputBus].mixing = nullptr;
  }
  AudioNodeImpl::onInputConnectionRemoved(inputBus);
}

void AudioMixerNodeImpl::renderBlock(const AudioBufferList* const* inputs,
  const uint32_t numInputs,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& /* when */) {
  // The graph renders standard deinterleaved float, so a list holds one float plane per
  // channel, and the render buffer is silent before this call: the inputs are summed into it.
  if (outputData == nullptr || numFrames == 0) {
    return;
  }
  const auto outChannels = static_cast<uint32_t>(outputData->numberOfBuffers());
  if (outChannels == 0) {
    return;
  }

  const auto outputGain = outputVolume.load(std::memory_order_relaxed);
  if (outputGain > 0.0f) {
    // The inputs are summed into the output, which is what makes the mixer a mixer.
    for (uint32_t bus = 0; bus < numInputs; ++bus) {
      const auto* input = inputs != nullptr ? inputs[bus] : nullptr;
      if (input == nullptr) {
        continue;
      }
      const auto inChannels = static_cast<uint32_t>(input->numberOfBuffers());
      if (inChannels == 0) {
        continue;
      }
      // A mono input feeds every output channel; otherwise the channels that exist on both
      // sides are mapped in order.
      const auto mapped = inChannels == 1 ? outChannels : std::min(inChannels, outChannels);

      // The volume of a source node that conforms to the AudioMixing protocol applies to the
      // mixer connection it feeds, so it is applied to the samples before they are summed.
      // The interface is resolved once, when the connection is made, so this path only reads
      // the cached pointer and the volume. A node that does not conform (an effect, for
      // example) has no input volume and keeps the unity gain of the connection.
      auto inputVolume = 1.0f;
      if (bus < mInputStates.size() && bus < mInputBuses.size()) {
        const auto& state = mInputStates[bus];
        if (state.mixing != nullptr && state.sourceNode == mInputBuses[bus].upstream.node) {
          inputVolume = state.mixing->getVolume();
        }
      }
      const auto gain =
        outputGain * inputVolume * (bus < busGains_.size() ? busGains_[bus] : 1.0f);

      for (uint32_t c = 0; c < mapped; ++c) {
        const auto* srcBuffer = input->getBuffer(inChannels == 1 ? 0 : c);
        const auto* dstBuffer = outputData->getBuffer(c);
        if (srcBuffer == nullptr || dstBuffer == nullptr ||
            srcBuffer->mData == nullptr || dstBuffer->mData == nullptr) {
          continue;
        }
        const auto* src = static_cast<const float*>(srcBuffer->mData);
        auto* dst = static_cast<float*>(dstBuffer->mData);
        for (AudioFrameCount f = 0; f < numFrames; ++f) {
          dst[f] += src[f] * gain;
        }
      }
    }
  }

  // A mixer sums, so the result is clipped back into the range the graph works in.
  for (uint32_t c = 0; c < outChannels; ++c) {
    const auto* buffer = outputData->getBuffer(c);
    if (buffer == nullptr || buffer->mData == nullptr) {
      continue;
    }
    auto* samples = static_cast<float*>(buffer->mData);
    for (AudioFrameCount f = 0; f < numFrames; ++f) {
      samples[f] = std::clamp(samples[f], -1.0f, 1.0f);
    }
  }
}

} // namespace coral
