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

#include "private/graph/AudioEngineImpl.h"

#include "private/graph/AudioGraphUtils.h"

#include <core/event/EventLooper.h>

#include <minilog/log.h>

#include <algorithm>
#include <ranges>

namespace coral {

namespace {

/**
 * Quits an event thread and disposes of it.
 *
 * A thread that quits from its own looper thread can only be detached, and the detached
 * thread goes on touching its `EventThread` object while it unwinds, so destroying that
 * object here would be a use after free. The case only arises when the engine is stopped
 * from inside a tap block, which runs on the IO thread.
 */
void disposeEventThread(std::unique_ptr<EventThread>& thread) {
  if (thread == nullptr) {
    return;
  }
  const auto& looper = thread->getLooper();
  const bool self = looper != nullptr && looper->isCurrentThread();
  thread->quit();
  if (self) {
    (void) thread.release();
  } else {
    thread.reset();
  }
}

/**
 * Reports a failure of an engine operation to the caller.
 *
 * @param outError the caller's error pointer, which keeps an error the caller owns.
 * @param message  the reason of the failure.
 */
void handleEngineError(Error** outError, const char* message) {
  MINILOG_ERROR("{}", message);
  if (outError == nullptr) {
    return;
  }
  if (*outError == nullptr) {
    *outError = new Error();
  }
  (*outError)->code = ErrCode::kInvalidArgument;
  (*outError)->failureReason = message;
}

} // namespace anonymous

void AudioEngineImpl::init() {
  // Nothing is allocated until prepare(): the hardware format of the input and output
  // nodes is only known once their streams are open.
}

void AudioEngineImpl::shutdown() {
  stopIOThread();
  stopRenderThread();
  AudioEngine::shutdown();
}

void AudioEngineImpl::attachNode(const std::shared_ptr<AudioNode>& node) {
  if (!node) {
    return;
  }
  const std::lock_guard<std::mutex> guard(mNodesLock);
  if (std::ranges::find(mAttachedNodes, node) == mAttachedNodes.end()) {
    node->attachToEngine(shared_from_this());
    mAttachedNodes.push_back(node);
  }
}

void AudioEngineImpl::detachNode(const std::shared_ptr<AudioNode>& node) {
  if (!node) {
    return;
  }
  if (!isNodeAttached(node)) {
    return;
  }

  // If the audio node is connected to AudioEngine, disconnect all input and output nodes associated with this node.
  // This happens outside of `mNodesLock`, because the disconnect helpers take it themselves.
  disconnectNodeInput(node);
  disconnectNodeOutput(node);
  node->detachFromEngine();

  const std::lock_guard<std::mutex> guard(mNodesLock);
  mAttachedNodes.erase(std::ranges::remove(mAttachedNodes, node).begin(), mAttachedNodes.end());
}

void AudioEngineImpl::connect(const std::shared_ptr<AudioNode>& sourceNode,
  const std::vector<AudioConnectionPoint>& destNodes,
  const AudioNodeBus sourceBus,
  const std::optional<AudioFormat>& format) {
  if (!sourceNode) {
    MINILOG_ERROR("Failed to connect: source node is null");
    return;
  }
  if (isRunning()) {
    MINILOG_ERROR("Failed to connect audio node when the AudioEngine is running");
    return;
  }
  // Checks if the audio nodes whether is attached to the AudioEngine
  if (!isNodeAttached(sourceNode)) {
    return;
  }

  // Connects multi nodes to output bus
  for (const auto& point: destNodes) {
    auto dst = point.node();
    if (!dst) {
      continue;
    }
    if (dst.get() == sourceNode.get()) {
      MINILOG_ERROR("Failed to connect: self-loop not allowed");
      continue;
    }
    if (!dst->setInputConnection(point.bus(), sourceNode, sourceBus, format)) {
      MINILOG_ERROR("Failed to connect: destination input bus out of range ({}:{})", dst->getName(), point.bus());
      continue;
    }
    sourceNode->setOutputConnection(sourceBus, dst.get(), point.bus());
  }
}

void AudioEngineImpl::disconnectNodeInput(const std::shared_ptr<AudioNode>& node, const AudioNodeBus bus) {
  if (!node || !isNodeAttached(node)) {
    return;
  }
  if (isRunning()) {
    MINILOG_ERROR("Failed to disconnect audio node when the AudioEngine is running");
    return;
  }
  node->clearInputConnection(bus);
}

void AudioEngineImpl::disconnectNodeInput(const std::shared_ptr<AudioNode>& node) {
  if (!node || !isNodeAttached(node)) {
    return;
  }
  if (isRunning()) {
    MINILOG_ERROR("Failed to disconnect audio node when the AudioEngine is running");
    return;
  }
  const auto numInputs = node->numberOfInputs();
  for (auto bus = 0; bus < numInputs; ++bus) {
    node->clearInputConnection(bus);
  }
}

void AudioEngineImpl::disconnectNodeOutput(const std::shared_ptr<AudioNode>& node, const AudioNodeBus bus) {
  if (!node || !isNodeAttached(node)) {
    return;
  }
  if (isRunning()) {
    MINILOG_ERROR("Failed to disconnect audio node when the AudioEngine is running");
    return;
  }
  node->clearOutputConnection(bus);
}

void AudioEngineImpl::disconnectNodeOutput(const std::shared_ptr<AudioNode>& node) {
  if (!node || !isNodeAttached(node)) {
    return;
  }
  if (isRunning()) {
    MINILOG_ERROR("Failed to disconnect audio node when the AudioEngine is running");
    return;
  }
  const auto numOutputs = node->numberOfOutputs();
  for (auto bus = 0; bus < numOutputs; ++bus) {
    node->clearOutputConnection(bus);
  }
}

std::optional<AudioConnectionPoint>
AudioEngineImpl::inputConnectionPointForNode(const std::shared_ptr<AudioNode>& node, const AudioNodeBus bus) {
  if (!node || !isNodeAttached(node) || bus >= node->numberOfInputs()) {
    return std::nullopt;
  }
  const auto& source = node->impl->mInputBuses[bus].upstream;
  if (!source.valid()) {
    return std::nullopt;
  }
  return AudioConnectionPoint(source.node->shared_from_this(), source.bus);
}

std::vector<AudioConnectionPoint>
AudioEngineImpl::outputConnectionPointsForNode(const std::shared_ptr<AudioNode>& node, const AudioNodeBus bus) {
  std::vector<AudioConnectionPoint> points;
  if (!node || !isNodeAttached(node) || bus >= node->numberOfOutputs()) {
    return points;
  }
  const auto& connections = node->impl->mOutputBuses[bus].connections;
  points.reserve(connections.size());
  for (const auto& [targetNode, targetBus]: connections) {
    if (targetNode) {
      points.emplace_back(targetNode->shared_from_this(), targetBus);
    }
  }
  return points;
}

AudioEngine::ManualRenderingStatus AudioEngineImpl::renderOffline(const AudioFrameCount numberOfFrames,
  std::unique_ptr<AudioPCMBuffer>& toBuffer,
  Error** outError) {
  if (!isInManualRenderingMode()) {
    handleEngineError(outError, "Failed to render offline: the AudioEngine is not in manual rendering mode");
    return ManualRenderingStatus::Error;
  }
  if (mManualRenderingMode != ManualRenderingMode::Offline) {
    handleEngineError(outError,
      "Failed to render offline: the AudioEngine renders in the realtime manual rendering mode");
    return ManualRenderingStatus::Error;
  }
  if (!isRunning()) {
    handleEngineError(outError, "Failed to render offline: the AudioEngine is not running");
    return ManualRenderingStatus::Error;
  }
  if (numberOfFrames == 0 || numberOfFrames > mManualRenderingMaximumFrameCount) {
    handleEngineError(
      outError, "Failed to render offline: the number of frames exceeds the manual rendering maximum frame count");
    return ManualRenderingStatus::Error;
  }
  if (toBuffer == nullptr || !toBuffer->getFormat().has_value() || !toBuffer->getFormat()->isValid()) {
    handleEngineError(outError, "Failed to render offline: the destination buffer has no valid format");
    return ManualRenderingStatus::Error;
  }
  if (mManualRenderingFormat.has_value() && toBuffer->getFormat() != mManualRenderingFormat) {
    handleEngineError(outError,
      "Failed to render offline: the destination buffer format does not match the render format");
    return ManualRenderingStatus::Error;
  }

  // Manual rendering always renders the singleton output node, which pulls the graph. A graph
  // without an output node can still be rendered when it ends in a sink node: the sink chains
  // are rendered by the engine here, exactly as the output node does in device rendering.
  const auto root = mOutputNode.get();
  if (root == nullptr && mSinkNodes.empty()) {
    handleEngineError(outError, "Failed to render offline: the AudioEngine has no output node and no sink chain");
    return ManualRenderingStatus::Error;
  }

  // The engine's timeline advances by the requested frames, whether or not the graph
  // produced all of them. The block that is being rendered starts at the current position.
  const auto sampleRate = mManualRenderingFormat.has_value() ? mManualRenderingFormat->getSampleRate() : 0;
  const auto when = AudioTime(mManualRenderingSampleTime, sampleRate, 0);
  mManualRenderingSampleTime += numberOfFrames;

  const auto epoch = ++mRenderEpoch;
  const auto* rendered = root != nullptr ? root->process(epoch, numberOfFrames, when) : nullptr;
  renderSinkChains(epoch, numberOfFrames, when);
  if (rendered == nullptr) {
    AudioGraphUtils::silencePCMBuffer(*toBuffer, numberOfFrames);
    toBuffer->setFrameLength(0);
    return ManualRenderingStatus::CannotDoInCurrentContext;
  }

  const auto frames = AudioGraphUtils::copyPCMBuffer(*rendered, 0, *toBuffer, 0, numberOfFrames);
  toBuffer->setFrameLength(frames);
  return frames == numberOfFrames ? ManualRenderingStatus::Success : ManualRenderingStatus::CannotDoInCurrentContext;
}

void AudioEngineImpl::onPrepare() {
  const auto manualRendering = isInManualRenderingMode();
  if (manualRendering) {
    // Manual rendering always routes the graph through the singleton I/O nodes.
    mayPrepareInputNode();
    mayPrepareOutputNode();
  }

  auto maxFrames = manualRendering ? std::max(mManualRenderingMaximumFrameCount, kDefaultNumberFrames)
                                   : kDefaultNumberFrames;
  const auto nodes = attachedNodesSnapshot();
  for (const auto& node: nodes) {
    node->impl->setMaxRenderFrames(maxFrames);
  }

  // The I/O nodes own the hardware format, so they are prepared first: the streams they
  // open decide how many frames a render call can contain.
  if (mInputNode) {
    mInputNode->prepareBeforeRendering();
  }
  if (mOutputNode) {
    mOutputNode->prepareBeforeRendering();
  }
  maxFrames = resolveMaxRenderFrames(maxFrames);

  // The output node has learned the format of the device by now, and that is the format the
  // graph renders in. Connections that were made before the device was opened still carry the
  // format they were made with, so they are brought in line before the nodes allocate their
  // render buffers.
  applyRenderFormatToGraph();

  for (const auto& node: nodes) {
    node->impl->setMaxRenderFrames(maxFrames);
  }

  // A time effect at a rate above 1.0 consumes more input frames than the block it renders,
  // so everything upstream of it has to be able to render that larger block. This runs before
  // the nodes allocate their render buffers.
  propagateInputFrameRequirements();

  for (const auto& node: nodes) {
    if (node == mInputNode || node == mOutputNode) {
      // Their streams are already open: only the render buffer has to grow to the block
      // size the whole graph agreed on.
      node->impl->prepareRenderBuffer();
      continue;
    }
    node->prepareBeforeRendering();
  }

  // The input chains that end in a sink node have to be rendered by the engine, because
  // pulling the output node never reaches them.
  resolveSinkChains();

  AudioEngine::onPrepare();
}

std::optional<Error> AudioEngineImpl::onStart() {
  if (isRunning()) {
    MINILOG_DEBUG("The AudioEngine is already running");
    return std::nullopt;
  }

  // Manual rendering never touches an audio device. Only its realtime mode needs a
  // thread: it pulls the graph on a dedicated render thread, while the offline mode
  // starts no thread at all and lets the client call `renderOffline`.
  if (isInManualRenderingMode()) {
    if (mManualRenderingMode == ManualRenderingMode::Realtime) {
      startRenderThread();
    }
    return AudioEngine::onStart();
  }

  // Realtime rendering: the driver threads of the input and output nodes schedule the
  // graph, and the engine's IO thread delivers the tap blocks.
  startIOThread();

  if (mOutputNode == nullptr && !mSinkNodes.empty()) {
    // A chain that ends in a sink node is rendered by the output node's render pass. Without
    // an output node there is no device pulling the graph, so the engine drives those chains
    // itself on its render thread, paced by the clock of the chain's own format.
    const auto format = mSinkNodes.front()->inputFormatForBus(0);
    if (format == std::nullopt || !format->isValid() || format->getSampleRate() <= 0) {
      MINILOG_ERROR("The engine has {} chain(s) that end in a sink node but no output node, and the "
                    "format of the chain is unknown: nothing renders them.", mSinkNodes.size());
    } else {
      mManualRenderingFormat = format;
      mManualRenderingMaximumFrameCount = std::max(mManualRenderingMaximumFrameCount, kDefaultNumberFrames);
      startRenderThread();
      MINILOG_DEBUG("The engine drives {} chain(s) that end in a sink node on its render thread",
        mSinkNodes.size());
    }
  }

  if (mInputNode && !mInputNode->getImpl()->start()) {
    MINILOG_ERROR("Failed to start the AudioInputNode");
    stopIOThread();
    return Error(ErrCode::kNotInit, "Failed to start the AudioInputNode");
  }
  if (mOutputNode && !mOutputNode->getImpl()->start()) {
    MINILOG_ERROR("Failed to start the AudioOutputNode");
    if (mInputNode) {
      mInputNode->getImpl()->stop();
    }
    stopIOThread();
    return Error(ErrCode::kNotInit, "Failed to start the AudioOutputNode");
  }

  return AudioEngine::onStart();
}

void AudioEngineImpl::onPause() {
  if (mInputNode) {
    mInputNode->getImpl()->pause();
  }
  if (mOutputNode) {
    mOutputNode->getImpl()->pause();
  }
  if (isInManualRenderingMode() && mManualRenderingMode == ManualRenderingMode::Realtime) {
    stopRenderThread();
  }
  AudioEngine::onPause();
}

void AudioEngineImpl::onStop() {
  // Stops capturing or rendering audio.
  if (mInputNode) {
    mInputNode->getImpl()->stop();
  }
  if (mOutputNode) {
    mOutputNode->getImpl()->stop();
  }
  stopIOThread();
  stopRenderThread();
  mRenderEpoch = 0;
  AudioEngine::onStop();
}

void AudioEngineImpl::onReset() {
  // reset all AudioNode attached to the engine, keeping the graph connections.
  for (const auto& node: attachedNodesSnapshot()) {
    node->reset();
  }
  mRenderEpoch = 0;
  AudioEngine::onReset();
}

void AudioEngineImpl::mayPrepareInputNode() {
  if (mInputNode) {
    return;
  }
  mInputNode = std::shared_ptr<AudioInputNode>(new AudioInputNode());
  attachNode(mInputNode);

  // In manual rendering mode the node captures in the engine's render format; the client
  // installs the matching input block with
  // AudioInputNode::setManualRenderingInputPCMFormat.
  if (isInManualRenderingMode() && mManualRenderingFormat.has_value()) {
    mInputNode->getImpl()->setManualRenderingFormat(mManualRenderingFormat);
  }
}

void AudioEngineImpl::mayPrepareOutputNode() {
  if (mOutputNode) {
    return;
  }
  mOutputNode = std::shared_ptr<AudioOutputNode>(new AudioOutputNode());
  attachNode(mOutputNode);

  // In manual rendering mode the node renders in the engine's render format.
  if (isInManualRenderingMode() && mManualRenderingFormat.has_value()) {
    mOutputNode->getImpl()->setOutputFormat(mManualRenderingFormat.value());
  }
}

void AudioEngineImpl::mayPrepareMixerNode() {
  if (mMainMixerNode) {
    return;
  }
  mMainMixerNode = std::make_shared<AudioMixerNode>();
  attachNode(mMainMixerNode);

  // The engine will construct a singleton main mixer and connect it to the outputNode
  // on demand, when this property is first accessed.
  const auto outNode = outputNode();
  // The mixer feeds the input scope of the output node, which is the float format of the
  // graph: the output node converts to the device or render format itself.
  auto format = outNode->inputFormatForBus(0);

  // In manual rendering mode, prioritize the user-specified format.
  if (isInManualRenderingMode() && mManualRenderingFormat.has_value()) {
    if (const auto graphFormat = AudioGraphUtils::standardFormat(mManualRenderingFormat.value());
      graphFormat.has_value()) {
      format = graphFormat.value();
    }
  }
  AudioEngine::connect(mMainMixerNode, outNode, format);
}

bool AudioEngineImpl::handleManualRenderingMode() {
  if (mStatus.load(std::memory_order_relaxed) != Status::kStatusStopped) {
    MINILOG_ERROR("Failed to enable manual rendering mode: the AudioEngine is not in a stopped state");
    return false;
  }
  if (mManualRenderingFormat.has_value() && !mManualRenderingFormat->isValid()) {
    MINILOG_ERROR("Failed to enable manual rendering mode: the render format is invalid");
    return false;
  }
  if (mManualRenderingMaximumFrameCount == 0) {
    MINILOG_ERROR("Failed to enable manual rendering mode: the maximum frame count must be greater than zero");
    return false;
  }

  // Manual rendering always routes the graph through the singleton I/O nodes.
  mayPrepareInputNode();
  mayPrepareOutputNode();

  // 1. Switches the input and output nodes to manual rendering mode.
  if (mInputNode && mManualRenderingFormat.has_value()) {
    mInputNode->getImpl()->setManualRenderingFormat(mManualRenderingFormat);
  }
  if (mOutputNode && mManualRenderingFormat.has_value()) {
    mOutputNode->getImpl()->setOutputFormat(mManualRenderingFormat.value());
  }

  // 2. Removes any taps previously installed on the input and output nodes.
  if (mInputNode) {
    mInputNode->removeTapOnBus(0);
  }
  if (mOutputNode) {
    mOutputNode->removeTapOnBus(0);
  }
  return true;
}

std::vector<AudioNode*> AudioEngineImpl::topologicalOrder() const {
  const auto nodes = attachedNodesSnapshot();

  std::unordered_map<AudioNode*, uint32_t> inDeg;
  inDeg.reserve(nodes.size());
  for (auto& node: nodes) {
    inDeg[node.get()] = 0;
  }
  for (auto& node: nodes) {
    for (auto bus = 0; bus < node->numberOfOutputs(); ++bus) {
      for (const auto& [targetNode, targetBus]: node->impl->mOutputBuses[bus].connections) {
        if (targetNode) {
          inDeg[targetNode]++;
        }
      }
    }
  }
  std::vector<AudioNode*> q, order;
  order.reserve(nodes.size());
  for (auto& [node, d]: inDeg) {
    if (d == 0) {
      q.push_back(node);
    }
  }
  while (!q.empty()) {
    auto* node = q.back();
    q.pop_back();
    order.push_back(node);
    for (auto bus = 0; bus < node->numberOfOutputs(); ++bus) {
      for (const auto& [targetNode, targetBus]: node->impl->mOutputBuses[bus].connections) {
        if (targetNode && --inDeg[targetNode] == 0) {
          q.push_back(targetNode);
        }
      }
    }
  }
  return order;
}

bool AudioEngineImpl::isNodeAttached(const std::shared_ptr<AudioNode>& node) const {
  const std::lock_guard<std::mutex> guard(mNodesLock);
  return std::ranges::find(mAttachedNodes, node) != mAttachedNodes.end();
}

std::vector<std::shared_ptr<AudioNode>> AudioEngineImpl::attachedNodesSnapshot() const {
  const std::lock_guard<std::mutex> guard(mNodesLock);
  return mAttachedNodes;
}

AudioFrameCount AudioEngineImpl::resolveMaxRenderFrames(const AudioFrameCount fallback) const {
  auto maxFrames = fallback;
  if (mInputNode) {
    maxFrames = std::max(maxFrames, mInputNode->getImpl()->getMaxRenderFrames());
  }
  if (mOutputNode) {
    maxFrames = std::max(maxFrames, mOutputNode->getImpl()->getMaxRenderFrames());
  }
  return maxFrames;
}

void AudioEngineImpl::applyRenderFormatToGraph() {
  if (mOutputNode == nullptr) {
    return;
  }
  const auto graphFormat = mOutputNode->impl->renderFormat();
  if (!graphFormat.has_value() || !graphFormat->isValid()) {
    return;
  }

  for (const auto& node: attachedNodesSnapshot()) {
    for (auto bus = 0; bus < node->numberOfInputs(); ++bus) {
      auto& input = node->impl->mInputBuses[bus];
      if (!input.upstream.valid()) {
        continue;
      }

      auto* const source = input.upstream.node;
      const auto sourceBus = input.upstream.bus;
      if (source == mInputNode.get()) {
        // The capture format is what the input node produces, and nothing converts between a
        // capture rate and a render rate yet, so the connection keeps it and a mismatch is
        // reported rather than silently applied.
        if (input.format != graphFormat) {
          MINILOG_WARN("The input node produces {}, which differs from the render format {} of the device",
            input.format.has_value() ? input.format->toString() : "an unknown format", graphFormat->toString());
        }
        continue;
      }

      if (input.format != graphFormat) {
        MINILOG_INFO("The connection into '{}' bus {} renders in the format of the device: {}",
          node->getName(), bus, graphFormat->toString());
      }
      input.format = graphFormat;
      if (sourceBus < source->numberOfOutputs()) {
        source->impl->mOutputBuses[sourceBus].format = graphFormat;
      }
    }
  }
}

void AudioEngineImpl::propagateInputFrameRequirements() {
  const auto nodes = attachedNodesSnapshot();
  if (nodes.empty()) {
    return;
  }

  std::unordered_map<AudioNode*, AudioFrameCount> required;
  required.reserve(nodes.size());
  for (const auto& node: nodes) {
    required.emplace(node.get(), node->impl->getMaxRenderFrames());
  }

  // The topological order lists the sources before their consumers, so walking it backwards
  // sizes every node from the consumers that pull it.
  const auto order = topologicalOrder();
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    auto* const node = *it;
    const auto nodeFrames = required[node];
    for (size_t bus = 0; bus < node->numberOfInputs(); ++bus) {
      const auto& input = node->impl->mInputBuses[bus];
      if (!input.upstream.valid()) {
        continue;
      }
      auto* const source = input.upstream.node;
      const auto wanted = node->impl->maxInputFramesForBlock(nodeFrames);
      const auto found = required.find(source);
      if (found != required.end() && wanted > found->second) {
        found->second = wanted;
      }
    }
  }

