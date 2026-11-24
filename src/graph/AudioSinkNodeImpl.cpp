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

#include "private/graph/AudioSinkNodeImpl.h"

#include "private/graph/AudioEngineImpl.h"

#include <minilog/log.h>

#include <algorithm>
#include <utility>

namespace coral {

AudioSinkNodeImpl::AudioSinkNodeImpl(ReceiverBlock callback) noexcept
  : mCallback(std::move(callback)),
    mBufferList(std::make_unique<AudioBufferList>()) {
}

void AudioSinkNodeImpl::onReset() {
  // The receiver block is the reason this node exists, so a reset only clears the node's
  // timeline: the client keeps its block until `InitWithCallback` installs another one.
  mFramesReceived = 0;
  mBufferList->resize(0);
}

void AudioSinkNodeImpl::prepareBeforeRendering() {
  const auto engine = getEngine();
  if (engine == nullptr) {
    MINILOG_ERROR("Failed to prepare: The audio node is not attached to the AudioEngine.");
    return;
  }
  // The node is only reachable while a device renders the graph: manual rendering pulls the
  // output node, which no input chain reaches.
  if (engine->isInManualRenderingMode()) {
    MINILOG_ERROR("The sink node is only supported while the AudioEngine renders to an audio device");
  }

  mFramesReceived = 0;
  // The list the receiver block is called with is sized here, while the engine is stopped, so
  // that a render pass never has to grow it.
  if (const auto format = inputFormatForBus(0); format.has_value() && format->isValid()) {
    mBufferList->resize(format->getChannelCount());
  }
  AudioNodeImpl::prepareBeforeRendering();
}

AudioFrameCount AudioSinkNodeImpl::describeInput(const AudioBufferList& input, const AudioFrameCount frames) {
  const auto format = inputFormatForBus(0);
  const auto bytesPerSample = format.has_value() && format->isValid()
                                ? format->getBitsPerChannel() / 8
                                : sizeof(float);
  const auto buffers = input.numberOfBuffers();
  if (buffers == 0 || bytesPerSample == 0) {
    return 0;
  }

  // The block is promised valid buffers whose byte size matches the frame count, and that the
  // buffers only have to live for the duration of the call. The input buffers are views of the
  // upstream render buffer, whose memory satisfies both, so they are handed over as they are.
  if (mBufferList->numberOfBuffers() != buffers) {
    mBufferList->resize(buffers);
  }
  for (size_t b = 0; b < buffers; ++b) {
    const auto* buffer = input.getBuffer(b);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    auto* entry = mBufferList->getBuffer(b);
    entry->mNumberChannels = buffer->mNumberChannels;
    entry->mData = buffer->mData;
    entry->mDataByteSize = frames * buffer->mNumberChannels * bytesPerSample;
  }
  return frames;
}

void AudioSinkNodeImpl::renderBlock(const AudioBufferList* const* inputs,
  const uint32_t numInputs,
  AudioBufferList* /* outputData */,
  const AudioFrameCount numFrames,
  const AudioTime& when) {
  // A sink node has no output bus, so the engine calls this as the root of an input chain:
  // there is nothing to fill, only input to hand over.
  if (mCallback == nullptr || inputs == nullptr || numInputs == 0 || numFrames == 0) {
    return;
  }
  const auto* input = inputs[0];
  if (input == nullptr) {
    return;
  }

  if (describeInput(*input, numFrames) == 0) {
    return;
  }

  const auto result = mCallback(when, numFrames, mBufferList);
  if (result != Result::OK) {
    // The documentation states that a failed block invalidates the data it supplied, so the
    // frames are not accounted for.
    MINILOG_ERROR("Failed to call the receiver block of the sink node: {}", static_cast<int32_t>(result));
    return;
  }

  mFramesReceived += numFrames;
  updateLastRenderTime(mFramesReceived, renderSampleRate(when));
}

} // namespace coral
