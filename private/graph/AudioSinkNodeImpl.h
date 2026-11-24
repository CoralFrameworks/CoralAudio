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

#ifndef CORAL_AUDIO_GRAPH_SINK_NODE_H
#define CORAL_AUDIO_GRAPH_SINK_NODE_H

#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioSinkNode.h>

#include <memory>

namespace coral {

/**
 * The implementation of the AudioSinkNode.
 *
 * @discussion
 * The node ends an input chain: it has no output bus, so the graph never pulls it through the
 * output node. The engine pulls it as the root of its own chain instead, on the thread that
 * renders the graph, which is what makes the client's receiver block run on an audio IO
 * thread.
 *
 * The frames handed to the block are the frames of the input it received, in the node's input
 * format: the sink performs no conversion, as its documentation states.
 *
 * Internal use only.
 */
class AudioSinkNodeImpl final : public AudioNodeImpl {
public:
  using ReceiverBlock = AudioSinkNode::ReceiverBlock;

  explicit AudioSinkNodeImpl(ReceiverBlock callback) noexcept;

  AudioSinkNodeImpl* asSinkImpl() override {
    return this;
  }

protected:
  void onReset() override;

  void prepareBeforeRendering() override;

  void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) override;

private:
  /**
   * Describes the frames of `input` for the receiver block.
   *
   * @return the number of frames the buffer list describes, or 0 when it could not be made.
   */
  AudioFrameCount describeInput(const AudioBufferList& input, AudioFrameCount frames);

  //!< The block that receives the audio.
  ReceiverBlock mCallback;
  //!< The buffer list the block is called with, reused for every call.
  std::unique_ptr<AudioBufferList> mBufferList;
  //!< The frames the block has received so far, which is the node's timeline.
  AudioFramePosition mFramesReceived = 0;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_SINK_NODE_H
