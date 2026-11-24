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

#ifndef CORAL_AUDIO_ENGINE_H
#define CORAL_AUDIO_ENGINE_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioConnectionPoint.h>
#include <coral/audio/AudioIONode.h>
#include <coral/audio/AudioMixerNode.h>

#include <memory>
#include <optional>
#include <vector>

namespace coral {

/**
 * An AudioEngine contains a group of connected AudioNodes ("nodes"), each of
 * which performs an audio signal generation, processing, or input/output task.
 * Nodes are created separately and attached to the engine.
 * The engine supports dynamic connection, disconnection and removal of nodes
 * while running, with only minor limitations:
 *   - all dynamic reconnections must occur upstream of a mixer
 *   - while removals of effects will normally result in the automatic
 * connection of the adjacent nodes, removal of a node which has differing input
 * vs. output channel counts, or which is a mixer, is likely to result in a
 * broken graph.
 *
 * By default, the engine is connected to an audio device and automatically
 * renders in realtime. It can also be configured to operate in manual rendering
 * mode, i.e. not connected to an audio device and rendering in response to
 * requests from the client, normally at or faster than realtime rate.
 */
class AudioEngine : public AudioNoncopyable {
public:
  /**
   * Creates a new instance of the AudioEngine
   */
  static std::shared_ptr<AudioEngine> Create();

  /**
   * Destroys the instance of the AudioEngine
   */
  static void Destroy(std::shared_ptr<AudioEngine> engine);

  /**
   * Take ownership of a new audio node.
   *
   * @param node The node to be attached to the engine.
   *
   * @discussion
   * To support the instantiation of arbitrary AudioNode subclasses, instances are created
   * externally to the engine, but are not usable until they are attached to the engine via
   * this method. Thus the idiom is:
   *
   * ```
   * auto player = AudioPlayerNode::Create();
   * engine->attachNode(player);
   * ...
   * // when destroying engine
   * player->release();
   * ```
   */
  virtual void attachNode(const std::shared_ptr<AudioNode>& node) = 0;

  /**
   * Detach an audio node previously attached to the engine.
   *
   * @param node The node to be detached to the engine.
   *
   * @discussion
   * If necessary, the engine will safely disconnect the node before detaching it.
   */
  virtual void detachNode(const std::shared_ptr<AudioNode>& node) = 0;

  /**
   * Establish a connection between two nodes.
   *
   * @param fromNode The source node
   * @param toNode   The destination node
   * @param fromBus  The output bus on the source node
   * @param toBus    The input bus on the destination node
   * @param format   If non-nil, the format of the source node's output bus is set to this
   *                 format. In all cases, the format of the destination node's input bus
   *                 is set to match that of the source node's output bus.
   *
   * @discussion
   * Nodes have input and output buses (AudioNodeBus). Use this method to
   * establish one-to-one connections between nodes. Connections made using this
   * method are always one-to-one, never one-to-many or many-to-one.
   *
   * Note that any pre-existing connection(s) involving the source's output bus or the
   * destination's input bus will be broken.
   */
  void connect(const std::shared_ptr<AudioNode>& fromNode,
    const std::shared_ptr<AudioNode>& toNode,
    const AudioNodeBus fromBus,
    const AudioNodeBus toBus,
    const std::optional<AudioFormat>& format) {
    connect(fromNode, std::vector<AudioConnectionPoint>{ { toNode, toBus } }, fromBus, format);
  }

  /**
   * Establish a connection between two nodes
   *
   * @discussion
   * This calls connect:fromNode:toNode:fromBus:toBus:format: using bus 0 on the source node,
   * and bus 0 on the destination node, except in the case of a destination which is a mixer,
   * in which case the destination is the mixer's nextAvailableInputBus.
   */
  void connect(const std::shared_ptr<AudioNode>& fromNode,
    const std::shared_ptr<AudioNode>& toNode,
    const std::optional<AudioFormat>& format) {
    constexpr AudioNodeBus fromBus = 0;
    AudioNodeBus toBus = 0;
    const auto mixerNode = std::dynamic_pointer_cast<AudioMixerNode>(toNode);
    if (mixerNode) {
      toBus = mixerNode->nextAvailableInputBus();
    }
    connect(fromNode, toNode, fromBus, toBus, format);
  }

