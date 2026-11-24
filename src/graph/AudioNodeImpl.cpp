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

#include "private/graph/AudioNodeImpl.h"

#include "private/graph/AudioEngineImpl.h"
#include "private/graph/AudioGraphUtils.h"

#include <coral/audio/AudioNode.h>

#include <minilog/log.h>

#include <algorithm>
#include <ranges>
#include <sstream>
#include <utility>

namespace coral {

AudioNodeImpl::AudioNodeImpl()
  : mLatency(0),
    mOutputPresentationLatency(0) {
}

void AudioNodeImpl::clear() {
  mLastRenderTime.reset();
  mCachedEpoch = 0;
  // The render buffer belongs to a prepared engine, so it is released here rather
  // than kept alive while the node is stopped.
  mOutputBuffer.reset();
  onReset();
}

void AudioNodeImpl::onReset() {
  // Do nothing
}

void AudioNodeImpl::declareInputBus(std::string busName) {
  mInputBuses.push_back({ .format = std::nullopt, .name = std::move(busName), .upstream = {} });
}

void AudioNodeImpl::declareOutputBus(std::string busName) {
  mOutputBuses.push_back({ .format = std::nullopt, .name = std::move(busName), .connections = {} });
}

std::optional<AudioFormat>
AudioNodeImpl::inputFormatForBus(const AudioNodeBus bus) const {
  if (numberOfInputs() == 0 || bus >= mInputBuses.size()) {
    return std::nullopt;
  }
  return mInputBuses[bus].format;
}

std::optional<AudioFormat>
AudioNodeImpl::outputFormatForBus(const AudioNodeBus bus) const {
  if (numberOfOutputs() == 0 || bus >= mOutputBuses.size()) {
    return std::nullopt;
  }
  return mOutputBuses[bus].format;
}

std::string AudioNodeImpl::nameForInputBus(AudioNodeBus bus) const {
  if (numberOfInputs() == 0 || bus >= mInputBuses.size()) {
    return "";
  }
  return mInputBuses[bus].name;
}

std::string AudioNodeImpl::nameForOutputBus(AudioNodeBus bus) const {
  if (numberOfOutputs() == 0 || bus >= mOutputBuses.size()) {
    return "";
  }
  return mOutputBuses[bus].name;
}

void AudioNodeImpl::installTapOnBus(const AudioNodeBus outBus,
  const AudioFrameCount bufferSize,
  const std::optional<AudioFormat>& format,
  AudioNode::TapBlock block) {
  if (outBus >= numberOfOutputs()) {
    MINILOG_ERROR("Failed to install tap: bus out of range");
    return;
  }
  if (bufferSize == 0) {
    MINILOG_ERROR("Failed to install tap: bufferSize must be > 0");
    return;
  }
  if (!block) {
    MINILOG_ERROR("Failed to install tap: block must be non-null");
    return;
  }
  if (!getEngine()) {
    MINILOG_ERROR("Failed to install tap: this node is not attached to any AudioEngine");
    return;
  }

  const auto outputFormat = outputFormatForBus(outBus);
  if (outputFormat != std::nullopt && format != std::nullopt && format != outputFormat) {
    MINILOG_ERROR("Failed to install tap: format does not match the bus output format");
    return;
  }

  const auto tapFormat = format != std::nullopt ? format : outputFormat;
  if (tapFormat == std::nullopt || !tapFormat->isValid()) {
    MINILOG_ERROR("Failed to install tap: the bus has no valid format");
    return;
  }

  auto tapBuffer = AudioPCMBuffer::InitWithPCMFormat(tapFormat.value(), bufferSize);
  if (tapBuffer == nullptr) {
    MINILOG_ERROR("Failed to install tap: cannot allocate the tap buffer");
    return;
  }

  const std::lock_guard<std::mutex> guard(mTapsLock);
  if (mTaps.size() <= outBus) {
    mTaps.resize(outBus + 1);
  }
  // Built in place: the pending flag makes TapState neither copyable nor movable.
  mTaps[outBus] = std::shared_ptr<TapState>(new TapState{
    .bus = outBus,
    .bufferSize = bufferSize,
    .format = tapFormat,
    .block = std::move(block),
    .tapBuffer = std::move(tapBuffer),
  });
  mHasTaps.store(true, std::memory_order_release);
}

void AudioNodeImpl::removeTapOnBus(const AudioNodeBus outBus) {
  const std::lock_guard<std::mutex> guard(mTapsLock);
  if (outBus < mTaps.size()) {
    mTaps[outBus].reset();
  }
  mHasTaps.store(std::ranges::any_of(mTaps, [](const auto& tap) { return tap != nullptr; }),
    std::memory_order_release);
}

bool AudioNodeImpl::isInputBusAvailable(const AudioNodeBus bus) const {
  if (bus >= mInputBuses.size()) {
    return false;
  }
  return !mInputBuses[bus].upstream.valid();
}

void AudioNodeImpl::updateLastRenderTime(AudioFramePosition position, Float64 sampleRate) {
  // TODO get the host time from the audio 
  mLastRenderTime = AudioTime(position, sampleRate, 0);
}

Float64 AudioNodeImpl::renderSampleRate(const AudioTime& when) const {
  if (when.getSampleRate() > 0) {
    return when.getSampleRate();
  }
  const auto format = renderFormat();
  return format.has_value() ? format->getSampleRate() : 0;
}

bool AudioNodeImpl::setInputConnection(const AudioNode* inputNode,
  const AudioNodeBus inputBus,
  const std::shared_ptr<AudioNode>& sourceNode,
  const AudioNodeBus sourceBus,
  const std::optional<AudioFormat>& format) {

  if (inputBus >= mInputBuses.size()) {
    if (!supportsMultiInputs()) {
      return false;
    }
    // declare a new input bus and update the format and connection
    while (mInputBuses.size() <= inputBus) {
      mInputBuses.push_back({
        .format = format,
        .name = "in " + std::to_string(mInputBuses.size()),
        .upstream = Connection(sourceNode.get(), sourceBus)
      });
    }
  } else {
    // remove the output bus of the upstream node.
    auto& input = mInputBuses[inputBus];
    if (input.upstream.valid()) {
      input.upstream.node->clearOutputConnection(input.upstream.bus, inputNode, inputBus);
    }

    // If the input bus has already been declared, only update the format and connection of that bus.
    input.format = format;
    input.upstream = { .node = sourceNode.get(), .bus = sourceBus };
  }
  // updates the format of the source node's output bus
  if (sourceBus < sourceNode->numberOfOutputs()) {
    sourceNode->impl->mOutputBuses[sourceBus].format = format;
  }
  return true;
}

void AudioNodeImpl::removeInputConnection(const AudioNode *inputNode, AudioNodeBus inputBus) noexcept {
  if (inputBus >= mInputBuses.size()) {
    return;
  }
  if (auto& input = mInputBuses[inputBus]; input.upstream.valid()) {
    input.upstream.node->clearOutputConnection(input.upstream.bus, inputNode, inputBus);
    input.upstream = Connection{};
    onInputConnectionRemoved(inputBus);
  }
}

void AudioNodeImpl::removeInputConnection(const AudioNode* fromNode,
  const AudioNodeBus fromBus,
  const AudioNodeBus inputBus) noexcept {
  if (inputBus >= mInputBuses.size()) {
    return;
  }
  if (auto& input = mInputBuses[inputBus]; input.upstream.node == fromNode && input.upstream.bus == fromBus) {
    input.upstream = {};
    onInputConnectionRemoved(inputBus);
  }
}

void AudioNodeImpl::addOutputConnection(const AudioNodeBus outBus,
  AudioNode* destNode,
  const AudioNodeBus destBus) {
  if (outBus >= mOutputBuses.size()) {
    return;
  }
  auto& connections = mOutputBuses[outBus].connections;
  for (const auto& [node, bus]: connections) {
    if (node == destNode && bus == destBus) {
      return;
    }
  }
  connections.push_back({ .node = destNode, .bus = destBus });
}

void AudioNodeImpl::removeOutputConnection(const AudioNode* outputNode, const AudioNodeBus outputBus) {
  if (outputBus >= mOutputBuses.size()) {
    return;
  }
  auto& connections = mOutputBuses[outputBus].connections;
  for (const auto& [targetNode, targetBus]: connections) {
    if (targetNode) {
      targetNode->clearInputConnectionFrom(outputNode, outputBus, targetBus);
    }
  }
  connections.clear();
}

void AudioNodeImpl::removeOutputConnection(const AudioNodeBus outBus,
  const AudioNode* targetNode,
  const AudioNodeBus targetBus) {
  if (outBus >= mOutputBuses.size()) {
    return;
  }
  auto& connections = mOutputBuses[outBus].connections;
  connections.erase(std::ranges::remove_if(connections,
    [&](const Connection& connection) {
      return connection.node == targetNode && connection.bus == targetBus;
    }).begin(), connections.end());
}

std::optional<AudioFormat> AudioNodeImpl::renderFormat() const {
  return outputFormatForBus(0);
}

void AudioNodeImpl::prepareBeforeRendering() {
  prepareRenderBuffer();
}

void AudioNodeImpl::prepareRenderBuffer() {
  // The graph and therefore every render buffer works in the standard float format.
  const auto format = renderFormat();
  if (!format.has_value() || !format->isValid()) {
    if (numberOfOutputs() > 0) {
      MINILOG_ERROR("Failed to prepare: the output bus has no valid format");
    }
    return;
  }

  const auto capacity = std::max(mMaxRenderFrames, kDefaultNumberFrames);
  mOutputBuffer = AudioPCMBuffer::InitWithPCMFormat(format.value(), capacity);
  if (mOutputBuffer == nullptr) {
    MINILOG_ERROR("Failed to prepare: cannot allocate the render buffer");
    return;
  }
  mOutputBuffer->setFrameLength(0);
  mCachedEpoch = 0;
}

const AudioPCMBuffer* AudioNodeImpl::render(const uint64_t epoch, const AudioFrameCount frames, const AudioTime& when) {
  if (mCachedEpoch == epoch) {
    return mOutputBuffer.get();
  }

  // The buffer is sized for the largest block the engine announced; a larger request is
  // clamped rather than grown on an audio thread.
  const auto renderFrames = mOutputBuffer != nullptr
                              ? std::min(frames, mOutputBuffer->getFrameCapacity())
                              : frames;

  // A node that consumes a different number of frames than it produces, which a time effect
  // at a rate other than 1.0 does, asks its inputs for the count it needs. The upstream
  // buffers were sized for the largest count this node may ask, so the request normally fits;
  // a buffer that is smaller anyway clamps it instead of reading past its end.
  auto inputFrames = inputFramesForBlock(renderFrames);
  if (inputFrames == 0) {
    inputFrames = renderFrames > 0 ? renderFrames : 1;
  }
  for (const auto& bus: mInputBuses) {
    if (bus.upstream.node != nullptr) {
      inputFrames = std::min(inputFrames, bus.upstream.node->impl->getMaxRenderFrames());
    }
  }

  // Only the owning buffers are kept: a render block is handed views of their memory, while
  // the frame lengths, the silence padding and the taps need the objects themselves.
  const auto inputSize = mInputBuses.size();
  if (mInputPtrs.size() < inputSize) {
    mInputPtrs.resize(inputSize);
  }
  if (mInputViews.size() < inputSize) {
    mInputViews.resize(inputSize);
  }
  for (size_t i = 0; i < inputSize; ++i) {
    const auto srcNode = mInputBuses[i].upstream.node;
    const auto* pcm = srcNode != nullptr ? srcNode->process(epoch, inputFrames, when) : nullptr;
    mInputPtrs[i] = pcm;
    // A null view keeps the meaning a null buffer had: this bus has no input.
    mInputViews[i] = pcm != nullptr ? pcm->audioBufferList() : nullptr;
  }

  // An upstream node may produce less than the block it was asked for, for instance when a
  // player has nothing buffered yet. Whatever it did not produce is silenced here, so every
  // input a node reads holds exactly `inputFrames` valid frames: a node that does not check
  // the length of its input then reads silence instead of the samples of an earlier block,
  // which is heard as noise.
  for (size_t i = 0; i < inputSize; ++i) {
    auto* const input = mInputPtrs[i];
    if (input == nullptr) {
      continue;
    }
    const auto produced = std::min(input->getFrameLength(), inputFrames);
    if (produced < inputFrames) {
      // The buffer belongs to the node that produced it, which hands it out as a read-only
      // block: filling its tail is what makes the block the rest of the graph reads valid.
      AudioGraphUtils::silencePCMBuffer(*const_cast<AudioPCMBuffer*>(input), produced, inputFrames - produced);
    }
    const_cast<AudioPCMBuffer*>(input)->setFrameLength(inputFrames);
  }

  if (mOutputBuffer != nullptr) {
    mOutputBuffer->setFrameLength(renderFrames);
    // A node that produces less than the block asks for leaves the tail of its buffer as it
    // was, and the next node renders that: the block starts as silence, so an underfilled
    // buffer ends in silence instead of in the samples of an earlier block.
    AudioGraphUtils::silencePCMBuffer(*mOutputBuffer, renderFrames);
    renderBlock(mInputViews.data(),
      static_cast<uint32_t>(inputSize),
      mOutputBuffer->mutableAudioBufferList(),
      renderFrames,
      when);
    dispatchTaps(*mOutputBuffer, renderFrames);
  } else {
    // A node without an output bus (an AudioSinkNode) ends an input chain. There is no
    // render buffer to fill and nothing to tap, but the node still has to consume its
    // inputs, so the engine pulls it as the root of that chain. Its inputs are filled the
    // same way, for the same reason, and it is handed a null output view.
    renderBlock(mInputViews.data(), static_cast<uint32_t>(inputSize), nullptr, frames, when);
  }

  mCachedEpoch = epoch;
  return mOutputBuffer.get();
}

void AudioNodeImpl::dispatchTaps(const AudioPCMBuffer& source, const AudioFrameCount frames) {
  if (frames == 0 || !mHasTaps.load(std::memory_order_acquire)) {
    return;
  }
  const auto format = source.getFormat();
  if (!format.has_value() || format->getSampleFormat() != AudioSampleFormat::kFloat) {
    return;
  }

  const auto engine = getEngine();
  // In manual rendering mode the client (or the engine's own render thread) drives the
  // render call, so the tap block runs on that thread. When rendering to an audio
  // device the render call runs on the audio driver thread, where the block must not
  // run: a full buffer is published and delivered later by `flushTaps()`.
  const bool asynchronous = engine != nullptr && engine->isTapDispatchAsynchronous();
  {
    static std::atomic_int sDispatchCount{ 0 };
    if (sDispatchCount.fetch_add(1, std::memory_order_relaxed) < 5) {
      MINILOG_DEBUG("Dispatching taps: {} frames, {} tap(s), {} asynchronous", frames,
        mTaps.size(), asynchronous);
    }
  }

  {
    const std::lock_guard<std::mutex> guard(mTapsLock);
    // Only this thread ever touches `mReadyTaps`, so growing it here cannot race with a
    // client that installs or removes a tap.
    if (mReadyTaps.capacity() < mTaps.size()) {
      mReadyTaps.reserve(mTaps.size());
    }
    for (auto& tapPtr: mTaps) {
      const auto& tap = tapPtr;
      if (tap == nullptr) {
        continue;
      }
      if (asynchronous && tap->pending.load(std::memory_order_acquire)) {
        // The IO thread has not delivered the previous buffer yet. The frames of this
        // pass are dropped instead of overwriting audio the client has not read.
        continue;
      }

      AudioFrameCount srcOffset = 0;
      while (srcOffset < frames) {
        const AudioFrameCount room = tap->bufferSize - tap->fillPos;
        const AudioFrameCount take = std::min(room, frames - srcOffset);
        if (tap->fillPos == 0) {
          tap->pendingPosition = tap->samplesSent;
        }
        if (AudioGraphUtils::copyPCMBuffer(source, srcOffset, *tap->tapBuffer, tap->fillPos, take) != take) {
          break;
        }
        tap->fillPos += take;
        srcOffset += take;

        if (tap->fillPos == tap->bufferSize) {
          tap->tapBuffer->setFrameLength(tap->bufferSize);
          if (asynchronous) {
            tap->pending.store(true, std::memory_order_release);
          } else {
            mReadyTaps.push_back(PendingTap{ tap, tap->pendingPosition });
          }
          tap->samplesSent += static_cast<int64_t>(tap->bufferSize);
          tap->fillPos = 0;
          if (asynchronous) {
            // One buffer per tap is in flight at a time.
            break;
          }
        }
      }
    }
  }

  // The blocks run outside of the lock because a tap block may install or remove taps.
  for (auto& ready: mReadyTaps) {
    const auto pending = ready.tap;
    pending->block(pending->tapBuffer, AudioTime(ready.position, format->getSampleRate(), 0));
  }
  mReadyTaps.clear();
}

void AudioNodeImpl::flushTaps() {
  if (!mHasTaps.load(std::memory_order_acquire)) {
    return;
  }

  {
    const std::lock_guard<std::mutex> guard(mTapsLock);
    // `mPendingTaps` belongs to the IO thread alone.
    if (mPendingTaps.capacity() < mTaps.size()) {
      mPendingTaps.reserve(mTaps.size());
    }
    for (auto& tapPtr: mTaps) {
      const auto& tap = tapPtr;
      if (tap == nullptr || !tap->pending.load(std::memory_order_acquire)) {
        continue;
      }
      mPendingTaps.push_back(PendingTap{ tap, tap->pendingPosition });
    }
  }

  for (auto& ready: mPendingTaps) {
    const auto pending = ready.tap;
    {
      static std::atomic_int sDeliveryCount{ 0 };
      if (sDeliveryCount.fetch_add(1, std::memory_order_relaxed) < 5) {
        MINILOG_DEBUG("Delivering a tap buffer of {} frames at position {}", pending->bufferSize,
          ready.position);
      }
    }
    // The block runs before `pending` is cleared, so the render thread cannot reuse the
    // buffer while the client is still reading it.
    pending->block(pending->tapBuffer, AudioTime(ready.position, pending->format->getSampleRate(), 0));
    pending->pending.store(false, std::memory_order_release);
  }
  mPendingTaps.clear();
}

std::string AudioNodeImpl::toString() const {
  std::stringstream out;

  out << "{\n";

  // print input buses information
  out << "inputs: [\n";
  for (const auto& [format, name, upstream]: mInputBuses) {
    if (upstream.valid()) {
      out << "{bus: " << name << ", format: " << format.value()
        << ", upstream: {" << upstream.node->getName() << ":" << upstream.bus << "}\n},\n";
    }
  }
  out << "],\n";

  // print output buses information
  out << "outputs: [\n";
  for (const auto& [format, name, connections]: mOutputBuses) {
    if (!connections.empty()) {
      out << "{bus: " << name << ", format: " << format.value() << ",\nconnections: [";
      for (const auto& [node, bus]: connections) {
        out << "\n{" << bus << ": " << node->getName() << "},";
      }
      out << "\n],\n},\n";
    }
  }
  out << "],\nlatency: " << mLatency
    << ",\noutputPresentationLatency: " << mOutputPresentationLatency
    << "\n}";

  return out.str();
}

//----------------------------------------------------------------------------------------------------------------------
void AudioNodeImpl::handleError(Error** outError, const ErrCode code, const char* errorMsg, const char* description) {
  if (outError != nullptr) {
    if (*outError == nullptr) {
      *outError = new Error();
    }
    (*outError)->code = code;
    (*outError)->failureReason = errorMsg;
    (*outError)->description = description;
#ifndef NDEBUG
    logError(code, errorMsg, description);
#endif
  } else {
    logError(code, errorMsg, description);
  }
}

void AudioNodeImpl::logError(const ErrCode code, const char* errMsg, const char* description) {
  MINILOG_ERROR("ErrCode: {}, msg: {}, description: {}", std::to_string(code), errMsg, description);
}

} // namespace coral