  for (const auto& node: nodes) {
    const auto found = required.find(node.get());
    if (found != required.end()) {
      node->impl->setMaxRenderFrames(found->second);
    }
  }
}

std::unordered_set<AudioNode*> AudioEngineImpl::upstreamClosure(AudioNode* root) const {
  std::unordered_set<AudioNode*> nodes;
  if (root == nullptr) {
    return nodes;
  }

  std::vector<AudioNode*> pending{ root };
  while (!pending.empty()) {
    auto* node = pending.back();
    pending.pop_back();
    if (!nodes.insert(node).second) {
      continue;
    }
    for (auto bus = 0; bus < node->numberOfInputs(); ++bus) {
      const auto& upstream = node->impl->mInputBuses[bus].upstream;
      if (upstream.valid()) {
        pending.push_back(upstream.node);
      }
    }
  }
  return nodes;
}

void AudioEngineImpl::resolveSinkChains() {
  mSinkNodes.clear();

  const auto nodes = attachedNodesSnapshot();
  auto* outputNode = mOutputNode.get();
  std::unordered_set<AudioNode*> outputChain;
  if (outputNode != nullptr) {
    outputChain = upstreamClosure(outputNode);
  }

  for (const auto& node: nodes) {
    if (node.get() == outputNode || node->numberOfOutputs() != 0) {
      continue;
    }
    if (node->impl->asSinkImpl() == nullptr) {
      // Only a sink is documented as the end of an input chain, so nothing else is pulled as
      // a chain root.
      continue;
    }

    if (outputNode != nullptr) {
      // A chain that shares a node with the output chain cannot be rendered from both roots,
      // because the shared node would be processed twice, on two threads.
      const auto chain = upstreamClosure(node.get());
      const bool shared = std::ranges::any_of(chain, [&outputChain](AudioNode* upstream) {
        return outputChain.contains(upstream);
      });
      if (shared) {
        MINILOG_ERROR("The sink node '{}' shares its input chain with the output chain, so it is not rendered",
          node->getName());
        continue;
      }
    }

    mSinkNodes.push_back(node);
  }

  if (!mSinkNodes.empty()) {
    MINILOG_DEBUG("The engine renders {} input chain(s) that end in a sink node", mSinkNodes.size());
  }
}

