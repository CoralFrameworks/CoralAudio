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

#ifndef CORAL_AUDIO_NODE_H
#define CORAL_AUDIO_NODE_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioTime.h>
#include <coral/audio/AudioTypes.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace coral {

class AudioEngine;
class AudioEngineImpl;
class AudioNodeImpl;

/**
 * Base class for an audio generation, processing, or I/O block.
 *
 * An AudioEngine object contains instances of audio nodes that you attach,
 * and this base class provides common functionality. Instances of this class
 * don’t provide useful functionality until you attach them to an engine.
 *
 * Nodes have input and output busses, which can be thought of as connection
 * points. For example, an effect typically has one input bus and one output
 * bus. A mixer typically has multiple input busses and one output bus.
 *
 * A bus contains a format the framework expresses in terms of sample rate and
 * channel count. Formats must match exactly when making connections between
 * nodes, excluding AudioMixerNode and AudioOutputNode.
 *
 * Nodes do not currently provide useful functionality until attached to an
 * engine.
 */
class AudioNode : public AudioNoncopyable,
                  public std::enable_shared_from_this<AudioNode> {
  friend class AudioEngineImpl;
  friend class AudioNodeImpl;

public:
  ~AudioNode() override;

  /**
   * Clear a unit's previous processing state.
   */
  void reset();

  /**
   * Gets the input format for the bus you specify.
   */
  [[nodiscard]] std::optional<AudioFormat> inputFormatForBus(AudioNodeBus bus) const;

  /**
   * Retrieves the output format for the bus you specify.
   */
  [[nodiscard]] std::optional<AudioFormat> outputFormatForBus(AudioNodeBus bus) const;

  /**
   * Gets the name of the input bus you specify.
   */
  [[nodiscard]] std::string nameForInputBus(AudioNodeBus bus) const;

  /**
   * Retrieves the name of the output bus you specify.
   */
  [[nodiscard]] std::string nameForOutputBus(AudioNodeBus bus) const;

  /**
   * Returns the number of input busses for the node.
   */
  [[nodiscard]] size_t numberOfInputs() const;

  /**
   * Returns the number of output busses for the node.
   */
  [[nodiscard]] size_t numberOfOutputs() const;

  /**
   * A block that receives copies of the output of an AudioNode.
   *
   * @param buffer
   *     a buffer of audio captured from the output of an AudioNode
   * @param when
   *     the time at which the buffer was captured
   *
   * This callback may be invoked on a thread other than the main thread.
   */
  using TapBlock = std::function<void(const std::unique_ptr<AudioPCMBuffer>& buffer,
    const std::optional<AudioTime>& when)>;

  /**
   * Create a "tap" to record/monitor/observe the output of the node.
   *
   * @param bus        the node output bus to which to attach the tap
   * @param bufferSize the requested size of the incoming buffers in sample frames.
   *                   Supported range is [100, 400] ms.
   * @param format     If non-nil, attempts to apply this as the format of the specified output bus.
   *                   This should only be done when attaching to an output bus which is not
   *                   connected to another node; an error will result otherwise.
   *                   The tap and connection formats (if non-nil) on the specified bus should be identical.
   *                   Otherwise, the latter operation will override any previously set format.
   * @param block a block to be called with audio buffers
   *
   * @discussion
   * Only one tap may be installed on any bus. Taps may be safely installed and removed while the engine
   * is running.
   *
   * Note that if you have a tap installed on AudioOutputNode, there could be a mismatch between the tap
   * buffer format and AudioOutputNode's output format, depending on the underlying physical device.
   * Hence, instead of tapping the AudioOutputNode, it is advised to tap the node connected to it.
   *
   * E.g. to capture audio from input node:
   *<pre>
   *```
   * auto engine = AudioEngine::Create();
   * auto input = engine->inputNode();
   * auto format = input->outputFormatForBus(0);
   * input->installTap(0, 8192, format,
   *   [&](const std::unique_ptr<AudioPCMBuffer>& buffer, const std::optional<AudioTime>& when) {
   *   // 'buffer' contains audio capture from input node at time 'when'
   * });
   * ...
   * // start engine
   * ```
   * </pre>
   */
  void installTapOnBus(AudioNodeBus bus,
    AudioFrameCount bufferSize,
    const std::optional<AudioFormat>& format,
    TapBlock block);

  /**
   * Removes an audio tap on a bus you specify.
   *
   * @param bus the node output bus whose tap is to be destroyed
   */
  void removeTapOnBus(AudioNodeBus bus);

  /**
   * Returns engine to which the node is attached (or nullptr).
   */
  [[nodiscard]] std::shared_ptr<AudioEngine> engine() const;

  /**
   * Returns the time for which the node most recently rendered.
   *
   * @discussion
   * Will return null if the engine is not running or if the node is not connected to an input or
   * output node.
   */
  [[nodiscard]] std::optional<AudioTime> lastRenderTime() const;

  /**
   * Returns the processing latency of the node, in seconds.
   *
   * @discussion
   * This property reflects the delay between when an impulse in the audio stream arrives at the
   * input vs. output of the node. This should reflect the delay due to signal processing
   * (e.g. filters, FFT's, etc.), not delay or reverberation which is being applied as an effect.
   * A value of zero indicates either no latency or an unknown latency.
   */
  [[nodiscard]] uint64_t latency() const;

  /**
   * Returns the maximum render pipeline latency downstream of the node, in seconds.
   *
   * @discussion
   * This describes the maximum time it will take for the audio at the output of a node to be
   * presented.
   * For instance, the output presentation latency of the output node in the engine is:
   * - zero in manual rendering mode
   * - the presentation latency of the device itself when rendering to an audio device
   *   (see `AudioIONode(presentationLatency)`)
   * The output presentation latency of a node connected directly to the output node is the
   * output node's presentation latency plus the output node's processing latency (see `latency`).
   *
   * For a node which is exclusively in the input node chain (i.e. not connected to engine's
   * output node), this property reflects the latency for the output of this node to be
   * presented at the output of the terminating node in the input chain.
   *
   * A value of zero indicates either an unknown or no latency.
   *
   * Note that this latency value can change as the engine is reconfigured (started/stopped,
   * connections made/altered downstream of this node etc.). So it is recommended not to cache
   * this value and fetch it whenever it's needed.
   */
  [[nodiscard]] uint64_t outputPresentationLatency() const;

  /**
   * Returns the name of the AudioNode.
   */
  [[nodiscard]] std::string getName() const {
    return mName;
  }

  [[nodiscard]] std::string toString() const;

  friend std::ostream& operator<<(std::ostream& os, const std::shared_ptr<AudioNode>& node) {
    if (!node) {
      return os;
    }
    return os << node->getName() << ": " << node->toString();
  }

private:
  void attachToEngine(const std::shared_ptr<AudioEngineImpl>& engine);

  void detachFromEngine();

  void clearInputConnection(AudioNodeBus bus) noexcept;

  void clearInputConnectionFrom(const AudioNode* fromNode, AudioNodeBus fromBus, AudioNodeBus bus) noexcept;

  void setOutputConnection(AudioNodeBus outBus, AudioNode* targetNode, AudioNodeBus targetBus) noexcept;

  void clearOutputConnection(AudioNodeBus outBus, const AudioNode* targetNode, AudioNodeBus targetBus) noexcept;

  void clearOutputConnection(AudioNodeBus outBus) noexcept;

  /**
   * Pulls this node's output for one render cycle.
   *
   * @param epoch the render pass, which caches the result of a node that feeds several chains.
   * @param frames the number of frames to render.
   * @param when the time of the first frame of the block in the render timeline, which the
   *             node hands to its render block.
   */
  const AudioPCMBuffer* process(uint64_t epoch, AudioFrameCount frames, const AudioTime& when);

protected:
  explicit AudioNode(std::string name, std::unique_ptr<AudioNodeImpl> impl) noexcept;

  void declareInputBus(const std::string& busName = {});

  void declareOutputBus(const std::string& busName = {});

  bool setInputConnection(AudioNodeBus inputBus,
    const std::shared_ptr<AudioNode>& sourceNode,
    AudioNodeBus sourceBus,
    const std::optional<AudioFormat>& format);

  /**
   * Allocates all resources before rendering.
   */
  virtual void prepareBeforeRendering();

  const std::string mName;

  std::unique_ptr<AudioNodeImpl> impl;
};

} // namespace coral

#endif // CORAL_AUDIO_NODE_H
