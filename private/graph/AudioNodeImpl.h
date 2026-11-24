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

#ifndef CORAL_AUDIO_GRAPH_AUDIO_NODE_IMPL_H
#define CORAL_AUDIO_GRAPH_AUDIO_NODE_IMPL_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioNode.h>
#include <coral/audio/AudioTime.h>
#include <coral/audio/AudioTypes.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace coral {

class AudioEngineImpl;
class AudioInputNodeImpl;
class AudioMixerNodeImpl;
class AudioOutputNodeImpl;
class AudioPlayerNodeImpl;
class AudioSinkNodeImpl;
class AudioSourceNodeImpl;
class AudioUnitDelayImpl;
class AudioUnitDistortionImpl;
class AudioUnitEqualizerImpl;
class AudioUnitReverbImpl;
class AudioUnitTimePitchImpl;
class AudioUnitVarispeedImpl;

/**
 * Bass class for all graph nodes to process audio samples
 */
class AudioNodeImpl : public AudioNoncopyable {
  friend class AudioEngineImpl;

public:
  AudioNodeImpl();

  virtual void clear();

  void declareInputBus(std::string busName);

  void declareOutputBus(std::string busName);

  void attachToEngine(const std::shared_ptr<AudioEngineImpl>& engine) {
    mEngine = engine;
  }

  void detachFromEngine() {
    mEngine.reset();
  }

  [[nodiscard]] virtual std::optional<AudioFormat> inputFormatForBus(AudioNodeBus bus) const;

  [[nodiscard]] virtual std::optional<AudioFormat> outputFormatForBus(AudioNodeBus bus) const;

  [[nodiscard]] std::string nameForInputBus(AudioNodeBus bus) const;

  [[nodiscard]] std::string nameForOutputBus(AudioNodeBus bus) const;

  [[nodiscard]] size_t numberOfInputs() const {
    return mInputBuses.size();
  }

  [[nodiscard]] size_t numberOfOutputs() const {
    return mOutputBuses.size();
  }

  void installTapOnBus(AudioNodeBus outBus,
    AudioFrameCount bufferSize,
    const std::optional<AudioFormat>& format,
    AudioNode::TapBlock block);

  void removeTapOnBus(AudioNodeBus outBus);

  /**
   * Whether a tap is installed on the given output bus.
   *
   * @discussion Safe to call from any thread: the atomic gate keeps the common
   * "no tap installed" answer lock free, and the tap list itself is only read under
   * the lock that installing and removing a tap take.
   */
  [[nodiscard]] bool hasTapOnBus(AudioNodeBus bus) const {
    if (!mHasTaps.load(std::memory_order_acquire)) {
      return false;
    }
    const std::lock_guard<std::mutex> guard(mTapsLock);
    return bus < mTaps.size() && mTaps[bus] != nullptr;
  }

  /**
   * Sets the largest number of frames a single render call may ask this node for.
   *
   * @discussion Called by the engine before rendering starts, so that a node can
   * allocate its render buffer once instead of growing it in a callback.
   */
  void setMaxRenderFrames(const AudioFrameCount frames) {
    if (frames > 0) {
      mMaxRenderFrames = frames;
    }
  }

  [[nodiscard]] AudioFrameCount getMaxRenderFrames() const {
    return mMaxRenderFrames;
  }

  /**
   * The number of frames this node asks each input to render for one output block.
   *
   * @discussion
   * A normal node consumes as many frames as it produces, so this is `frames`. A time effect
   * whose rate is not 1.0 consumes a different number of frames than it produces: a rate of
   * 2.0 plays the input twice as fast, which means it has to consume two frames of input for
   * every frame it hands to the graph. Such a node overrides this and is pulled with the frame
   * count it asks for, which is why the nodes upstream of it are sized with
   * `maxInputFramesForBlock()` before rendering starts.
   *
   * @param frames the number of frames the node renders in this block.
   * @return the number of frames the node wants from each of its inputs.
   */
  [[nodiscard]] virtual AudioFrameCount inputFramesForBlock(const AudioFrameCount frames) const {
    return frames;
  }

