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

#ifndef CORAL_AUDIO_SOURCE_NODE_H
#define CORAL_AUDIO_SOURCE_NODE_H

#include <coral/audio/AudioMixing.h>
#include <coral/audio/AudioNode.h>

#include <memory>

namespace coral {

class AudioFormat;

/**
 * AudioSourceNode wraps a client provided block to supply audio.
 *
 * @discussion
 *     With AudioSourceNode the client can supply audio data for rendering
 * through an AudioSourceNode::RenderBlock block.
 */
class AudioSourceNode : public AudioNode, public AudioMixing {
public:
  /**
   * Callback to supply audio data from AudioSourceNode
   *
   * @param isSilence
   *     The client may use this flag to indicate that the buffer it vends
   * contains only silence. The receiver of the buffer can then use the flag as a
   * hint as to whether the buffer needs to be processed or not. Note that because
   * the flag is only a hint, when setting the silence flag, the originator of a
   * buffer must also ensure that it contains silence (zeroes).
   * @param timestamp
   *     The timestamp at which the audio data will be rendered. If there is a
   * sample rate conversion or time compression/expansion downstream, the sample
   * time will not be valid.
   * @param frameCount
   *     The number of sample frames of audio data requested. It is normally the number of
   *     frames the engine renders in the cycle, but a node that feeds a time effect at a rate
   *     above 1.0 is asked for more frames than that, because the effect consumes more input
   *     than it renders. The buffers of outputData are sized for the count.
   * @param outputData
   *     The output audio data.
   *
   *     The caller must supply valid buffers in outputData's mData and
   * mDataByteSize. mDataByteSize must be consistent with frameCount. This block
   * may provide output in those specified buffers, or it may replace the mData
   * pointers with pointers to memory which it owns and guarantees will remain
   * valid until the next render cycle.
   *
   * @return A result code. If an error(negative value) is returned. the audio
   * data should be assumed to be invalid
   */
  using RenderBlock = std::function<Result(bool* isSilence,
      const std::optional<AudioTime>& timestamp,
      AudioFrameCount frameCount,
      std::unique_ptr<AudioBufferList>& outputData)>;

  /**
   * Create a node with a callback.
   *
   * @param block The block to supply audio data to the output.
   * @return An instance of AudioSourceNode
   *
   * The block can be called on realtime or non-realtime threads depending on
   * the engine’s operating mode and it is the client's responsibility to handle
   * it in a thread-safe manner.
   *
   * The audio format for the output bus will be set from the connection format
   * when connecting to another node.
   *
   * The audio format for the block will be set to the node's output format. If
   * node is reconnected with a different output format, the audio format for
   * the block will also change.
   */
  static std::shared_ptr<AudioSourceNode> InitWithCallback(const RenderBlock& block);

  /**
   * Create a node with a callback.
   *
   * @param format The format of the PCM audio data that will be supplied by the callback.
   *
   * @param block The callback to supply audio data to the output.
   *
   * @return An instance of AudioSourceNode
   *
   * @discussion
   * The block can be called on realtime or non-realtime threads depending on
   * the engine’s operating mode and it is the client's responsibility to handle
   * it in a thread-safe manner.
   *
   * The audio format for the output bus will be set from the connection format
   * when connecting to another node.
   *
   * AudioSourceNode supports different audio formats for the block and output,
   * but only Linear PCM conversions are supported (sample rate, bit depth,
   * interleaving).
   */
  static std::shared_ptr<AudioSourceNode>
  InitWithCallback(const std::optional<AudioFormat>& format, const RenderBlock& block);

  std::shared_ptr<AudioMixingDestination>
  destinationForMixer(const std::shared_ptr<AudioNode>& mixer, AudioNodeBus bus) override;

protected:
  explicit AudioSourceNode(const std::optional<AudioFormat>& format, const RenderBlock& block);
};

} // namespace coral

#endif // CORAL_AUDIO_SOURCE_NODE_H
