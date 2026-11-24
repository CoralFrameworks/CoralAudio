/*
 * Copyright (C) 2026 The Coral Project
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

#ifndef CORAL_AUDIO_LINUX_AUDIO_STREAM_ALSA_H
#define CORAL_AUDIO_LINUX_AUDIO_STREAM_ALSA_H

#include "private/audio/AudioStream.h"
#include "private/audio/AudioStreamBuilder.h"
#include "private/common/AudioRingBuffer.h"
#include "private/common/AudioThreadPriority.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace coral {

/**
 * An `AudioStream` implemented on top of ALSA.
 *
 * The stream supports both directions and both ways of moving data:
 *
 *  - `Direction::Render` writes the application's frames to a playback PCM;
 *  - `Direction::Capture` reads frames from a capture PCM.
 *
 * With a data callback the worker thread calls the application once per period (or once per
 * requested callback size), which is the mode the graph and the player use. Without one,
 * `write()`/`read()` move frames through an `AudioRingBuffer` and the worker thread only copies
 * between that buffer and the device, which is the mode a simple client uses.
 *
 * Threading model
 * ---------------
 * One worker thread owns the PCM device. `request*()` only records the desired state and wakes the
 * worker; it never touches ALSA. The worker applies the command and, while the stream is started,
 * services one period at a time. That is why the state machine needs no lock around the PCM: only
 * the worker runs the device.
 *
 * The worker raises itself through `AudioThreadPriority` when the stream starts and drops the
 * priority when it pauses, stops or exits. Real-time scheduling is best effort: a process without
 * `RLIMIT_RTPRIO`/`CAP_SYS_NICE` still plays, just with ordinary scheduling.
 *
 * Format handling
 * ---------------
 * ALSA is asked for the sample rate, channel count and format the application requested, and the
 * stream reports what the device actually accepted. When the device cannot express a field exactly
 * (the common case for an unsupported rate), ALSA's `_near` helpers choose the closest supported
 * value and `getSampleRate()`, `getChannelCount()` and `getFormat()` report that value, so callers
 * can adapt as they do for the other platforms.
 *
 * The PCM device defaults to `default`. `CORAL_ALSA_DEVICE` overrides it, which is useful for
 * running against the `null` plugin on a machine with no sound hardware.
 */
class AudioStreamAlsa final : public AudioStream {
public:
  /** Creates a stream that is not opened yet; `openStream()` calls `open()` on it. */
  static std::shared_ptr<AudioStream> Create(const AudioStreamBuilder& builder);

  explicit AudioStreamAlsa(const AudioStreamBuilder& builder) noexcept;

  ~AudioStreamAlsa() override;

  AudioStreamAlsa(const AudioStreamAlsa&) = delete;
  AudioStreamAlsa& operator=(const AudioStreamAlsa&) = delete;

  Result open() override;

  /** Stops the worker and closes the PCM, leaving the object re-openable. */
  Result release() override;

  /** Stops the worker, closes the PCM and marks the stream closed. */
  Result close() override;

  Result requestStart() override;

  Result requestPause() override;

  Result requestFlush() override;

  Result requestStop() override;

  StreamState getState() override;

