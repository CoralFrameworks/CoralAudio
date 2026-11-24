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

#ifndef CORAL_AUDIO_SINK_NODE_H
#define CORAL_AUDIO_SINK_NODE_H

#include <coral/audio/AudioNode.h>

#include <functional>
#include <memory>
#include <optional>

namespace coral {

/**
 * AudioSinkNode wraps a client provided callback to receive input audio on the
 * audio IO thread.
 *
 * @discussion
 *     AudioSinkNode is restricted to be used in the input chain and does not
 *     support format conversion. Hence when connecting to an AudioSinkNode node,
 *     the format for the connection should be the output scope format of the input
 *     node (essential the format should match the input hardware sample rate).
 *
 *     This node is only supported when the engine is rendering to the audio
 *     device and not in manual rendering mode.
 *
 *     AudioSinkNode does not have an output bus and therefore it does not
 *     support tapping.
 */
class AudioSinkNode : public AudioNode {
public:
  /**
   * Block to receive audio data from AudioSinkNode
   *
   * @param timestamp
   *     The time at which the input data will be rendered.
   *
   * @param frameCount
   *     The number of sample frames of input provided.
   *
   * @param inputData
   *     The input audio data.
   *
   *     The engine will supply valid buffers in inputData's mData and mDataByteSize.
   *     mDataByteSize will be consistent with frameCount.
   *
   *     The pointer to the AudioBuffer is only valid within the scope of this block.
   * @return
   *     The result code. If an error(negative value) is returned, the input data
   *     should be assumed to be invalid.
   */
  using ReceiverBlock = std::function<Result(const std::optional<AudioTime>& timestamp,
      AudioFrameCount frameCount,
      const std::unique_ptr<AudioBufferList>& inputData)>;

  /**
   * Create a node with a receiver block.
   *
   * @param block  The block that receives audio data from the input.
   * @return An instance of AudioSinkNode
   *
   * @discussion
   * The receiver block is called when the input data is available.
   *
   * The block will be called on the realtime thread and it is the client's
   * responsibility to handle it in a thread-safe manner and to not make any
   * blocking calls.
   *
   * The audio format for the input bus will be set from the connection format
   * when connecting to another node.
   *
   * The audio format for the data received by the block will be set to the
   * node's input format.
   */
  static std::shared_ptr<AudioSinkNode> InitWithCallback(const ReceiverBlock& block);

protected:
  explicit AudioSinkNode(const ReceiverBlock& block);
};

} // namespace coral

#endif // CORAL_AUDIO_SINK_NODE_H
