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

#ifndef CORAL_AUDIO_GRAPH_INPUT_NODE_H
#define CORAL_AUDIO_GRAPH_INPUT_NODE_H

#include "private/graph/AudioNodeImpl.h"

#include "private/audio/AudioStream.h"
#include "private/common/AudioRingBuffer.h"

#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioIONode.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace coral {

/**
 * A node implementation that performs audio input in specified platform system,
 * such as iOS, Android, Windows etc.
 *
 * @discussion
 * When the engine renders to an audio device, the capture stream drives this node on
 * the audio driver thread: `onAudioReady` converts the recorded frames into the
 * interleaved float ring buffer, and the graph pulls them out again through
 * `renderBlock` when it renders. In manual rendering mode no stream is created at
 * all; `renderBlock` then asks the client's input block for data.
 *
 * Internal use only.
 */
class AudioInputNodeImpl final : public AudioNodeImpl,
                                 public AudioStreamDataCallback {
public:
  using InputBlock = AudioInputNode::InputBlock;

  AudioInputNodeImpl();

  ~AudioInputNodeImpl() override;

  std::optional<AudioFormat> inputFormatForBus(AudioNodeBus bus) const override;

  std::optional<AudioFormat> outputFormatForBus(AudioNodeBus bus) const override;

  int64_t getPresentationLatency() const {
    return mPresentationLatency.load(std::memory_order_relaxed);
  }

  void prepareBeforeRendering() override;

  bool start();

  void pause();

  void stop();

  /**
   * Sets the format of the audio the node captures in manual rendering mode.
   *
   * @param format The PCM format the manual rendering input block supplies.
   *
   * @discussion Unlike `setManualRenderingInputPCMFormat` this keeps the block that
   * is already installed, so the engine can apply the render format without dropping
   * an input block the client registered earlier.
   */
  void setManualRenderingFormat(const std::optional<AudioFormat>& format);

  void setManualRenderingInputPCMFormat(std::optional<AudioFormat> format, InputBlock block);

  DataCallbackResult
  onAudioReady(AudioStream* stream, void* audioData, AudioFrameCount numFrames) override;

  AudioInputNodeImpl* asInputImpl() override {
    return this;
  }

protected:
  void onReset() override;

  std::optional<AudioFormat> renderFormat() const override;

  void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) override;

private:
  [[nodiscard]] bool isRunning() const {
    return mRunning.load(std::memory_order_relaxed);
  }

  /**
   * Whether the engine renders manually, in which case there is no capture stream and
   * the client's input block supplies the samples.
   */
  [[nodiscard]] bool isManualRendering() const {
    return mManualRendering.load(std::memory_order_relaxed);
  }

  /**
   * Whether another node renders this node's output, i.e. whether the graph pulls it.
   */
  [[nodiscard]] bool hasOutputConnection() const {
    return !mOutputBuses.empty() && !mOutputBuses[0].connections.empty();
  }

  static constexpr AudioFrameCount kDefaultNumberFrames = 1024;
  static constexpr AudioFrameCount kMaxNumberPacket = 4;

  std::atomic<int64_t> mPresentationLatency = 0;

  //!< Indicates whether the engine renders in manual rendering mode.
  std::atomic_bool mManualRendering{ false };

  //!< The input callback for manual render mode.
  InputBlock mInputBlock;
  //!< The input stream to record audio from platform system
  std::shared_ptr<AudioStream> mAudioStream;
  //!< The format of the audio captured by the stream: the hardware format when
  //!< rendering to a device, the manual rendering format otherwise.
  std::optional<AudioFormat> mAudioFormat;
  //!< The format this node renders to the graph, always standard deinterleaved float.
  std::optional<AudioFormat> mGraphFormat;
  //!< Indicates whether the InputNode is running.
  std::atomic_bool mRunning{ false };

  AudioRingBuffer mRingBuffer;
  //!< Interleaved staging between the ring buffer and the graph's float render buffer.
  std::vector<float> mInterleavedBuffer;
  AudioFrameCount mFramesWritten = 0;
  //!< The epoch used when this callback renders the input chains that end in a sink node.
  uint64_t mRenderEpoch = 0;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_INPUT_NODE_H