  /**
   * Establish connections between a source node and multiple destination nodes.
   *
   * @param sourceNode The source node
   * @param destNodes  An array of AVAudioConnectionPoint objects specifying destination
                       nodes and busses
   * @param sourceBus  The output bus on source node
   * @param format     If non-nil, the format of the source node's output bus is set to this
   *                   format. In all cases, the format of the destination nodes' input bus is
   *                   set to match that of the source node's output bus
   *
   * @discussion
   * Use this method to establish connections from a source node to multiple destination nodes.
   * Connections made using this method are either one-to-one (when a single destination
   * connection is specified) or one-to-many (when multiple connections are specified), but
   * never many-to-one.
   *
   * To incrementally add a new connection to a source node, use this method with an array
   * of AudioConnectionPoint objects comprising of pre-existing connections (obtained from
   * `outputConnectionPointsForNode:outputBus:`) and the new connection.
   *
   * Note that any pre-existing connection involving the destination's input bus will be
   * broken. And, any pre-existing connection on source node which is not a part of the
   * specified destination connection array will also be broken.
   *
   * Also note that when the output of a node is split into multiple paths, all the paths
   * must render at the same rate until they reach a common mixer.
   * In other words, starting from the split node until the common mixer node where all split
   * paths terminate, you cannot have:
   *     - any AudioUnitTimeEffect
   *     - any sample rate conversion
   */
  virtual void connect(const std::shared_ptr<AudioNode>& sourceNode,
    const std::vector<AudioConnectionPoint>& destNodes,
    AudioNodeBus sourceBus,
    const std::optional<AudioFormat>& format) = 0;

  /**
   * Remove a connection between two nodes.
   *
   * @param node The node whose input is to be disconnected
   * @param bus The destination's input bus to disconnect
   */
  virtual void disconnectNodeInput(const std::shared_ptr<AudioNode>& node, AudioNodeBus bus) = 0;

  /**
   * Remove a connection between two nodes.
   *
   * @param node The node whose inputs are to be disconnected
   *
   * @discussion
   * Connections are broken on each of the node's input busses.
   */
  virtual void disconnectNodeInput(const std::shared_ptr<AudioNode>& node) = 0;

  /**
   * Remove a connection between two nodes.
   *
   * @param node The node whose output is to be disconnected
   * @param bus The source's output bus to disconnect
   */
  virtual void disconnectNodeOutput(const std::shared_ptr<AudioNode>& node, AudioNodeBus bus) = 0;

  /**
   * Remove a connection between two nodes.
   *
   * @param node The node whose outputs are to be disconnected
   *
   * @discussion
   * Connections are broken on each of the node's output busses.
   */
  virtual void disconnectNodeOutput(const std::shared_ptr<AudioNode>& node) = 0;

  /**
   * Prepare the engine for starting.
   *
   * @discussion
   * This method pre-allocates many of the resources the engine requires in order to start.
   * It can be used to be able to start more responsively.
   */
  void prepare();

  /**
   * Start the engine.
   *
   * @return std::nullopt for success
   *
   * @discussion
   * Calls prepare if it has not already been called since stop.
   *
   * When the engine is rendering to/from an audio device, starts the audio hardware via the
   * AudioInputNode and/or AudioOutputNode instances in the engine. Audio begins to flow through
   * the engine.
   * Reasons for potential failure to start in this mode include:
   * 1. There is problem in the structure of the graph. Input can't be routed to output or to a
   *    recording tap through converter type nodes.
   * 2. An AudioSession error.
   * 3. The driver failed to start the hardware.
   *
   * In manual rendering mode, prepares the engine to render when requested by the client.
   */
  std::optional<Error> start();

  /**
   * Pause the engine.
   *
   * @discussion
   * When the engine is rendering to/from an audio device, stops the audio hardware and the flow
   * of audio through the engine. When operating in this mode, it is recommended that the engine
   * be paused or stopped (as applicable) when not in use, to minimize power consumption.
   *
   * Pausing the engine does not deallocate the resources allocated by prepare. Resume the engine
   * by invoking start again.
   */
  void pause();

  /**
   * Reset all of the nodes in the engine.
   *
   * @discussion
   * This will reset all of the nodes in the engine. This is useful, for example, for silencing
   * reverb and delay tails.
   *
   * In manual rendering mode, the render time is reset to a sample time of zero.
   */
  void reset();

