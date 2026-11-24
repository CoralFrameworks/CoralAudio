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

#include <coral/audio/AudioNode.h>

#include <coral/audio/AudioEngine.h>
#include <coral/audio/AudioTime.h>

#include "private/graph/AudioEngineImpl.h"
#include "private/graph/AudioNodeImpl.h"

#include <minilog/log.h>

#include <algorithm>
#include <ranges>
#include <utility>

namespace coral {

AudioNode::AudioNode(std::string name, std::unique_ptr<AudioNodeImpl> impl) noexcept
  : mName(std::move(name)),
    impl(std::move(impl)) {
}

AudioNode::~AudioNode() {
  impl.reset();
}

void AudioNode::reset() {
  impl->clear();
}

std::optional<AudioFormat>
AudioNode::inputFormatForBus(const AudioNodeBus bus) const {
  return impl->inputFormatForBus(bus);
}

std::optional<AudioFormat>
AudioNode::outputFormatForBus(const AudioNodeBus bus) const {
  return impl->outputFormatForBus(bus);
}

std::string AudioNode::nameForInputBus(const AudioNodeBus bus) const {
  return impl->nameForInputBus(bus);
}

std::string AudioNode::nameForOutputBus(const AudioNodeBus bus) const {
  return impl->nameForOutputBus(bus);
}

size_t AudioNode::numberOfInputs() const {
  return impl->numberOfInputs();
}

size_t AudioNode::numberOfOutputs() const {
  return impl->numberOfOutputs();
}

void AudioNode::installTapOnBus(const AudioNodeBus bus,
  const AudioFrameCount bufferSize,
  const std::optional<AudioFormat>& format,
  TapBlock block) {
  impl->installTapOnBus(bus, bufferSize, format, std::move(block));
}

void AudioNode::removeTapOnBus(const AudioNodeBus bus) {
  impl->removeTapOnBus(bus);
}

std::shared_ptr<AudioEngine> AudioNode::engine() const {
  return impl->getEngine();
}

std::optional<AudioTime> AudioNode::lastRenderTime() const {
  return impl->getLastRenderTime();
}

uint64_t AudioNode::latency() const {
  return impl->getLatency();
}

uint64_t AudioNode::outputPresentationLatency() const {
  return impl->getOutputPresentationLatency();
}

void AudioNode::declareInputBus(const std::string& busName) {
  impl->declareInputBus(busName);
}

void AudioNode::declareOutputBus(const std::string& busName) {
  impl->declareOutputBus(busName);
}

bool AudioNode::setInputConnection(const AudioNodeBus inputBus,
  const std::shared_ptr<AudioNode>& sourceNode,
  const AudioNodeBus sourceBus,
  const std::optional<AudioFormat>& format) {
  return impl->setInputConnection(this, inputBus, sourceNode, sourceBus, format);
}

void AudioNode::attachToEngine(const std::shared_ptr<AudioEngineImpl>& engine) {
  impl->attachToEngine(engine);
}

void AudioNode::detachFromEngine() {
  impl->detachFromEngine();
}

void AudioNode::clearInputConnection(const AudioNodeBus bus) noexcept {
  impl->removeInputConnection(this, bus);
}

void AudioNode::clearInputConnectionFrom(const AudioNode* fromNode,
  const AudioNodeBus fromBus,
  const AudioNodeBus bus) noexcept {
  impl->removeInputConnection(fromNode, fromBus, bus);
}

void AudioNode::setOutputConnection(const AudioNodeBus outBus,
  AudioNode* targetNode,
  const AudioNodeBus targetBus) noexcept {
  impl->addOutputConnection(outBus, targetNode, targetBus);
}

void AudioNode::clearOutputConnection(const AudioNodeBus outBus,
  const AudioNode* targetNode,
  const AudioNodeBus targetBus) noexcept {
  impl->removeOutputConnection(outBus, targetNode, targetBus);
}

void AudioNode::clearOutputConnection(const AudioNodeBus outBus) noexcept {
  impl->removeOutputConnection(this, outBus);
}

void AudioNode::prepareBeforeRendering() {
  impl->prepareBeforeRendering();
}

const AudioPCMBuffer* AudioNode::process(const uint64_t epoch, const AudioFrameCount frames, const AudioTime& when) {
  return impl->render(epoch, frames, when);
}

std::string AudioNode::toString() const {
  return impl->toString();
}

} // namespace coral