  Result waitForStateChange(StreamState inputState, StreamState* nextState, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> write(const void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> read(void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> setBufferSizeInFrames(uint32_t requestedFrames) override;

  OpResult<uint32_t> getXRunCount() override;

  [[nodiscard]] bool isXRunCountSupported() const override;

  Result getTimestamp(ClockMode mode, int64_t* framePosition, int64_t* timeNanoseconds) override;

  OpResult<double> calculateLatencyMillis() override;

  [[nodiscard]] void* getUnderlyingStream() const override {
    return mPcm;
  }

protected:
  void updateFramesWritten() override;
  void updateFramesRead() override;

private:
  enum class Command : uint8_t {
    None,
    Start,
    Pause,
    Flush,
    Stop,
  };

  // ------------------------------------------------------------------ lifecycle

  Result openDevice();
  Result configureDevice();
  Result startWorker();
  void stopWorker();

  void workerLoop();
  void applyStart();
  void applyPause();
  void applyFlush();
  void applyStop();

  // ------------------------------------------------------------------ data path

  void pumpRender();
  void pumpCapture();

  /** Waits up to @p timeoutMillis for the PCM to become ready; handles a device error. */
  void waitForDevice(int timeoutMillis);

  /**
   * Handles an ALSA error from the worker thread.
   *
   * @return true when the error was an xrun or a suspended device that was recovered, in which
   *         case the caller can keep going; false when the stream was moved to Disconnected.
   */
  bool handleAlsaError(int error, const char* operation);

  void reportDeviceError(Result error, const char* operation);

  void setState(StreamState state);
  void notifyBufferWaiters();

  /** Raises the worker to real-time priority. Called on the worker thread. */
  void promoteWorker();

  /** Restores the worker's previous priority. Called on the worker thread. */
  void demoteWorker();

  // ------------------------------------------------------------------ helpers

  [[nodiscard]] bool isRender() const {
    return getDirection() == Direction::Render;
  }

  [[nodiscard]] bool isCallbackMode() const {
    return isDataCallbackSpecified();
  }

  /** The largest number of frames one data callback is given. */
  [[nodiscard]] uint32_t callbackChunkFrames(uint32_t frames) const;

  size_t pushToRingBuffer(const uint8_t* source, size_t frames);
  size_t popFromRingBuffer(uint8_t* destination, size_t frames);
  void resetRingBuffer();

  static snd_pcm_format_t pcmFormatFor(AudioSampleFormat format);
  static Result resultFromAlsa(int error);

  /** The device name, from `CORAL_ALSA_DEVICE` when set, or `default`. */
  static const char* deviceName();

  // ------------------------------------------------------------------ device

  snd_pcm_t* mPcm = nullptr;

  /** Period size, in frames: the amount the device consumes or produces per wake-up. */
  uint32_t mDevicePeriodFrames = 0;

  /** Ring/device buffer size, in frames, as accepted by ALSA. */
  uint32_t mDeviceBufferFrames = 0;

  /** One period in the stream's sample format, reused by the worker. */
  std::vector<uint8_t> mScratch;


  // ------------------------------------------------------------------ worker

  std::thread mWorker;
  std::mutex mLock;
  std::condition_variable mCommandAvailable;
  std::condition_variable mStateChanged;

  Command mCommand = Command::None;
  bool mExitRequested = false;

  std::atomic<StreamState> mState{ StreamState::Uninitialized };

  /** Real-time scheduling state, valid only on the worker thread. */
  AudioThreadPriority::Handle mThreadPriority;

  // ------------------------------------------------------------------ buffering

  AudioRingBuffer mRingBuffer;

  /** Guards the ring buffer's counters and the two condition variables. */
  std::mutex mBufferLock;
  std::condition_variable mBufferSpaceAvailable;
  std::condition_variable mBufferDataAvailable;

  // ------------------------------------------------------------------ state

  /** Serializes `open()`, `release()` and `close()` so the worker is created or joined once. */
  std::mutex mLifecycleLock;

  /** The values the application asked for, kept because `open()` may be called again. */
  uint32_t mRequestedFramesPerCallback = 0;
  uint32_t mRequestedBufferCapacity = 0;
  uint64_t mRequestedSampleRate = 0;
  uint32_t mRequestedChannelCount = 0;
  AudioSampleFormat mRequestedFormat = AudioSampleFormat::kDefault;

  /** Offset that turns CLOCK_MONOTONIC into CLOCK_REALTIME, captured when the device opens. */
  int64_t mRealtimeOffsetNanos = 0;

  std::atomic<uint32_t> mXRunCount{ 0 };

  /** Set once, so the application sees at most one error notification per open. */
  std::atomic<bool> mFatalErrorReported{ false };
};

} // namespace coral

#endif //CORAL_AUDIO_LINUX_AUDIO_STREAM_ALSA_H