  /**
   * When the engine is rendering to/from an audio device, stops the audio hardware and the engine.
   * When operating in this mode, it is recommended that the engine be paused or stopped (as
   * applicable) when not in use, to minimize power consumption.
   *
   * Stopping the engine releases the resources allocated by prepare.
   */
  void stop();

  /**
   * Get connection information on a node's input bus.
   *
   * @param node The node whose input connection is being queried.
   * @param bus  The node's input bus on which the connection is being queried.
   *
   * @return An AudioConnectionPoint object with connection information on the node's
   *         specified input bus.
   *
   * @discussion
   * Connections are always one-to-one or one-to-many, never many-to-one.
   *
   * Returns nullptr if there is no connection on the node's specified input bus.
   */
  virtual std::optional<AudioConnectionPoint>
  inputConnectionPointForNode(const std::shared_ptr<AudioNode>& node, AudioNodeBus bus) = 0;

  /**
   * Get connection information on a node's output bus.
   *
   * @param node The node whose output connections are being queried.
   * @param bus  The node's output bus on which connections are being queried.
   *
   * @return An array of AudioConnectionPoint objects with connection information on the node's
   *         specified output bus.
   *
   * @discussion
   * Connections are always one-to-one or one-to-many, never many-to-one.
   *
   * Returns an empty array if there are no connections on the node's specified output bus.
   */
  virtual std::vector<AudioConnectionPoint>
  outputConnectionPointsForNode(const std::shared_ptr<AudioNode>& node, AudioNodeBus bus) = 0;

  /**
   * Returns the engine's singleton input node.
   *
   * @discussion
   * Audio input is performed via an input node. The engine creates a singleton on demand when
   * this property is first accessed. To receive input, connect another node from the output of
   * the input node, or create a recording tap on it.
   *
   * When the engine is rendering to/from an audio device, the AudioSession category and/or
   * availability of hardware determine whether an app can perform input (e.g. input hardware
   * is not available on tvos). Check for the input node's input format (i.e. hardware format)
   * for non-zero sample rate and channel count to see if input is enabled.
   * Trying to perform input through the input node when it is not enabled or available will
   * cause the engine to throw an error (when possible) or an exception.
   *
   * In manual rendering mode, the input node can be used to synchronously supply data to
   * the engine while it is rendering.
   */
  [[nodiscard]] const std::shared_ptr<AudioInputNode>& inputNode();

  /**
   * Returns the engine's singleton output node.
   *
   * @discussion
   * Audio output is performed via an output node. The engine creates a singleton on demand when
   * this property is first accessed. Connect another node to the input of the output node, or
   * obtain a mixer that is connected there by default, using the "mainMixerNode" property.
   *
   * When the engine is rendering to/from an audio device, the AudioSession category and/or
   * availability of hardware determine whether an app can perform output. Check the output format
   * of output node (i.e. hardware format) for non-zero sample rate and channel count to see if
   * output is enabled.
   * Trying to perform output through the output node when it is not enabled or available will
   * cause the engine to throw an error (when possible) or an exception.
   *
   * In manual rendering mode, the output format of the output node will determine the render
   * format of the engine.
   */
  [[nodiscard]] const std::shared_ptr<AudioOutputNode>& outputNode();

  /**
   * Returns the engine's optional singleton main mixer node.
   *
   * @discussion
   * The engine will construct a singleton main mixer and connect it to the outputNode on demand,
   * when this property is first accessed. You can then connect additional nodes to the mixer.
   *
   * By default, the mixer's output format (sample rate and channel count) will track the format
   * of the output node. You may however make the connection explicitly with a different format.
   */
  [[nodiscard]] const std::shared_ptr<AudioMixerNode>& mainMixerNode();

  /**
   * Returns the engine's running state.
   */
  [[nodiscard]] bool isRunning() const {
    return mStatus.load(std::memory_order_acquire) == Status::kStatusStarted;
  }

  //--------------------------------------------------------------------------------------------------------------------
  /**
   * By default, the engine is connected to an audio device and automatically
   * renders in realtime. It can also be configured to operate in manual rendering
   * mode, i.e. not connected to an audio device and rendering in response to
   * requests from the client.
   */
  enum class ManualRenderingMode : uint8_t {
    /**
     * The engine operates in an offline mode without any realtime constraints.
     */
    Offline = 0,

