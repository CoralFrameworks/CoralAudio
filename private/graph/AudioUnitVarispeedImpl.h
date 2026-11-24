/*
 * Copyright (C) 2024 The Coral Project
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

#ifndef CORAL_AUDIO_GRAPH_VARISPEED_H
#define CORAL_AUDIO_GRAPH_VARISPEED_H

#include "private/common/AudioRingBuffer.h"
#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioUnitVarispeed.h>

#include <atomic>
#include <optional>
#include <sonic.h>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioUnitTimeEffect that controls the playback rate.
 *
 * @discussion
 * The rate is applied by the sonic library. Sonic's "rate" parameter resamples the stream, so
 * the speed and the pitch move together, which is what this unit documents: a rate of 2.0
 * plays an octave higher.
 *
 * The graph hands the node a fixed number of frames and expects the same number back, while a
 * rate other than 1.0 changes how many frames of input a block of output needs. The node
 * therefore pulls the number of input frames its rate asks for through
 * `inputFramesForBlock()`, which the engine sizes the upstream render buffers for, and whatever
 * sonic produced beyond the block is held in the node's ring buffer. The ring only has to
 * absorb the rounding of the rate, its change at runtime and the latency of the sonic stream,
 * so a rate other than 1.0 no longer leaves the node with less audio than the graph asked for.
 *
 * Internal use only.
 */
class AudioUnitVarispeedImpl final : public AudioNodeImpl {
public:
  AudioUnitVarispeedImpl();

  ~AudioUnitVarispeedImpl() override;

  /**
   * Returns the bypass state of the audio unit.
   */
  [[nodiscard]] bool isBypass() const {
    return bypass.load(std::memory_order_relaxed);
  }

  /**
   * Sets the bypass state of the audio unit.
   *
   * @param newBypass the new bypass state
   */
  void setBypass(const bool newBypass) {
    bypass.store(newBypass, std::memory_order_relaxed);
  }

  /**
   * Returns the playback rate of the audio signal.
   *
   * The default value is 1.0. The range of values is 0.25 to 4.0.
   */
  [[nodiscard]] float getRate() const {
    return rate.load(std::memory_order_relaxed);
  }

  /**
   * Sets the playback rate of the audio signal.
   *
   * @param newRate the playback rate, clamped to the documented range of 0.25 to 4.0.
   */
  void setRate(float newRate);

  AudioUnitVarispeedImpl* asVarispeedImpl() override {
    return this;
  }

  void prepareBeforeRendering() override;

  /**
   * The number of frames the unit pulls from its input for `frames` frames of output.
   *
   * @discussion Sonic's rate is a resampling ratio: a rate of 2.0 produces half as many output
   * frames as the input it consumes, so the unit has to consume twice the frames of the block
   * it renders. Without that, the graph would ask for more frames than the unit can produce
   * and the extra frames would be silence.
   */
  [[nodiscard]] AudioFrameCount inputFramesForBlock(AudioFrameCount frames) const override;

  /**
   * The largest number of input frames the unit may ask for, which is the block times the
   * largest rate the API allows. The engine sizes the upstream buffers with it.
   */
  [[nodiscard]] AudioFrameCount maxInputFramesForBlock(AudioFrameCount frames) const override;

protected:
  void onReset() override;

  void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) override;

private:
  /** Creates the sonic stream, the staging buffers and the ring buffer for `format`. */
  void initWithInputFormat(const AudioFormat& format);

  /** Applies a rate the client changed since the last block, on the render thread. */
  void applyPendingRate();

  /** Moves everything sonic has produced into the ring buffer. */
  void drainSonic();

  //!< How many frames the drain loop moves per iteration.
  static constexpr AudioFrameCount kDrainBlockFrames = 1024;

  //!< The rate range the API documents, which also bounds how much input a block may pull.
  static constexpr float kMinRate = 0.25f;
  static constexpr float kMaxRate = 4.0f;

  //!< Bounds how many times one render block drains the sonic stream.
  static constexpr int kMaxDrainRounds = 32;

  //!< The playback rate. The default is 1.0 and the range is 0.25 to 4.0.
  std::atomic<float> rate{ 1.0f };
  //!< Set by the setter and consumed by the render thread, so the setter stays lock free.
  std::atomic_bool mRateChanged{ false };

  sonicStream mSonic = nullptr;
  //!< The bypass flag of the owning node, or null when there is none.
  std::atomic_bool bypass{ false };
  //!< The format the sonic stream was created for.
  std::optional<AudioFormat> mSonicFormat;

  //!< Interleaved float staging, which is the layout sonic reads and writes.
  std::vector<float> mInterleavedInput;
  std::vector<float> mInterleavedOutput;
  //!< Absorbs the difference between what the rate produces and what the graph asks for.
  AudioRingBuffer mRingBuffer;
  //!< The frames the node has produced, which is its timeline.
  AudioFramePosition mWrittenFrames = 0;
  //!< Set after a format mismatch has been reported once.
  bool mReportedFormatMismatch = false;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_VARISPEED_H
