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

#ifndef CORAL_AUDIO_IO_AUDIO_PLAYER_IMPL_H
#define CORAL_AUDIO_IO_AUDIO_PLAYER_IMPL_H

#include <coral/audio/AudioPlayer.h>

#include <coral/audio/AudioFile.h>

#include <core/event/EventHandler.h>
#include <core/event/EventThread.h>

#include "private/audio/AudioStream.h"
#include "private/common/AudioRingBuffer.h"

#include <sonic.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioPlayer using AudioEngine
 *
 * @discussion
 * The player is split in two halves that meet at a ring buffer:
 *
 * - the render half is `onAudioReady`, which the audio stream calls on its own real time thread. It
 *   drains the ring buffer through the rate changer and, when the buffer is running low, sends one
 *   `MSG_READ_AUDIO` event: the audio that is played is what triggers the reading of the file.
 * - the file half runs on the player's own `EventThread`, driven through an `EventHandler`.
 *   Handling a read event reads as much of the file as the ring buffer can take, and every entry
 *   point of the public API (`play`, `playAtTime`, `pause`, `stop`, `seekTo`, the rate and the
 *   volume) is an event of that same queue.
 *
 * Because the reading is driven by the playback rather than by a timer, the file is only read when
 * the audio that is being rendered needs it, and one event refills the whole ring buffer instead of
 * handing over a single block.
 */
class AudioPlayerImpl final : public AudioPlayer,
                              public AudioStreamDataCallback,
                              public EventHandler::Callback,
                              public std::enable_shared_from_this<AudioPlayerImpl> {
public:
  static std::shared_ptr<AudioPlayerImpl> Make(const std::string& url);

  explicit AudioPlayerImpl(std::string url) noexcept;

  ~AudioPlayerImpl() override;

  DataCallbackResult
  onAudioReady(AudioStream* stream, void* audioData, AudioFrameCount numFrames) override;

  /**
   * Handles one event of the player's message thread.
   *
   * @param msg the event to handle, whose `what` is one of the `MSG_*` constants.
   * @return always true, i.e. the event is never redelivered.
   */
  bool handleMessage(Message* msg) override;

protected:
  bool onPrepare() override;

  bool onPlayDelay(int64_t delayMs) override;

  void onPause() override;

  void onStop() override;

  void onPlaybackRateUpdate() override;

  void onSeekUpdate(int64_t timeUs) override;

  void onVolumeUpdate() override;

private:
  /** Starts the thread the file work runs on, if it is not running yet. */
  void startMessageThread();

  /** Quits the thread the file work runs on and releases it. */
  void stopMessageThread();

  /** Sends one event to the message thread, and reports whether it was accepted. */
  bool sendEvent(int what);

  /** Sends one event to the message thread, to be handled once the delay elapsed. */
  bool sendEventDelayed(int what, TimeDelta delay);

  /**
   * Asks the message thread to read the file.
   *
   * At most one read event is queued at a time, because handling one reads everything the ring
   * buffer can take: the events that are not needed would only make the queue longer than the audio.
   */
  void requestReadEvent();

  /** Asks the message thread to handle the end of the playback, at most one event at a time. */
  void requestCompletionEvent();

  /** Drops the read and completion events that a playback leaves behind when it is stopped. */
  void cancelPendingEvents();

  /** Handles `MSG_READ_AUDIO`. Runs on the message thread. */
  void onReadAudioEvent();

  /** Handles `MSG_PLAYBACK_COMPLETED`. Runs on the message thread. */
  void onPlaybackCompletedEvent();

  /** Stopped the message thread and releases every resource of the player. */
  void release();

  void startPlayback(int64_t delayMs);

  void resetInternal();

  void startInternal();

  void resumeInternal();

  void pauseInternal();

  void stopInternal();

  void seekInternal();

  void resetSeekTime();

  bool canReadAudioFile() const;

  bool isPlaybackFinished() const;

  bool shouldNextLoop() const;

  void startNextLooping();

  bool initAudioFile(Error* error);

  bool initAudioStream(Error* error);

  void maybeStartAudioStream();

  void maybeUpdateRateAndVolume();

  /** Reads the file until the ring buffer cannot take another block. */
  void readAudioFromFile();

  /**
   * Reads one block of the file, converts it and stores it in the ring buffer.
   *
   * @return whether the block reached the ring buffer, i.e. whether another pass would make
   *         progress.
   */
  bool readNextAudioBlock();

  bool writeAudioBuffer(const std::unique_ptr<AudioPCMBuffer>& pcmBuffer);

  void updateStartPosition();

  void updateStartReadPosition();

  void updatePlaybackTimeUs();

  void notifyPlaybackFinished(bool successfully);

  void notifyPlaybackError(const Error* error = nullptr);

  Error* getOutError() const {
    if (mError) {
      mError->clear();
    }
    return mError.get();
  }

  int64_t getStartPositionUs() const {
    return mStartTimeUs;
  }

  int64_t getSeekPositionUs() const {
    return mSeekTimeUs;
  }

  static std::string getName() {
    return "AudioPlayer";
  }

  static constexpr AudioFrameCount kDefaultBurstSize = 8;

  static constexpr AudioFrameCount kDefaultCacheDuration = 1;

  static constexpr AudioFrameCount kDefaultNumberFrames = 1152;

  /** Starts the playback, after the delay of a scheduled start. */
  static constexpr int MSG_START_PLAYBACK = 0;
  /** Resumes a paused playback. */
  static constexpr int MSG_RESUME_PLAYBACK = 1;
  /** Pauses the audio stream of the player. */
  static constexpr int MSG_PAUSE_PLAYBACK = 2;
  /** Stops the audio stream and drops the buffered audio. */
  static constexpr int MSG_STOP_PLAYBACK = 3;
  /** Moves the reading position of the file to the requested time. */
  static constexpr int MSG_SEEK = 4;
  /** Applies the playback rate and the volume to the rate changer. */
  static constexpr int MSG_UPDATE_RATE_AND_VOLUME = 5;
  /** Reads the file. Sent by the render callback when the ring buffer runs low. */
  static constexpr int MSG_READ_AUDIO = 6;
  /** Reports the end of the playback, or starts the next loop. */
  static constexpr int MSG_PLAYBACK_COMPLETED = 7;

  /** The thread the file work runs on, and the handler its events are posted through. */
  std::unique_ptr<EventThread> mThread;
  std::shared_ptr<EventHandler::Callback> mCallbackRef;
  std::shared_ptr<EventHandler> mHandler;
  /** Guards `mHandler`, which a caller thread replaces while the message thread posts. */
  mutable std::mutex mHandlerLock;
  /** Whether a read event is already queued, so that only one is ever pending. */
  std::atomic_bool mReadScheduled{ false };
  /** Whether the completion event is already queued, so that only one is ever pending. */
  std::atomic_bool mCompletionScheduled{ false };

  std::atomic<int64_t> mReadPosition;
  std::atomic_int mNumberOfReading;

  std::atomic<int64_t> mSeekTimeUs;
  std::atomic<int64_t> mStartTimeUs;
  std::atomic_bool mReadingEof;

  std::unique_ptr<Error> mError;
  std::shared_ptr<AudioFile> mAudioFile;
  std::unique_ptr<AudioPCMBuffer> mPcmBuffer;
  std::shared_ptr<AudioStream> mAudioStream;

  sonicStream mSonic;

  std::vector<short> mAppBuffer;
  AudioRingBuffer mRingBuffer;
};

} // namespace coral

#endif // CORAL_AUDIO_IO_AUDIO_PLAYER_IMPL_H