    /**
     * The engine operates under realtime constraints, i.e. it will not make any
     * blocking call (e.g. blocking on a mutex, allocating memory etc.) while
     * rendering. Note that only the block based render mechanism can be used in
     * this mode
     */
    Realtime = 1,
  };

  /**
   * Status codes returned from the render call to the engine operating in manual
   * rendering mode.
   */
  enum class ManualRenderingStatus : int8_t {
    /**
     * An error occurred when rendering and no data was returned. See the returned
     * error code for the description of the error.
     */
    Error = -1,

    /**
     * All of the requested data was returned successfully.
     */
    Success = 0,

    /**
     * Applicable only to the input node, when it provides input data for rendering (see
     * `AudioInputNode(setManualRenderingInputPCMFormat:inputBlock:)`).
     * Indicates that not enough input data was returned by the input node to satisfy the
     * render request at the current time. The output buffer may contain data rendered by
     * other active sources in the engine's processing graph.
     */
    InsufficientDataFromInputNode = 1,

    /**
     * The operation could not be performed now, but the client could retry later if needed.
     * This is usually to guard a realtime render operation (e.g. rendering through
     * `manualRenderingBlock()`) when a reconfiguration of the engine's internal state
     * is in progress.
     */
    CannotDoInCurrentContext = 2
  };

  // using ManualRenderingBlock =
  // std::function<ManualRenderingStatus>(AudioFrameCount numberOfFrames, AudioBufferList *outBuffer, Error *outError)>;

  /**
   * Set the engine to operate in a manual rendering mode with the specified render format and
   * maximum frame count.
   *
   * @param mode              The manual rendering mode to use.
   * @param pcmFormat         The format of the output PCM audio data from the engine.
   * @param maximumFrameCount The maximum number of PCM sample frames the engine will be asked
   *                          to produce in any single render call.
   * @param outError          On exit, if the engine cannot switch to the manual rendering mode,
   *                          a description of the error. (see `ManualRenderingError` for the
   *                          possible errors)
   * @return True for success.
   *
   * @discussion
   * Use this method to configure the engine to render in response to requests from the client.
   *
   * The engine must be in a stopped state before calling this method.
   * The render format must be a PCM format and match the format of the buffer to which
   * the engine is asked to render (see `renderOffline:toBuffer:error:`).
   *
   * The input data in manual rendering mode can be supplied through the source nodes, e.g.
   * `AudioPlayerNode`, `AudioInputNode` etc.
   *
   * When switching to manual rendering mode, the engine:
   * 1. Switches the input and output nodes to manual rendering mode. Their input and output
   *    formats may change.
   * 2. Removes any taps previously installed on the input and output nodes.
   * 3. Maintains all the engine connections as is.
   *
   * Reasons for potential failure when switching to manual rendering mode include:
   * - Engine is not in a stopped state.
   */
  bool enableManualRenderingMode(ManualRenderingMode mode,
    const std::optional<AudioFormat>& pcmFormat,
    AudioFrameCount maximumFrameCount,
    Error** outError);

  /**
   * Set the engine to render to/from an audio device.
   *
   * @discussion
   * When disabling the manual rendering mode, the engine:
   * 1. Stops and resets itself (see `stop` and `reset`).
   * 2. Switches the output/input nodes to render to/from an audio device. Their input and
   *    output formats may change.
   * 3. Removes any taps previously installed on the input and output nodes.
   * 4. Maintains all the engine connections as is.
   *
   * Calling this method when the engine is already rendering to/from an audio device has no
   * effect.
   */
  void disableManualRenderingMode();

