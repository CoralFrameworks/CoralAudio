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

#ifndef CORAL_AUDIO_DARWIN_AUDIO_STREAM_CORE_AUDIO_H
#define CORAL_AUDIO_DARWIN_AUDIO_STREAM_CORE_AUDIO_H

#include "private/audio/AudioStream.h"
#include "private/audio/AudioStreamBuilder.h"
#include "private/common/AudioRingBuffer.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace coral {

/**
 * An `AudioStream` implemented on top of Core Audio's AUHAL output unit.
 *
 * The stream supports both directions with one HAL output audio unit, which is the Core Audio
 * object that gives a process direct access to an input or output device:
 *
 *  - `Direction::Render` leaves the output element of the unit enabled and renders into the
 *    callback's buffer list;
 *  - `Direction::Capture` enables the input element, disables the output one, and pulls the
 *    captured frames with `AudioUnitRender()` from the input callback.
 *
 * And both ways of moving data:
 *
 *  - **callback driven**: when the builder supplied an `AudioStreamDataCallback`, the HAL thread
 *    calls the application once per device buffer (or once per requested callback size). This is
 *    the mode the graph, the player and the recorder use;
 *  - **blocking**: when no data callback was supplied, the HAL callback moves frames between the
 *    device and an `AudioRingBuffer`, and `write()`/`read()` move them between that buffer and the
 *    caller. Waiting for space or data is bounded by the caller's timeout.
 *
 * Threading model
 * ---------------
 * Unlike the ALSA and WASAPI backends there is no worker thread pumping the device: Core Audio owns
 * the IO thread and calls `onRender()`/`onInput()` on it, at real-time priority. The
 * only thread this class creates is a control thread that applies the `request*()` commands:
 * `AudioOutputUnitStart`, `AudioOutputUnitStop` and `AudioUnitReset` are not called from the
 * application's data callback, because stopping a unit from its own IO thread can deadlock the HAL.
 *
 * `request*()` publishes the state and wakes the control thread; the `start()`, `pause()`,
 * `flush()` and `stop()` helpers inherited from `AudioStream` block until the state actually
 * changed. The audio unit is created, configured and initialized in `open()`; it is disposed in
 * `release()` (leaving the object re-openable) and in `close()` (leaving it closed).
 *
 * Format handling
 * ---------------
 * The client side of the unit (input scope of element 0 for render, output scope of element 1 for
 * capture) is set to the requested sample rate, channel count and sample format. Core Audio
 * converts between that client format and the device's own format, including sample rate: the
 * device can stay at 48 kHz while the application renders a 44.1 kHz file. `getSampleRate()`,
 * `getChannelCount()` and `getFormat()` report the format of the buffers the callbacks receive,
 * which is that client format, so callers can adapt exactly as they do for the other platforms.
 *
 * A rate or channel count the builder left unspecified is taken from the device, and the sample
 * format defaults to float. The device buffer size is left at the value the system chose, so the
 * stream does not change the settings of a device other applications are using; the frames per
 * burst it reports is that device buffer size, and `FramesPerDataCallback` larger than one burst is
 * split into one callback per burst.
 *
 * `CORAL_COREAUDIO_DEVICE` selects a device by name or UID instead of the system default, which is
 * useful for running the tests against a specific output or input device.
 */
class AudioStreamCoreAudio final : public AudioStream {
public:
  /** Creates a stream that is not opened yet; `openStream()` calls `open()` on it. */
  static std::shared_ptr<AudioStream> Create(const AudioStreamBuilder& builder);

  explicit AudioStreamCoreAudio(const AudioStreamBuilder& builder) noexcept;

  ~AudioStreamCoreAudio() override;

  AudioStreamCoreAudio(const AudioStreamCoreAudio&) = delete;
  AudioStreamCoreAudio& operator=(const AudioStreamCoreAudio&) = delete;

  Result open() override;

  /** Stops the control thread, drops the unit and the device, leaving the object re-openable. */
  Result release() override;

  /** Stops the control thread, drops the unit and marks the stream closed. */
  Result close() override;

  Result requestStart() override;

  Result requestPause() override;

  Result requestFlush() override;

  Result requestStop() override;

  StreamState getState() override;

