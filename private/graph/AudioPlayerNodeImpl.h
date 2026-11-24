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

#ifndef CORAL_AUDIO_GRAPH_PLAYER_NODE_H
#define CORAL_AUDIO_GRAPH_PLAYER_NODE_H

#include "private/common/AudioRingBuffer.h"
#include "private/graph/AudioNodeImpl.h"

#include <coral/audio/AudioConverter.h>
#include <coral/audio/AudioPlayerNode.h>
#include <coral/audio/AudioTime.h>

#include <core/event/EventHandler.h>
#include <core/event/EventThread.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace coral {

class AudioFile;

/**
 * The implementation of the AudioPlayerNode.
 *
 * @discussion
 * The node is split in two halves that meet at a ring buffer:
 *
 * - the producer half runs on the node's own IO thread, which is an `EventThread`
 *   driven through an `EventHandler`. It opens/reads the `AudioFile`, takes the
 *   scheduled `AudioPCMBuffer`, converts whatever it decoded into the format of the
 *   output bus with an `AudioConverter`, and writes interleaved float frames into the
 *   ring buffer. While a scheduled start time has not been reached yet it writes
 *   silence instead, which is what makes a scheduled start exact without the render
 *   side having to know about it.
 * - the consumer half is `renderBlock`, which the engine calls on its render thread.
 *   It drains the ring buffer into the render buffer and advances the node's sample
 *   time. A paused or stopped player leaves the frames in the ring buffer, so its
 *   sample time does not advance, which is what `AudioPlayerNode::pause` promises.
 *
 * Only the producer thread touches the decoding state, and only the render thread
 * touches the ring buffer's read side, so the render path takes no lock and does not
 * allocate.
 *
 * @note Scheduling
 * The node keeps one schedule at a time. A new one is handed to the producer through
 * `mPendingSchedule` and replaces whatever was being served, which matches the options
 * this API offers: `kBufferInterrupts` and `kBufferInterruptsAtLoop` both say that the new
 * source interrupts the running one, and the frames that have not been rendered yet are
 * dropped. Because the whole queued block is dropped rather than the tail of a loop pass,
 * interrupting "at the loop point" is approximated by interrupting immediately. A buffer
 * scheduled with `kBufferLoops` repeats until a new schedule or `stop()` replaces it.
 *
 * Internal use only.
 */
class AudioPlayerNodeImpl final : public AudioNodeImpl {
public:
  using BufferOptions = AudioPlayerNode::BufferOptions;
  using CompletionCallbackType = AudioPlayerNode::CompletionCallbackType;
  using CompletionHandler = AudioPlayerNode::CompletionHandler;

  AudioPlayerNodeImpl();

  ~AudioPlayerNodeImpl() override;