  /**
   * The largest number of frames this node can ask from its inputs for an output block of
   * `frames`.
   *
   * @discussion
   * The engine calls this before rendering starts, so that the render buffers of the nodes
   * upstream of this one can be allocated large enough for the biggest block they may be asked
   * for. A node that changes its input consumption at runtime (a time effect whose rate the
   * client can change without a new prepare) reports the maximum it may ever need.
   */
  [[nodiscard]] virtual AudioFrameCount maxInputFramesForBlock(const AudioFrameCount frames) const {
    return frames;
  }

  [[nodiscard]] std::shared_ptr<AudioEngineImpl> getEngine() const {
    return mEngine.lock();
  }

  [[nodiscard]] std::optional<AudioTime> getLastRenderTime() const {
    return mLastRenderTime;
  }

  [[nodiscard]] uint64_t getLatency() const {
    return mLatency;
  }

  [[nodiscard]] uint64_t getOutputPresentationLatency() const {
    return mOutputPresentationLatency;
  }

  /**
   * Records the node's own render time.
   *
   * @param position   the position of the node's timeline after the block it rendered.
   * @param sampleRate the rate `position` is expressed in, which a node takes from the
   *                   `when` of the render cycle it is rendering in.
   *
   * @discussion
   * A node's timeline is its own: a player counts the frames it produced and an input node
   * the frames it captured, which is not the position of the render cycle that the `when` of
   * `renderBlock` describes. That cycle only supplies the rate.
   */
  void updateLastRenderTime(AudioFramePosition position, Float64 sampleRate);

  /**
   * Clears the node's own render time.
   *
   * @discussion
   * A node whose timeline is reset, for example a player that is stopped, loses the render
   * time it had before the reset. The write belongs to the thread that renders the node, so a
   * node that renders while the client resets it defers this call to its render path.
   */
  void resetLastRenderTime() {
    mLastRenderTime.reset();
  }

  /**
   * The sample rate a node's own timeline is expressed in for the given render cycle.
   *
   * @discussion
   * This is the rate of the cycle the node is being rendered in. It falls back to the format
   * of the node's own render buffer, which is the same rate, so that a node rendered outside
   * the engine's render loop still gets a usable value.
   */
  [[nodiscard]] Float64 renderSampleRate(const AudioTime& when) const;

  virtual bool setInputConnection(const AudioNode* inputNode,
    AudioNodeBus inputBus,
    const std::shared_ptr<AudioNode>& sourceNode,
    AudioNodeBus sourceBus,
    const std::optional<AudioFormat>& format);

  void removeInputConnection(const AudioNode* inputNode, AudioNodeBus inputBus) noexcept;
  void removeInputConnection(const AudioNode* fromNode, AudioNodeBus fromBus, AudioNodeBus inputBus) noexcept;

  /**
   * Called when an input bus loses its connection.
   *
   * @discussion
   * A node can use it to drop the state it derived from the source of the bus, for example a
   * cached interface of that source. It runs while the engine is stopped, so it may allocate.
   */
  virtual void onInputConnectionRemoved(AudioNodeBus inputBus) {
    // Do nothing
  }

  void addOutputConnection(AudioNodeBus outBus, AudioNode* destNode, AudioNodeBus destBus);

  void removeOutputConnection(const AudioNode* outputNode, AudioNodeBus outputBus);
  void removeOutputConnection(AudioNodeBus outBus, const AudioNode* targetNode, AudioNodeBus targetBus);

  virtual void prepareBeforeRendering();

  /**
   * Allocates the render buffer for the current maximum render frame count.
   *
   * @discussion The engine re-runs this for the I/O nodes once the hardware streams have
   * decided how many frames a block can contain, without reopening those streams.
   */
  void prepareRenderBuffer();

  const AudioPCMBuffer* render(uint64_t epoch, AudioFrameCount frames, const AudioTime& when);

