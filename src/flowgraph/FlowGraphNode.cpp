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

#include "FlowGraphNode.h"

#include "SinkFloat.h"
#include "SinkInt16.h"
#include "SinkInt24.h"
#include "SinkInt32.h"

#include "SourceFloat.h"
#include "SourceInt16.h"
#include "SourceInt24.h"
#include "SourceInt32.h"

namespace coral {

//--------------------------------------------------------------------------------------------------

void FlowGraphNode::reset() {
  mLastFrameCount = 0;
  mLastCallCount = kInitialCallCount;
}

int32_t FlowGraphNode::pullData(int32_t numFrames, int64_t callCount) {
  int32_t frameCount = numFrames;
  // Prevent recursion and multiple execution of nodes.
  if (callCount > mLastCallCount) {
    mLastCallCount = callCount;
    if (mDataPulledAutomatically) {
      // Pull from all the upstream nodes.
      for (auto& inputPort: mInputPorts) {
        frameCount = inputPort.get().pullData(callCount, frameCount);
      }
    }
    if (frameCount > 0) {
      frameCount = onProcess(frameCount);
    }
    mLastFrameCount = frameCount;
  } else {
    frameCount = mLastFrameCount;
  }
  return frameCount;
}

void FlowGraphNode::pullReset() {
  if (!mBlockRecursion) {
    mBlockRecursion = true; // for cyclic graphs
    // Pull reset from all the upstream nodes.
    for (auto& inputPort: mInputPorts) {
      inputPort.get().pullReset();
    }
    mBlockRecursion = false;
    reset();
  }
}

//--------------------------------------------------------------------------------------------------

AudioGraphPortFloat::AudioGraphPortFloat(FlowGraphNode& parent,
    int32_t samplesPerFrame,
    int32_t framesPerBuffer) noexcept
    : AudioGraphPort(parent, samplesPerFrame),
      mFramesPerBuffer(framesPerBuffer), mBuffer(nullptr) {
  size_t numFloats =
      static_cast<size_t>(framesPerBuffer) * getChannelCount();
  mBuffer = std::make_unique<float[]>(numFloats);
}

//--------------------------------------------------------------------------------------------------

void AudioGraphPortFloatOutput::connect(AudioGraphPortFloatInput* port) {
  if (port != nullptr) {
    port->connect(this);
  }
}

void AudioGraphPortFloatOutput::disconnect(AudioGraphPortFloatInput* port) {
  if (port != nullptr) {
    port->disconnect(this);
  }
}

int32_t AudioGraphPortFloatOutput::pullData(int64_t callCount,
    int32_t numFrames) {
  numFrames = std::min(getFramesPerBuffer(), numFrames);
  return mContainingNode.pullData(numFrames, callCount);
}

void AudioGraphPortFloatOutput::pullReset() {
  mContainingNode.pullReset();
}

//--------------------------------------------------------------------------------------------------

float* AudioGraphPortFloatInput::getBuffer() {
  if (mConnected == nullptr) {
    return AudioGraphPortFloat::getBuffer();
  }
  return mConnected->getBuffer();
}

int32_t AudioGraphPortFloatInput::pullData(int64_t framePosition,
    int32_t numFrames) {
  return (mConnected == nullptr)
         ? std::min(getFramesPerBuffer(), numFrames)
         : mConnected->pullData(framePosition, numFrames);
}

void AudioGraphPortFloatInput::pullReset() {
  if (mConnected != nullptr) {
    mConnected->pullReset();
  }
}



//----------------------------------------------------------------------------------------------------------------------

std::unique_ptr<FlowGraphSourceBuffered>
FlowGraphSourceBuffered::make(int32_t channelCount, SourceType sourceType) {
  switch (sourceType) {
    case SourceType::kSourceInt16:
      return std::make_unique<SourceInt16>(channelCount);
    case SourceType::kSourceInt24:
      return std::make_unique<SourceInt24>(channelCount);
    case SourceType::kSourceInt32:
      return std::make_unique<SourceInt32>(channelCount);
    default:
      return std::make_unique<SourceFloat>(channelCount);
  }
}

//----------------------------------------------------------------------------------------------------------------------

std::unique_ptr<FlowGraphSink>
FlowGraphSink::make(uint32_t channelCount, FlowGraphSink::SinkType sinkType) {
  switch (sinkType) {
    case SinkType::kSinkFloat:
      return std::make_unique<SinkFloat>(channelCount);
    case SinkType::kSinkInt16:
      return std::make_unique<SinkInt16>(channelCount);
    case SinkType::kSinkInt24:
      return std::make_unique<SinkInt24>(channelCount);
    case SinkType::kSinkInt32:
      return std::make_unique<SinkInt32>(channelCount);
    default:
      return nullptr;
  }
}

FlowGraphSink::FlowGraphSink(uint32_t channelCount) noexcept {
  input = std::make_unique<AudioGraphPortFloatInput>(*this, channelCount);
}

int32_t FlowGraphSink::pullData(int32_t numFrames) {
  return FlowGraphNode::pullData(numFrames, getLastCallCount() + 1);
}

AudioGraphPortFloatInput* FlowGraphSink::getInput(uint32_t bus) const {
  if (bus == 0) {
    return input.get();
  }
  return nullptr;
}

//----------------------------------------------------------------------------------------------------------------------
FlowGraphFilter::FlowGraphFilter(int32_t channelCount) noexcept
    : mManufacturerName("Coral"),
      mVersion(0),
      output(*this, channelCount) {
  input = std::make_unique<AudioGraphPortFloatInput>(*this, channelCount);
}

AudioGraphPortFloatInput* FlowGraphFilter::getInput(uint32_t bus) const {
  if (bus == 0) {
    return input.get();
  }
  return nullptr;
}

} // namespace coral
