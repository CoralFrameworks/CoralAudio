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

#ifndef CORAL_AUDIO_GRAPH_SOURCE_NODE_H
#define CORAL_AUDIO_GRAPH_SOURCE_NODE_H

#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioConverter.h>
#include <coral/audio/AudioSourceNode.h>

#include <memory>
#include <optional>
#include <vector>

namespace coral {

class AudioPCMBuffer;

/**
 * The implementation of the AudioSourceNode.
 *
 * @discussion
 * The node has no inputs: it asks the client's render block for the frames the graph needs,
 * on whatever thread renders the graph. That is the audio driver thread while the engine
 * renders to a device, and the client's own thread while it renders manually, which is
 * exactly what the API documents.
 *
 * The block is called with a buffer list that describes the memory it is expected to fill and
 * whose byte sizes match the requested frame count. It may replace those buffers with memory
 * of its own, which stays valid for the duration of the call.
 *
 * When the client named a block format, the block runs in that format and the node converts
 * what it produced into the output format. Only Linear PCM conversion is supported, as the
 * API documents: the sample rate, the bit depth and the interleaving are handled by
 * `AudioConverter`.
 *
 * Internal use only.
 */
class AudioSourceNodeImpl final : public AudioNodeImpl {
public:
  using RenderBlock = AudioSourceNode::RenderBlock;

  AudioSourceNodeImpl(const std::optional<AudioFormat>& format, RenderBlock block) noexcept;

  ~AudioSourceNodeImpl() override;

  AudioSourceNodeImpl* asSourceImpl() override {
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
   * Points `mBufferList` at the memory the block is expected to fill.
   *
   * @param target the buffer list the block writes into: the render buffer when the block
   *               format is the output format, the node's own scratch buffer otherwise.
   * @param frames the number of frames the block may produce.
   * @return the number of frames the block may produce.
   */
  AudioFrameCount describeBlockOutput(AudioBufferList& target, AudioFrameCount frames);

  /** Whether the block replaced the buffers this node supplied with memory of its own. */
  [[nodiscard]] bool didBlockReplaceBuffers() const;

  /** Moves whatever the block produced into the render buffer, converting when needed. */
  AudioFrameCount moveBlockOutputInto(AudioBufferList& outputData, AudioFrameCount frames);

  //!< The block that supplies the audio.
  RenderBlock mRenderBlock;
  //!< The format the client asked the block to run in, when it named one.
  std::optional<AudioFormat> mRequestedBlockFormat;
  //!< The format the block runs in: the requested one, or the output format.
  std::optional<AudioFormat> mBlockFormat;
  //!< The format the node renders in: the format of its output bus.
  std::optional<AudioFormat> mOutputFormat;
  //!< The buffer the block fills when it runs in a different format than the output.
  std::unique_ptr<AudioPCMBuffer> mBlockBuffer;
  //!< What the converter produced, in the output format.
  std::unique_ptr<AudioPCMBuffer> mConvertedBuffer;
  std::unique_ptr<AudioConverter> mConverter;
  //!< Whether the block format and the output format are the same.
  bool mFormatsMatch = false;
  //!< The list the block is called with, reused for every call.
  std::unique_ptr<AudioBufferList> mBufferList;
  //!< The data pointers this node supplied, used to notice a replacement.
  std::vector<void*> mSuppliedPointers;
  //!< The frames the block has supplied, which is the node's timeline.
  AudioFramePosition mWrittenFrames = 0;
  //!< Set after a failed block call, so that it is only reported once.
  bool mReportedBlockError = false;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_SOURCE_NODE_H
