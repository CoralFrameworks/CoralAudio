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

#ifndef CORAL_AUDIO_IO_AUDIO_RECORDER_H
#define CORAL_AUDIO_IO_AUDIO_RECORDER_H

#include <coral/audio/AudioRecorder.h>

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFile.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/Resample.h>
#include <core/task/CountDownLatch.h>

#include <core/event/EventHandler.h>
#include <core/event/EventThread.h>

#include "private/audio/AudioStream.h"
#include "private/common/AudioRingBuffer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioRecorder.
 *
 * @discussion
 * The recorder is split in two halves that meet at a ring buffer:
 *
 * - the capture half is `onAudioReady`, which the audio stream calls on its own real time thread. It
 *   only copies the frames it was handed into the ring buffer and then sends one `MSG_WRITE_AUDIO`
 *   event: no resampling, no conversion and no file access happen on that thread.
 * - the file half runs on the recorder's own `EventThread`, driven through an `EventHandler`.
 *   Handling a write event drains the ring buffer into the file, resampling when the processing
 *   format of the file differs from the capture format, and every entry point of the public API
 *   (`prepareAsync`, `recordAtTime`, `stop`) is an event of that same queue.
 *
 * The events are what keeps the writing exactly as fast as the audio arrives: the capture side only
 * asks for a write event when it stored frames and no event is pending, and handling one event
 * writes everything the ring buffer holds, so the buffer stays shallow and the writer never spins
 * on an empty buffer.
 */
class AudioRecorderImpl final : public AudioRecorder,
                                public AudioStreamDataCallback,
                                public EventHandler::Callback,
                                public std::enable_shared_from_this<AudioRecorderImpl> {
public:
  static std::shared_ptr<AudioRecorderImpl>
  Make(const std::string& url, MediaFileType fileType, const AudioFormat& format);

  explicit AudioRecorderImpl(std::string url, MediaFileType fileType, AudioFormat format);

  ~AudioRecorderImpl() override;

  void release();

  bool isRecording() const override;

  DataCallbackResult
  onAudioReady(AudioStream* stream, void* audioData, AudioFrameCount numFrames) override;

  /**
   * Handles one event of the recorder's message thread.
   *
   * @param msg the event to handle, whose `what` is one of the `MSG_*` constants.
   * @return always true, i.e. the event is never redelivered.
   */
  bool handleMessage(Message* msg) override;

protected:
  void onPrepare() override;

  void onPrepareAsync() override;

  void onStartRecording(int64_t timeUs, int64_t durationUs) override;

  void onStop() override;

private:
  /** Starts the thread the file work runs on, if it is not running yet. */
  void startMessageThread();

  /** Quits the thread the file work runs on and releases it. */
  void stopMessageThread();

  /** Sends one event to the message thread, and reports whether it was accepted. */
  bool sendEvent(int what);

  /** Sends one event to the message thread, to be handled once the delay elapsed. */
  bool sendEventDelayed(int what, TimeDelta delay);

  /** Whether the calling thread is the message thread of the recorder. */
  [[nodiscard]] bool isOnMessageThread() const;

  /**
   * Asks the message thread to write the frames the ring buffer holds.
   *
   * At most one write event is queued at a time, because handling one writes everything that is
   * buffered: the events that are not needed would only make the queue longer than the audio.
   */
  void requestWriteEvent();

  /** Writes everything the ring buffer holds to the file. Runs on the message thread. */
  void writePendingAudioData();

  void startInternal();

  void stopInternal();

  bool initAudioFileAndStream(Error* error);

  void storeAudioData(const AudioStream* stream, const void* audioData, AudioFrameCount numFrames);

  /**
   * Turns one block of the ring buffer into the frames of the file and writes it.
   *
   * @return whether a block was written, i.e. whether another pass would make progress.
   */
  bool resampleAndWriteToFile();

  bool hasRemainingAudioData() const;

  void notifyRecordingFinish(bool successfully);

  void notifyRecordingFailed(const Error* error);

  Error* getOutError() const {
    if (mError) {
      mError->clear();
    }
    return mError.get();
  }

  static void handleError(Error* outError, const ErrCode errCode, const std::string& errorMsg) {
    handleError(outError, errCode, errorMsg, "");
  }

  static void handleError(Error* outError, ErrCode errCode, const std::string& errorMsg,
    const std::string& description);

  static void logError(ErrCode errCode, const std::string& errorMsg, const std::string& description = "");

  static constexpr uint32_t kDefaultFrameSize = 1152;
  static constexpr int32_t kMaxCacheDuration = 1;

  /** Writes the frames the ring buffer holds. Sent by the capture callback. */
  static constexpr int MSG_WRITE_AUDIO = 0;
  /** Prepares the capture stream and the file. Sent by `prepareAsync`. */
  static constexpr int MSG_PREPARE = 1;
  /** Starts the capture stream, after the delay of a scheduled recording. */
  static constexpr int MSG_START_RECORDING = 2;
  /** Stops the capture stream, writes what is buffered and closes the file. */
  static constexpr int MSG_STOP_RECORDING = 3;

  /** Released by the stop event once the file was written and closed. */
  CountDownLatch mStopBarrier{ 0 };

  std::unique_ptr<Error> mError;
  std::shared_ptr<AudioFile> mAudioFile;
  std::shared_ptr<AudioStream> mAudioStream;
  std::unique_ptr<AudioPCMBuffer> mPcmBuffer;
  std::unique_ptr<Resample> mResample;

  std::vector<uint8_t> mAppBuffer;
  AudioRingBuffer mRingBuffer;
  AudioSampleFormat mCaptureFormat = AudioSampleFormat::kInvalid;

  /** The thread the file work runs on, and the handler its events are posted through. */
  std::unique_ptr<EventThread> mThread;
  std::shared_ptr<EventHandler::Callback> mCallbackRef;
  std::shared_ptr<EventHandler> mHandler;
  /** Guards `mHandler`, which a caller thread replaces while the message thread posts. */
  mutable std::mutex mHandlerLock;
  /** Whether a write event is already queued, so that only one is ever pending. */
  std::atomic_bool mWriteScheduled{ false };
};

} // namespace coral

#endif // CORAL_AUDIO_IO_AUDIO_RECORDER_H