  /**
   * Copies the rendered frames into the tap buffers.
   *
   * @discussion
   * In manual rendering mode the tap block is invoked as soon as a tap buffer has
   * been filled. When the engine renders to an audio device, the tap block must not
   * run on the audio driver thread, so a full buffer is only published here and is
   * delivered later by `flushTaps()` on the engine's IO thread.
   */
  void dispatchTaps(const AudioPCMBuffer& source, AudioFrameCount frames);

  /**
   * Delivers every tap buffer that the render thread has published.
   *
   * @discussion Called from the engine's IO thread when rendering to an audio device.
   */
  void flushTaps();

  [[nodiscard]] std::string toString() const;

  // Type query; the base class returns `nullptr` by default.
  virtual AudioInputNodeImpl* asInputImpl() { return nullptr; }
  virtual AudioOutputNodeImpl* asOutputImpl() { return nullptr; }
  virtual AudioMixerNodeImpl* asMixerImpl() { return nullptr; }
  virtual AudioPlayerNodeImpl* asPlayerImpl() { return nullptr; }
  virtual AudioSinkNodeImpl* asSinkImpl() { return nullptr; }
  virtual AudioSourceNodeImpl* asSourceImpl() { return nullptr; }
  virtual AudioUnitDelayImpl* asDelayImpl() { return nullptr; }
  virtual AudioUnitDistortionImpl* asDistortionImpl() { return nullptr; }
  virtual AudioUnitEqualizerImpl* asEqualizerImpl() { return nullptr; }
  virtual AudioUnitReverbImpl* asReverbImpl() { return nullptr; }
  virtual AudioUnitTimePitchImpl* asTimePitchImpl() { return nullptr; }
  virtual AudioUnitVarispeedImpl* asVarispeedImpl() { return nullptr; }

protected:
  virtual void onReset();

  static void handleError(Error** outError, const char* errorMsg) {
    handleError(outError, errorMsg, "");
  }

  static void handleError(Error** outError, const char* errorMsg, const char* description) {
    handleError(outError, ErrCode::kUnknown, errorMsg, description);
  }

  static void handleError(Error** outError, ErrCode code, const char* errorMsg, const char* description);

  static void logError(ErrCode code, const char* errMsg, const char* description);

  /**
   * The default number of samples per frame
   */
  static constexpr uint32_t kDefaultNumberFrames = 1024;

  /**
   * The max number of packet to cache
   */
  static constexpr AudioFrameCount kMaxNumberPacket = 4;

  /**
   * The minimum duration(ms) for the AudioNode::TapBlock
   */
  static constexpr int32_t kTapBlockMinDurationMs = 100;

  /**
   * The maximum duration(ms) for the AudioNode::TapBlock
   */
  static constexpr int32_t kTapBlockMaxDurationMs = 100;

  [[nodiscard]] bool isInputBusAvailable(AudioNodeBus bus) const;

  /**
   * Renders one block of audio for the current render cycle.
   *
   * @discussion
   * Every argument is a non-owning view. `inputs` holds one `AudioBufferList` per input bus,
   * `nullptr` for a bus that has no upstream node, and `outputData` is the node's render
   * buffer, or `nullptr` for a node without an output bus (a sink), which ends an input
   * chain.
   *
   * A list is in the standard deinterleaved float format, so each of its buffers is one
   * channel plane of `numFrames` valid frames and a node reads and writes the samples through
   * `AudioBuffer::mData`. The byte sizes of the views belong to the graph, which set them up
   * before the call: a node must not change them and takes the number of frames from
   * `numFrames` instead. It must not resize a list either, which would allocate on an audio
   * thread.
   *
   * The output block is silent before the call, so whatever a node does not write stays
   * silent, and an input holds exactly `inputFramesForBlock(numFrames)` valid frames, because
   * the graph fills the tail of a short block with silence. For a normal node that is
   * `numFrames`; a time effect at a rate other than 1.0 asks for more or fewer frames and gets
   * exactly the count its override requested.
   *
   * @param inputs     one buffer list per input bus, in the order the buses were declared.
   * @param numInputs  the number of entries in `inputs`.
   * @param outputData the render buffer, or `nullptr` for a sink node.
   * @param numFrames  the number of frames to render.
   * @param when       the time of the first frame of this block in the render timeline.
   */
  virtual void renderBlock(const AudioBufferList* const* inputs,
    uint32_t numInputs,
    AudioBufferList* outputData,
    AudioFrameCount numFrames,
    const AudioTime& when) = 0;

