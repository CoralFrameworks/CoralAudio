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

#ifndef CORAL_AUDIO_GRAPH_ENGINE_H
#define CORAL_AUDIO_GRAPH_ENGINE_H

#include "private/graph/AudioInputNodeImpl.h"
#include "private/graph/AudioMixerNodeImpl.h"
#include "private/graph/AudioOutputNodeImpl.h"

#include <coral/audio/AudioEngine.h>

#include <core/event/EventHandler.h>
#include <core/event/EventThread.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

namespace coral {

class AudioFormat;
class AudioNode;
class AudioInputNode;
class AudioMixerNode;
class AudioOutputNode;

/**
 * The implementation of the AudioEngine.
 *
 * @discussion
 * The engine has three ways of moving audio through the graph:
 *
 * 1. Realtime rendering, the default: the input and output nodes are scheduled by the
 *    audio driver threads of their `AudioStream`s, and the engine's own IO thread
 *    delivers `AudioNode::TapBlock` so that a client block never runs on an audio
 *    thread.
 * 2. Manual rendering, offline: the client calls `renderOffline` itself and the engine
 *    owns no thread at all.
 * 3. Manual rendering, realtime: a dedicated render thread pulls the graph at the
 *    render format's sample rate. Tap blocks are delivered directly by that thread.
 *
 * The IO thread and the render thread are both `EventThread`s, so all of their work is
 * posted and executed through an `EventHandler`.
 */
class AudioEngineImpl final : public AudioEngine,
                              public std::enable_shared_from_this<AudioEngineImpl> {
public:
  void init();

  void attachNode(const std::shared_ptr<AudioNode>& node) override;

  void detachNode(const std::shared_ptr<AudioNode>& node) override;

  void connect(const std::shared_ptr<AudioNode>& sourceNode,
    const std::vector<AudioConnectionPoint>& destNodes,
    AudioNodeBus sourceBus,
    const std::optional<AudioFormat>& format) override;

  void disconnectNodeInput(const std::shared_ptr<AudioNode>& node, AudioNodeBus bus) override;

  void disconnectNodeInput(const std::shared_ptr<AudioNode>& node) override;

  void disconnectNodeOutput(const std::shared_ptr<AudioNode>& node, AudioNodeBus bus) override;

  void disconnectNodeOutput(const std::shared_ptr<AudioNode>& node) override;

  std::optional<AudioConnectionPoint>
  inputConnectionPointForNode(const std::shared_ptr<AudioNode>& node, AudioNodeBus bus) override;

  std::vector<AudioConnectionPoint>
  outputConnectionPointsForNode(const std::shared_ptr<AudioNode>& node, AudioNodeBus bus) override;

  ManualRenderingStatus
  renderOffline(AudioFrameCount numberOfFrames, std::unique_ptr<AudioPCMBuffer>& toBuffer, Error** outError) override;

  /**
   * Whether a tap has to be delivered by the engine's IO thread instead of by the
   * thread that renders.
   *
   * @discussion True only while rendering to an audio device, where the render call runs
   * on an audio driver thread.
   */
  [[nodiscard]] bool isTapDispatchAsynchronous() const {
    return mTapDispatchAsynchronous.load(std::memory_order_acquire);
  }

  /**
   * Whether the engine has an output node, i.e. a render root that an audio device drives.
   */
  [[nodiscard]] bool hasOutputNode() const {
    return mOutputNode != nullptr;
  }

  /**
   * Renders the input chains that end in a node without an output bus.
   *
   * @param epoch  the render pass. It is shared with the chain rendered around this call, so
   *               that a node feeding both chains is only rendered once.
   * @param frames the number of frames to ask for.
   * @param when   the time of the first frame of this block in the render timeline, which is
   *               handed to every node the call reaches.
   *
   * @discussion
   * A node without an output bus, such as an `AudioSinkNode`, ends an input chain and is
   * therefore never reached by pulling the output node. The engine renders those chains from
   * its render root instead, so the client's receiver block runs on the same thread as the
   * rest of the graph.
   */
  void renderSinkChains(uint64_t epoch, AudioFrameCount frames, const AudioTime& when);

protected:
  void shutdown() override;

  void onPrepare() override;

  std::optional<Error> onStart() override;

  void onPause() override;

  void onStop() override;

  void onReset() override;

  void mayPrepareInputNode() override;

  void mayPrepareOutputNode() override;

  void mayPrepareMixerNode() override;

  bool handleManualRenderingMode() override;

  std::vector<AudioNode*> topologicalOrder() const override;

private:
  /**
   * Indicates whether the audio node is attached to the AudioEngine.
   */
  bool isNodeAttached(const std::shared_ptr<AudioNode>& node) const;

  /**
   * Returns a snapshot of the attached nodes, so that a thread other than the client
   * thread can iterate them without holding `mNodesLock`.
   */
  [[nodiscard]] std::vector<std::shared_ptr<AudioNode>> attachedNodesSnapshot() const;

  /**
   * The largest number of frames the engine may ask a node to render.
   */
  [[nodiscard]] AudioFrameCount resolveMaxRenderFrames(AudioFrameCount fallback) const;

  /**
   * Collects the input chains that end in a node without an output bus.
   *
   * @discussion Called from `onPrepare`, while the engine is stopped, so that the render root
   * can walk the list without taking a lock.
   */
  void resolveSinkChains();

  /**
   * Brings the graph connections in line with the format the output node renders in.
   *
   * @discussion
   * The graph renders in the format of the device (or of the manual rendering mode), which is
   * only known once the output node has opened its stream, that is after an application has
   * already made its connections. Called from `onPrepare`, before the nodes allocate their
   * render buffers, so that the whole graph agrees on one format.
   */
  void applyRenderFormatToGraph();

  /**
   * Sizes the nodes upstream of a node that consumes more frames than it produces.
   *
   * @discussion
   * A time effect at a rate above 1.0 asks its input for more frames than the block it renders,
   * so the render buffers of everything upstream of it have to be able to hold that larger
   * block. The requirement is propagated from the sinks back to the sources, before the nodes
   * allocate their render buffers.
   */
  void propagateInputFrameRequirements();

  /** Every node that feeds `root`, including `root` itself. */
  [[nodiscard]] std::unordered_set<AudioNode*> upstreamClosure(AudioNode* root) const;

  /** Starts the IO thread that delivers the tap blocks while rendering to a device. */
  void startIOThread();

  /** Stops and disposes of the IO thread. */
  void stopIOThread();

  /** Queues the next IO thread tick, if one is not already queued. */
  void scheduleIOTick();

  /** Runs on the IO thread: delivers published tap buffers and reschedules itself. */
  void onIOTick();

  /** Delivers the tap buffers of every attached node. */
  void flushNodeTaps();

  /** Starts the dedicated render thread of the realtime manual rendering mode. */
  void startRenderThread();

  /** Stops and disposes of the render thread. */
  void stopRenderThread();

  /** Runs on the render thread: pulls the graph and paces the next block. */
  void onRenderTick();

  static constexpr AudioFrameCount kDefaultNumberFrames = 1024;
  static constexpr int64_t kIOTickIntervalMs = 5;

  //!< Guards `mAttachedNodes`, which the IO thread reads while the client writes it.
  mutable std::mutex mNodesLock;

  //!< The IO thread that delivers AudioNode::TapBlock while rendering to a device.
  std::unique_ptr<EventThread> mIOThread;
  std::shared_ptr<EventHandler> mIOHandler;
  std::atomic_bool mIOTickScheduled{ false };
  std::atomic_bool mTapDispatchAsynchronous{ false };

  //!< The dedicated render thread of the realtime manual rendering mode.
  std::unique_ptr<EventThread> mRenderThread;
  std::shared_ptr<EventHandler> mRenderHandler;
  std::atomic_bool mRenderLoopActive{ false };
  //!< The host time the render thread's sample clock started at.
  TimePoint mRenderLoopStart;

  //!< The epoch of the current render pass in manual rendering mode.
  uint64_t mRenderEpoch = 0;

  /**
   * The nodes that end an input chain and have to be rendered by the engine's render root.
   *
   * @discussion Written by `onPrepare` while the engine is stopped and read by the render
   * root while it runs, which is the same lifetime rule the rest of the graph follows.
   */
  std::vector<std::shared_ptr<AudioNode>> mSinkNodes;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_ENGINE_H