  /**
   * Schedules a buffer, a whole file or a segment of a file for playback.
   *
   * @discussion The three entry points differ only in the source they put into the
   * pending schedule; the IO thread picks it up on its next tick.
   */
  void scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer,
    const std::optional<AudioTime>& when,
    BufferOptions options,
    CompletionCallbackType callbackType,
    CompletionHandler completionHandler);

  void scheduleFile(const std::shared_ptr<AudioFile>& file,
    const std::optional<AudioTime>& when,
    CompletionCallbackType callbackType,
    CompletionHandler completionHandler);

  void scheduleSegment(const std::shared_ptr<AudioFile>& file,
    AudioFramePosition startFrame,
    AudioFrameCount numberFrames,
    const std::optional<AudioTime>& when,
    CompletionCallbackType callbackType,
    CompletionHandler completionHandler);

  std::optional<AudioTime>
  nodeTimeForPlayerTime(const std::optional<AudioTime>& playerTime) const;

  std::optional<AudioTime>
  playerTimeForNodeTime(const std::optional<AudioTime>& nodeTime) const;

  /**
   * Prepares the scheduled source, and buffers up to `frameCount` frames of it before
   * returning.
   */
  void prepare(uint32_t frameCount);

  /**
   * Starts or resumes playback, at least `delayMs` milliseconds from now.
   */
  void playback(int64_t delayMs);

  /**
   * Pauses playback. The node's sample time stops advancing, and the frames that are
   * already buffered are kept for the resume.
   */
  void pause();

  /**
   * Clears the scheduled source, drops the buffered frames and resets the sample time.
   */
  void stop();

  [[nodiscard]] bool isPlaying() const {
    return mStatus.load(std::memory_order_relaxed) == Status::kStatusPlaying;
  }

  AudioPlayerNodeImpl* asPlayerImpl() override {
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
   * Everything one scheduling call asks for.
   */
  struct Schedule {
    std::shared_ptr<AudioFile> file;
    std::unique_ptr<AudioPCMBuffer> buffer;
    std::optional<AudioTime> when;
    BufferOptions options = BufferOptions::kBufferInterruptsAtLoop;
    CompletionCallbackType callbackType = CompletionCallbackType::kPlayedBack;
    CompletionHandler handler;
    //!< The first frame of the file to play, and how many frames to play.
    AudioFramePosition startFrame = 0;
    AudioFrameCount numberFrames = 0;

    [[nodiscard]] bool valid() const {
      return file != nullptr || buffer != nullptr;
    }
  };

  enum class Status : uint8_t {
    // clang-format off
    kStatusIdle      = 0, //!< when the player node is idle, which is also the state `stop` returns it to.
    kStatusPrepared  = 1, //!< When the player node is prepared and ready to play
    kStatusPlaying   = 2, //!< When the player node is playing.
    kStatusPaused    = 3, //!< When the player node is paused.
    kStatusCompleted = 5, //!< When the player node has playback completed
    kStatusFailed    = 6, //!< When the player node has something error occurs.
    // clang-format on
  };

  // ---------------------------------------------------------------- producer side

  /** Starts the node's IO thread, if it is not running yet. */
  void startIOThread();

  /** Quits and disposes of the node's IO thread. */
  void stopIOThread();

  /** Starts the IO thread and asks for the first tick, when there is a source to serve. */
  void ensureProducer();

  /**
   * Queues the next producer tick.
   *
   * @param delayMs how long to wait before the tick runs.
   */
  void requestProducerTick(int64_t delayMs);

  /** Runs on the IO thread: activates the pending schedule, then tops the buffer up. */
  void onProducerTick();

  /** Moves the pending schedule into the active one. IO thread only. */
  void activateSchedule();

  /**
   * Fills the ring buffer with as much as it can take.
   *
   * @return the number of frames queued.
   */
  AudioFrameCount produceFrames();

  /**
   * Decodes or converts the next block of the active schedule.
   *
   * @return the number of frames queued, or 0 when the source has no more frames.
   */
  AudioFrameCount produceNextBlock(AudioFrameCount maxFrames);

  /** Queues `frames` frames of interleaved silence. */
  void queueSilence(AudioFrameCount frames);

  /** Converts the processing format block and queues it in the output format. */
  AudioFrameCount queueProcessingBuffer(AudioFrameCount frames);

  /** Moves the schedule's completion state forward and fires the callback when due. */
  void updateCompletion();

  /** Calls the active schedule's completion handler, at most once. */
  void fireCompletion(CompletionCallbackType callbackType);

  /** Drops the pending and active schedules. */
  void clearSchedule();

  /** Drops the frames that are buffered but not rendered yet. */
  void flushRingBuffer();

  /** How long the producer should wait before its next tick. */
  [[nodiscard]] int64_t nextTickDelayMs() const;

  /** Sets up the decode scratch buffers and the converter for the output format. */
  void configureOutputFormat(const std::optional<AudioFormat>& outputFormat);

  /** Resolves the format the scheduled source decodes in. Caller holds `mScheduleLock`. */
  [[nodiscard]] std::optional<AudioFormat> resolveProcessingFormat() const;

  /** Turns a node time in frames into the player's own frame count. */
  [[nodiscard]] static AudioFramePosition convertFrames(AudioFramePosition frames, Float64 from, Float64 to);

  /**
   * Clears the node's own render time, on the thread that is allowed to write it.
   *
   * @discussion The render thread owns the field. While the graph renders, this only asks it
   * to clear the field on its next pass; otherwise it is cleared here.
   */
  void clearLastRenderTime();

  Error* getOutError() const;

  static constexpr AudioFrameCount kFrameCountInvalid = 0;

  //!< The number of frames the producer decodes and converts per turn.
  static constexpr AudioFrameCount kProducerBlockFrames = 1024;
  //!< How long the ring buffer holds, which is the slack the IO thread works with.
  static constexpr int64_t kRingBufferDurationMs = 500;
  //!< How long `prepare` waits for the requested frames to be buffered.
  static constexpr int64_t kPrepareTimeoutMs = 200;

  std::atomic<Status> mStatus{ Status::kStatusIdle };

  /** Guards the pending schedule and the decoding configuration it implies. */
  mutable std::mutex mScheduleLock;
  //!< Written by the client thread, taken over by the IO thread.
  Schedule mPendingSchedule;

  //!< The source the IO thread is serving. IO thread only.
  Schedule mActiveSchedule;
  bool mScheduleActive = false;
  //!< Frames of silence left before the scheduled content starts. IO thread only.
  AudioFramePosition mPendingSilenceFrames = 0;
  //!< Where the next frame comes from in the active source. IO thread only.
  AudioFramePosition mSourcePosition = 0;
  bool mSeekRequest = false;
  //!< Set once the source has no more frames to queue. IO thread only.
  bool mSourceDone = false;

  //!< The format the source decodes in, and the format the graph consumes.
  std::optional<AudioFormat> mProcessingFormat;
  std::optional<AudioFormat> mOutputFormat;
  bool mFormatsMatch = false;

  //!< Decode and conversion scratch. IO thread only.
  std::unique_ptr<AudioPCMBuffer> mPcmBuffer;
  std::unique_ptr<AudioPCMBuffer> mConvertedBuffer;
  std::unique_ptr<AudioConverter> mConverter;
  std::vector<float> mInterleaveScratch;

  /**
   * The room the producer reserves in the ring buffer before it converts a block.
   *
   * @discussion A converted block can be longer than the block read from the file, so the
   * reservation follows the ratio between the processing format and the output format.
   */
  AudioFrameCount mQueueBlockFrames = kProducerBlockFrames; // TEMP-REVERT

  //!< The frames that are buffered or already rendered on the node's timeline.
  std::atomic<int64_t> mQueuedFrames{ 0 };
  std::atomic<int64_t> mRenderedFrames{ 0 };

  //!< Completion bookkeeping for the active schedule. IO thread only.
  int64_t mScheduleRenderedBase = 0;
  int64_t mScheduleQueuedFrames = 0;
  bool mCompletionFired = false;
  TimePoint mPlayedBackDeadline;

  //!< The host time playback may start at, for `playAtTime`.
  TimePoint mPlaybackStartTime;
  //!< Set while playback is waiting for `mPlaybackStartTime` to arrive.
  std::atomic_bool mDelayedStart{ false };
  //!< The node time the player's sample time 0 sits at.
  std::atomic<AudioFramePosition> mStartNodeTime{ 0 };
  //!< The sample rate of the player's timeline.
  std::atomic<Float64> mSampleRate{ 0 };

  //!< The sample frames a `prepare` call was asked for.
  AudioFrameCount mPreparedFrameCount = kFrameCountInvalid;

  //!< The node's IO thread, and the handler its work is posted through.
  std::unique_ptr<EventThread> mIOThread;
  std::shared_ptr<EventHandler> mIOHandler;
  //! Guards `mIOHandler`, which the client thread replaces while the IO thread posts.
  mutable std::mutex mHandlerLock;
  std::atomic_bool mTickScheduled{ false };
  //!< Read by the render thread to know whether the ring buffer can be drained.
  std::atomic_bool mRingBufferReady{ false };
  //!< Set when a stop resets the timeline while the graph renders: the render thread, which
  //!< owns the node's last render time, clears it on its next pass.
  std::atomic_bool mResetLastRenderTimePending{ false };

  //!< The ring buffer between the two halves, and the scratch the render side drains into.
  AudioRingBuffer mRingBuffer;
  std::vector<float> mDeinterleaveScratch;

  std::atomic_bool mRunning{ false };
  std::unique_ptr<Error> mError;
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_PLAYER_NODE_H