  /**
   * The format this node renders its own output buffer in.
   *
   * @discussion
   * The graph renders in the standard deinterleaved float format, which is the
   * output bus format of every node except the I/O nodes: their bus format is the
   * hardware or manual rendering format, while their render buffer stays float and
   * the conversion happens at the stream boundary.
   */
  [[nodiscard]] virtual std::optional<AudioFormat> renderFormat() const;

  /**
   * Indicates whether the AudioGraphNode supports multiple input buses.
   */
  [[nodiscard]] virtual bool supportsMultiInputs() const {
    return false;
  }


  struct Connection {
    AudioNode* node = nullptr;
    AudioNodeBus bus = 0;

    [[nodiscard]] bool valid() const noexcept {
      return node != nullptr;
    }
  };

  struct InputBus {
    std::optional<AudioFormat> format;
    std::string name;
    Connection upstream;
  };

  struct OutputBus {
    std::optional<AudioFormat> format;
    std::string name;
    std::vector<Connection> connections;
  };

  std::weak_ptr<AudioEngineImpl> mEngine;

  std::unique_ptr<AudioPCMBuffer> mOutputBuffer;
  uint64_t mCachedEpoch = 0;
  std::vector<const AudioPCMBuffer*> mInputPtrs;
  std::vector<const AudioBufferList*> mInputViews;

  /**
   * The largest number of frames a single render call may ask this node for.
   * The engine sets it before rendering so the render buffer is allocated once.
   */
  AudioFrameCount mMaxRenderFrames = kDefaultNumberFrames;

  std::vector<InputBus> mInputBuses;
  std::vector<OutputBus> mOutputBuses;

  //!< The most recent render time.
  std::optional<AudioTime> mLastRenderTime;
  //!< The processing latency of the node, in seconds.
  uint64_t mLatency;
  //!< The maximum render pipeline latency downstream of the node, in seconds.
  uint64_t mOutputPresentationLatency;

  /**
   * The holder of the AudioNode::TapBlock
   */
  struct TapState {
    AudioNodeBus bus = 0;
    AudioFrameCount bufferSize = 0;
    std::optional<AudioFormat> format;
    AudioNode::TapBlock block;
    AudioFrameCount fillPos = 0;
    int64_t samplesSent = 0;
    std::unique_ptr<AudioPCMBuffer> tapBuffer;

    /**
     * Set by the render thread when `tapBuffer` is full and must be delivered on
     * another thread, cleared by the IO thread once the block has returned. A tap
     * that is still pending drops the frames of the next render pass instead of
     * overwriting audio the client has not consumed yet.
     */
    std::atomic_bool pending{ false };
    //!< The render time of the first frame currently held in `tapBuffer`.
    AudioFramePosition pendingPosition = 0;
  };

  //! The holders of the AudioNode::TapBlock
  std::vector<std::shared_ptr<TapState>> mTaps;
  //! Guards `mTaps`, which the render thread and the client thread both reach.
  mutable std::mutex mTapsLock;

  /**
   * A tap buffer that is full and is waiting for its block to be called.
   *
   * The tap is held by shared pointer so that removing a tap while a buffer is in
   * flight cannot free the state the delivery still reads.
   */
  struct PendingTap {
    std::shared_ptr<TapState> tap;
    AudioFramePosition position = 0;
  };

  //! Scratch list used to run the tap blocks outside of `mTapsLock`.
  std::vector<PendingTap> mReadyTaps;
  //! Scratch list used by `flushTaps`, kept apart from `mReadyTaps` so that the two
  //! delivery paths can never consume each other's entries.
  std::vector<PendingTap> mPendingTaps;
  //! Read by the render thread to keep the usual no-tap path lock free.
  std::atomic_bool mHasTaps{ false };
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_AUDIO_NODE_IMPL_H