  /**
   * Render call to the engine operating in the offline manual rendering mode
   *
   * @param numberOfFrames The number of PCM sample frames to be rendered
   * @param toBuffer       The PCM buffer to which the engine must render the audio
   * @param outError       On exit, if an error occurs during rendering, a description of the
   *                       error (see `ManualRenderingError` for the possible errors)
   * @return One of the status codes from `RenderingStatus`. Irrespective of the
   *         returned status code, on exit, the output buffer's frameLength will indicate the
   *         number of PCM samples rendered by the engine
   *
   * @discussion
   * The engine must be in the offline manual rendering mode (`ManualRenderingMode::kOffline`)
   * and started before calling this method.
   *
   * The format of the buffer must match the render format set through
   * `enableManualRenderingMode:format:maximumFrameCount:error:`. The buffer capacity must be
   * greater than or equal to the number of samples asked to render.
   * On exit, the buffer's frameLength will indicate the number of PCM samples rendered by the
   * engine.
   *
   * The engine's timeline in manual rendering mode starts at a sample time of zero, and is in
   * terms of the render format's sample rate. Resetting the engine (see `reset`) will reset the
   * time back to zero.
   *
   * When rendering in `ManualRenderingMode::kRealtime`, this render method must not be used,
   * an error is returned otherwise. Use the block based render call (`manualRenderingBlock`)
   * in that mode instead.
   */
  virtual ManualRenderingStatus renderOffline(AudioFrameCount numberOfFrames,
    std::unique_ptr<AudioPCMBuffer>& toBuffer,
    Error** outError);

  /**
   * Returns whether or not the engine is operating in manual rendering mode, i.e. not connected
   * to an audio device and rendering in response to the requests from the client
   */
  [[nodiscard]] bool isInManualRenderingMode() const {
    return mManualRenderingModeEnable.load(std::memory_order_relaxed);
  }

  /**
   * Returns the manual rendering mode configured on the engine
   *
   * @discussion
   * This property is meaningful only when the engine is operating in manual rendering mode,
   * i.e. when `isInManualRenderingMode` returns true.
   */
  [[nodiscard]] ManualRenderingMode manualRenderingMode() const {
    return mManualRenderingMode;
  }

  /**
   * Returns the render format of the engine in manual rendering mode.
   *
   * @discussion
   * Querying this property when the engine is not in manual rendering mode will return an
   * invalid format, with zero sample rate and channel count.
   */
  [[nodiscard]] std::optional<AudioFormat> manualRenderingFormat() const {
    return mManualRenderingFormat;
  }

  /**
   * Returns the maximum number of PCM sample frames the engine can produce in any single
   * render call in the manual rendering mode.
   *
   * @discussion
   * Querying this property when the engine is not in manual rendering mode will return zero.
   */
  [[nodiscard]] AudioFrameCount manualRenderingMaximumFrameCount() const {
    return mManualRenderingMaximumFrameCount;
  }

  /**
   * Indicates where the engine is on its render time in manual rendering mode.
   *
   * @discussion
   * The timeline in manual rendering mode starts at a sample time of zero, and is in terms
   * of the render format's sample rate. Resetting the engine (see `reset`) will reset the
   * timeline back to zero.
   */
  [[nodiscard]] AudioFramePosition manualRenderingSampleTime() const {
    return mManualRenderingSampleTime;
  }

  std::string toString() const;

protected:
  AudioEngine();

  virtual void shutdown();

  virtual void onPrepare();

  virtual std::optional<Error> onStart();

  virtual void onPause();

  virtual void onStop();

  virtual void onReset();

  virtual void mayPrepareInputNode() = 0;

  virtual void mayPrepareOutputNode() = 0;

  virtual void mayPrepareMixerNode() = 0;

  virtual bool handleManualRenderingMode() = 0;

  virtual std::vector<AudioNode*> topologicalOrder() const = 0;

  static std::string getName() {
    return "AudioEngine";
  }

  enum class Status : uint8_t {
    kStatusPrepared,
    kStatusStarted,
    kStatusPaused,
    kStatusStopped
  };

  std::atomic<Status> mStatus{ Status::kStatusStopped };

  //!< the attached nodes
  std::vector<std::shared_ptr<AudioNode>> mAttachedNodes;

  //!< The single instance of the AudioInputNode
  std::shared_ptr<AudioInputNode> mInputNode;
  //!< The single instance of the AudioOutputNode
  std::shared_ptr<AudioOutputNode> mOutputNode;
  //!< The single instance of the AudioMixerNode
  std::shared_ptr<AudioMixerNode> mMainMixerNode;

  //!< The manual rendering mode
  std::atomic_bool mManualRenderingModeEnable{ false };
  ManualRenderingMode mManualRenderingMode = ManualRenderingMode::Realtime;
  std::optional<AudioFormat> mManualRenderingFormat;
  AudioFrameCount mManualRenderingMaximumFrameCount = 0;
  AudioFramePosition mManualRenderingSampleTime = 0;
};

} // namespace coral

#endif // CORAL_AUDIO_ENGINE_H