void AudioEngineImpl::renderSinkChains(const uint64_t epoch,
  const AudioFrameCount frames,
  const AudioTime& when) {
  for (const auto& node: mSinkNodes) {
    (void) node->process(epoch, frames, when);
  }
}

void AudioEngineImpl::startIOThread() {
  if (mIOThread != nullptr && mIOThread->isAlive()) {
    return;
  }

  mIOThread = std::make_unique<EventThread>("AudioEngine.IOThread");
  mIOThread->start();
  if (mIOThread->getLooper() == nullptr) {
    MINILOG_ERROR("Failed to start the AudioEngine IO thread");
    mIOThread.reset();
    return;
  }

  mIOHandler = EventHandler::create(mIOThread->getLooper(), nullptr);
  // Only now may a render thread publish a tap instead of calling the block itself.
  mTapDispatchAsynchronous.store(true, std::memory_order_release);
  scheduleIOTick();
  MINILOG_DEBUG("The IO thread of the engine is running and has been asked for a tick");
}

void AudioEngineImpl::stopIOThread() {
  if (mIOThread != nullptr) {
    MINILOG_DEBUG("Stopping the IO thread of the engine");
  }
  mTapDispatchAsynchronous.store(false, std::memory_order_release);
  if (mIOHandler != nullptr) {
    mIOHandler->removeAllCallbacksAndMessages();
    mIOHandler.reset();
  }
  mIOTickScheduled.store(false, std::memory_order_release);
  disposeEventThread(mIOThread);
}