  Result waitForStateChange(StreamState inputState, StreamState* nextState, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> write(const void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> read(void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  /**
   * The device buffer size is chosen by the system, so the size in use is reported rather than
   * resized. Not a failure: the caller learns the real value either way.
   */
  OpResult<uint32_t> setBufferSizeInFrames(uint32_t requestedFrames) override;

  /**
   * Counts the buffers that had to be filled with silence because the application did not keep up
   * (render), or whose frames were discarded because it did not read in time (capture). Only the
   * blocking mode can observe those events; in callback mode the HAL thread calls the application
   * directly, so a missed deadline is invisible to this stream.
   */
  OpResult<uint32_t> getXRunCount() override;

  [[nodiscard]] bool isXRunCountSupported() const override;

  Result getTimestamp(ClockMode mode, int64_t* framePosition, int64_t* timeNanoseconds) override;

  OpResult<double> calculateLatencyMillis() override;

  [[nodiscard]] void* getUnderlyingStream() const override {
    return mAudioUnit;
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

  Result createUnit();
  Result configureFormat();
  Result configureCallbacks();
  Result initializeUnit();
  Result startControlThread();
  void stopControlThread();

  /** Disposes the device listener, the audio unit and everything that belongs to an open stream. */
  void teardownDevice();

  // ------------------------------------------------------------------ control thread

  void controlLoop();
  void applyStart();
  void applyPause();
  void applyFlush();
  void applyStop();

  // ------------------------------------------------------------------ HAL callbacks

  OSStatus onRender(AudioUnitRenderActionFlags* flags,
    const AudioTimeStamp* timestamp,
    uint32_t numFrames,
    ::AudioBufferList* ioData);

  OSStatus onInput(AudioUnitRenderActionFlags* flags,
    const AudioTimeStamp* timestamp,
    uint32_t numFrames);

  static OSStatus renderCallbackEntry(void* refCon,
    AudioUnitRenderActionFlags* flags,
    const AudioTimeStamp* timestamp,
    UInt32 busNumber,
    UInt32 numFrames,
    ::AudioBufferList* ioData);

  static OSStatus inputCallbackEntry(void* refCon,
    AudioUnitRenderActionFlags* flags,
    const AudioTimeStamp* timestamp,
    UInt32 busNumber,
    UInt32 numFrames,
    ::AudioBufferList* ioData);

  static OSStatus deviceListenerEntry(AudioObjectID objectId,
    UInt32 numberAddresses,
    const AudioObjectPropertyAddress* addresses,
    void* clientData);

  void onDevicePropertyChanged(const AudioObjectPropertyAddress& address);

  // ------------------------------------------------------------------ errors and state

  void reportDeviceError(Result error, const char* operation);

  void setState(StreamState state);
  void notifyBufferWaiters();

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

  [[nodiscard]] int64_t hostTimeToNanos(uint64_t hostTime) const;

  [[nodiscard]] UInt32 deviceLatencyFrames() const;

  static Result resultFromOSStatus(OSStatus status, const char* operation);

  static AudioStreamBasicDescription streamDescriptionFor(uint64_t sampleRate,
    uint32_t channelCount,
    AudioSampleFormat format);

  /** The default output or input device, or `CORAL_COREAUDIO_DEVICE` when it names one. */
  static AudioDeviceID selectDeviceFor(Direction direction);

  static uint32_t channelCountForDevice(AudioDeviceID device, Direction direction);

  static Float64 sampleRateForDevice(AudioDeviceID device);

  static uint32_t bufferFramesForDevice(AudioDeviceID device);

  // ------------------------------------------------------------------ unit and device

  AudioUnit mAudioUnit = nullptr;
  AudioDeviceID mDevice = kAudioObjectUnknown;

  /** Whether the device-is-alive listener is installed for `mDevice`. */
  bool mDeviceListenerInstalled = false;

  /** Device buffer size, in frames: how many frames one HAL callback carries. */
  uint32_t mDeviceBufferFrames = 0;

  /** Frame position and host time of the last HAL callback, for `getTimestamp()`. */
  std::atomic<int64_t> mTimestampFramePosition{ 0 };
  std::atomic<uint64_t> mTimestampHostTime{ 0 };

  /** One HAL callback in the stream's client format, reused by the capture path. */
  std::vector<uint8_t> mCaptureBuffer;
  ::AudioBufferList mCaptureBufferList{};

  /** Mach timebase, captured when the stream opens, to turn host times into nanoseconds. */
  uint32_t mTimebaseNumer = 1;
  uint32_t mTimebaseDenom = 1;

  /** Offset that turns the Mach monotonic clock into `AudioUtils::getNanoseconds()`. */
  int64_t mMonotonicOffsetNanos = 0;

  /** Offset that turns the monotonic clock into the realtime clock, captured at open. */
  int64_t mRealtimeOffsetNanos = 0;

  // ------------------------------------------------------------------ control thread

  std::thread mControlThread;
  std::mutex mLock;
  std::condition_variable mCommandAvailable;
  std::condition_variable mStateChanged;

  Command mCommand = Command::None;
  bool mExitRequested = false;

  std::atomic<StreamState> mState{ StreamState::Uninitialized };

  /** True from the moment the HAL is started until it is stopped; guards the callback paths. */
  std::atomic<bool> mDeviceRunning{ false };

  // ------------------------------------------------------------------ buffering

  AudioRingBuffer mRingBuffer;

  std::mutex mBufferLock;
  std::condition_variable mBufferSpaceAvailable;
  std::condition_variable mBufferDataAvailable;

  std::atomic<uint32_t> mXRunCount{ 0 };
  std::atomic<bool> mFatalErrorReported{ false };

  // ------------------------------------------------------------------ state

  /** Serializes `open()`, `release()` and `close()` so the control thread is created or joined once. */
  std::mutex mLifecycleLock;

  /** The values the application asked for, kept because `open()` may be called again. */
  uint32_t mRequestedFramesPerCallback = 0;
  uint32_t mRequestedBufferCapacity = 0;
  uint64_t mRequestedSampleRate = 0;
  uint32_t mRequestedChannelCount = 0;
  AudioSampleFormat mRequestedFormat = AudioSampleFormat::kDefault;
};

} // namespace coral

#endif //CORAL_AUDIO_DARWIN_AUDIO_STREAM_CORE_AUDIO_H
