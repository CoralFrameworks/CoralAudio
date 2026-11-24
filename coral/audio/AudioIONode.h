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

#ifndef CORAL_AUDIO_IO_NODE_H
#define CORAL_AUDIO_IO_NODE_H

#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioMixing.h>
#include <coral/audio/AudioNode.h>

#include <optional>

namespace coral {

class AudioInputNodeImpl;
class AudioOutputNodeImpl;

/**
 * Base class for a node that performs audio input or output in the engine.
 */
class AudioIONode : public AudioNode {
public:
  /**
   * Returns the presentation or hardware latency, applicable when the engine is
   * rendering to/from an audio device.
   */
  virtual int64_t getPresentationLatency() const = 0;

protected:
  using AudioNode::AudioNode;
};

/**
 * A node that performs audio input in the engine.
 *
 * When the engine is rendering to/from an audio device, this node connects to
 * the system's audio input. When the engine is operating in manual rendering
 * mode, this node can be used to supply the input data to the engine.
 *
 * This node has one element.
 * The format of the input scope reflects:
 *     - the audio hardware sample rate and channel count, when connected to the
 * hardware
 *     - the format of the PCM audio data that the node will supply to the
 * engine, in the manual rendering mode (see
 * `setManualRenderingInputPCMFormat:inputBlock:`)
 *
 * When rendering from an audio device, the input node does not support format
 * conversion. Hence, the format of the output scope must be same as that of the
 * input, as well as the formats for all the nodes connected in the input node
 * chain.
 *
 * In the manual rendering mode, the format of the output scope is initially the
 * same as that of the input, but you may set it to a different format, in which
 * case the node will convert.
 */
class AudioInputNode : public AudioIONode, public AudioMixing {
  friend class AudioEngineImpl;
public:
  int64_t getPresentationLatency() const override;

  /**
   * A callback which will be called by AudioEngine's render call when operating in the manual
   * rendering mode, to get input data as needed.
   *
   * @param inNumberOfFrames
   *     The number of frames required to complete the request. You may supply either these many
   *     frames or none. The count is normally the block the engine renders, but a time effect
   *     downstream at a rate above 1.0 asks for more input frames than it renders.
   * @return
   *     An AudioBufferList to be rendered, or null if no data is available.
   *     The data in the returned buffer must not be cleared or re-filled until the input block is
   *     called again or the rendering has finished.
   *     The format of the returned buffer must match the format specified when registering the
   *     block.
   *
   * @discussion
   *     If you are out of data and return null or less than the requested number of frames, this
   *     data will not be used for rendering. The engine will try to ender from other active
   *     sources in the processing graph, and will inform about the input node's status in the error
   *     returned from its render call.
   *
   *     Note that when the engine is configured to operate in ManualRenderingMode::Realtime, this
   *     block will be called from a realtime context. Care should be taken to make any blocking
   *     call (e.g. blocking on a mutex, allocating memory etc.) which may cause an overload at
   *     the lower layers.
   */
  using InputBlock = std::function<const AudioBufferList*(uint32_t inNumberOfFrames)>;

  /**
   * Supply the data through the input node to the engine operating in the
   * manual rendering mode.
   *
   * @param format The format of the PCM audio data the block will supply to the engine
   * @param block
   */
  void setManualRenderingInputPCMFormat(std::optional<AudioFormat> format, InputBlock block) const;

  std::shared_ptr<AudioMixingDestination>
  destinationForMixer(const std::shared_ptr<AudioNode>& mixer, AudioNodeBus bus) override;

protected:
  AudioInputNode();

  AudioInputNodeImpl* getImpl() const;
};

/**
 * A node that performs audio output in the engine.
 *
 * When the engine is rendering to/from an audio device, this node connects to
 * the system's audio output. When the engine is operating in manual rendering
 * mode, this node performs output in response to client's requests.
 *
 * This node has one element.
 * The format of the output scope reflects:
 *     - the audio hardware sample rate and channel count, when connected to the
 * hardware
 *     - the engine's manual rendering mode output format (see
 *     `AudioEngine(manualRenderingFormat)`), in the manual rendering mode
 *
 * The format of the input scope is initially the same as that of the output,
 * but you may set it to a different format, in which case the node will
 * convert.
 */
class AudioOutputNode : public AudioIONode {
  friend class AudioEngineImpl;
public:
  int64_t getPresentationLatency() const override;

protected:
  AudioOutputNode();

  AudioOutputNodeImpl* getImpl() const;
};

} // namespace coral

#endif // CORAL_AUDIO_IO_NODE_H