void AudioEngineImpl::scheduleIOTick() {
  if (mIOHandler == nullptr) {
    return;
  }
  if (mIOTickScheduled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  const auto handler = mIOHandler;
  handler->postDelay([this] { onIOTick(); }, std::chrono::milliseconds(kIOTickIntervalMs));
}

void AudioEngineImpl::onIOTick() {
  mIOTickScheduled.store(false, std::memory_order_release);
  {
    static std::atomic_int sTickCount{ 0 };
    if (sTickCount.fetch_add(1, std::memory_order_relaxed) < 5) {
      MINILOG_DEBUG("The IO thread of the engine ticks and flushes the taps");
    }
  }
  flushNodeTaps();

  // The tick reposts itself for as long as the IO thread is running. A tap buffer is
  // published by the render thread with a single atomic store, so the delivery has to be
  // polled: posting from an audio thread would take a lock and allocate.
  if (mTapDispatchAsynchronous.load(std::memory_order_acquire) && mIOHandler != nullptr) {
    scheduleIOTick();
  }
}

void AudioEngineImpl::flushNodeTaps() {
  for (const auto& node: attachedNodesSnapshot()) {
    node->impl->flushTaps();
  }
}

void AudioEngineImpl::startRenderThread() {
  if (mRenderThread != nullptr && mRenderThread->isAlive()) {
    return;
  }

  mRenderThread = std::make_unique<EventThread>("AudioEngine.RenderThread");
  mRenderThread->start();
  if (mRenderThread->getLooper() == nullptr) {
    MINILOG_ERROR("Failed to start the AudioEngine render thread");
    mRenderThread.reset();
    return;
  }

  mRenderHandler = EventHandler::create(mRenderThread->getLooper(), nullptr);
  mRenderEpoch = 0;
  mManualRenderingSampleTime = 0;
  mRenderLoopStart = std::chrono::steady_clock::now();
  mRenderLoopActive.store(true, std::memory_order_release);
  mRenderHandler->post([this] { onRenderTick(); });
}

void AudioEngineImpl::stopRenderThread() {
  mRenderLoopActive.store(false, std::memory_order_release);
  if (mRenderHandler != nullptr) {
    mRenderHandler->removeAllCallbacksAndMessages();
    mRenderHandler.reset();
  }
  disposeEventThread(mRenderThread);
}

void AudioEngineImpl::onRenderTick() {
  if (!mRenderLoopActive.load(std::memory_order_acquire) || mRenderHandler == nullptr) {
    return;
  }

  const auto frames = std::max(mManualRenderingMaximumFrameCount, 1u);
  const auto sampleRate = mManualRenderingFormat.has_value() ? mManualRenderingFormat->getSampleRate() : 0;
  const auto when = AudioTime(mManualRenderingSampleTime, sampleRate, 0);

  // The output node pulls the graph, and every tap of every node it reaches is delivered
  // synchronously on this thread. A graph that ends in a sink node is rendered here as well,
  // so a chain into a sink is driven even when the engine has no output node.
  const auto epoch = ++mRenderEpoch;
  if (mOutputNode) {
    (void) mOutputNode->process(epoch, frames, when);
  }
  renderSinkChains(epoch, frames, when);
  mManualRenderingSampleTime += frames;

  if (sampleRate <= 0) {
    mRenderHandler->post([this] { onRenderTick(); });
    return;
  }

  // Pace the next block to the sample clock, so that the dedicated render thread runs in
  // realtime instead of as fast as the CPU allows.
  const auto elapsedNanos =
    static_cast<int64_t>(static_cast<double>(mManualRenderingSampleTime) / sampleRate * Definitions::kNanosPerSecond);
  const auto targetTime = mRenderLoopStart + TimeDelta(elapsedNanos);
  const auto handler = mRenderHandler;
  handler->postAtTime([this] { onRenderTick(); }, targetTime);
}

} // namespace coral
