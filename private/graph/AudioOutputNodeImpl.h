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

#ifndef CORAL_AUDIO_GRAPH_OUTPUT_NODE_H
#define CORAL_AUDIO_GRAPH_OUTPUT_NODE_H

#include "private/graph/AudioNodeImpl.h"

#include "private/audio/AudioStream.h"
#include "private/common/AudioRingBuffer.h"

#include <coral/audio/AudioIONode.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace coral {

/**
 * A node implementation that performs audio output in specified platform system, such as iOS, Android, Windows etc.
 *
 * @discussion
 * When the engine renders to an audio device this node is the root of the graph: its
 * render stream callback pulls the graph, and the rendered block is handed to the
 * interleaved ring buffer so that the callback can drain it in the device's own format.
 * In manual rendering mode no stream is created and the engine pulls the node itself.
 *
 * Internal use only.
 */
class AudioOutputNodeImpl final : public AudioNodeImpl,
                                  public AudioStreamDataCallback {
public:
  AudioOutputNodeImpl();

  ~AudioOutputNodeImpl() override;

  void setOutputFormat(const AudioFormat& format);

  int64_t getPresentationLatency() const {
    return mPresentationLatency.load(std::memory_order_relaxed);
  }

  void prepareBeforeRendering() override;

  bool start();

  void pause();

  void stop();

  AudioOutputNodeImpl* asOutputImpl() override {
    return this;
  }

  DataCallbackResult
  onAudioReady(AudioStream* stream, void* audioData, AudioFrameCount numFrames) override;

protected:
  void onReset() override;

  std::optional<AudioFormat> inputFormatForBus(AudioNodeBus bus) const override;

  std::optional<AudioFormat> outputFormatForBus(AudioNodeBus bus) const override;

  std::optional<AudioFormat> renderFormat() const override;

  void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) override;

private:
  static constexpr int kDefaultBurstsPerBuffer = 8;
  static constexpr int kDefaultCacheDuration = 1;
  static constexpr AudioFrameCount kDefaultNumberFrames = 1024;

  /**
   * Whether the engine renders manually, in which case there is no render stream and the
   * engine pulls this node through `AudioEngine::renderOffline`.
   */
  [[nodiscard]] bool isManualRendering() const {
    return mManualRendering.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool isRunning() const {
    return mRunning.load(std::memory_order_relaxed);
  }

  /**
   * Interleaves the rendered block into the ring buffer in the device's own format.
   */
  void writeToRingBuffer(const AudioBufferList& source, AudioFrameCount numFrames);

  //!< Indicates whether the engine renders in manual rendering mode.
  std::atomic_bool mManualRendering{ false };

  std::atomic<int64_t> mPresentationLatency = 0;

  //!< Indicates whether the AudioOutputNode is started
  std::atomic_bool mRunning{ false };

  //!< The output audio stream to playback audio data to platform system.
  std::shared_ptr<AudioStream> mAudioStream;
  //!< The output format of the AudioStream or the manual render format.
  std::optional<AudioFormat> mAudioFormat;
  //!< The format the register graph renders in: standard deinterleaved float.
  std::optional<AudioFormat> mGraphFormat;
  //!< The render epoch for AudioStream's callback.
  uint64_t mRenderEpoch = 0;
  //!< The frames the graph has rendered, which is the timeline the render cycles are timed in.
  AudioFramePosition mRenderPosition = 0;

  //!< A RingBuffer to cache audio
  AudioRingBuffer mRingBuffer;
  //!< Interleaved staging between the graph's float buffer and the ring buffer.
  std::vector<float> mInterleavedBuffer;
  //!< Indicates how many frames written into AudioStream or consumed by client in manual render mode.
  std::atomic<int64_t> mFramesWritten{ 0 };
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_OUTPUT_NODE_H
