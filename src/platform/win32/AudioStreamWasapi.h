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

#ifndef CORAL_AUDIO_WIN32_AUDIO_STREAM_WASAPI_H
#define CORAL_AUDIO_WIN32_AUDIO_STREAM_WASAPI_H

// The Windows headers have to be pulled in first, and with these two switches:
// `NOMINMAX` keeps `windows.h` from defining `min`/`max` macros that break
// `std::min`/`std::max` used by the audio base classes, and `WIN32_LEAN_AND_MEAN`
// keeps the translation unit from dragging in the whole of `windows.h`.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include "private/audio/AudioStream.h"
#include "private/audio/AudioStreamBuilder.h"
#include "private/common/AudioRingBuffer.h"
#include "private/common/AudioThreadPriority.h"

#include <core/event/EventHandler.h>
#include <core/event/EventThread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace coral {

/**
 * An `AudioStream` implemented on top of WASAPI (Windows Audio Session API).
 *
 * The stream supports both directions:
 *
 *  - `Direction::Render` plays audio through the default playback endpoint using
 *    an `IAudioRenderClient`;
 *  - `Direction::Capture` records audio from the default recording endpoint using
 *    an `IAudioCaptureClient`.
 *
 * And both ways of moving data:
 *
 *  - **callback driven**: when the builder supplied a `AudioStreamDataCallback`,
 *    the stream thread asks the application for the frames of every device buffer
 *    (render) or hands it the frames the device just captured (capture);
 *  - **blocking**: when no data callback was supplied, `write()` fills and `read()`
 *    drains an `AudioRingBuffer` that the stream thread moves between the
 *    application and the device. Waiting for space or for data is bounded by the
 *    timeout the caller passes in.
 *
 * Threading model
 * ---------------
 * The device is never touched from the caller's thread. Every device operation is
 * a message handled on an `EventThread`'s `EventHandler`:
 *
 *  - the **stream thread** owns the device: it services one buffer per `MSG_PUMP`,
 *    which it reschedules itself for, and applies the `MSG_START`, `MSG_PAUSE`,
 *    `MSG_FLUSH` and `MSG_STOP` commands. Because a single thread performs every
 *    transition, the state machine needs no locking of its own;
 *  - the **error thread** exists only when the builder supplied an error callback.
 *    A device failure is reported there rather than on the stream thread, because
 *    the documented reaction to an error is to stop and close the stream, and
 *    closing joins the stream thread.
 *
 * `request*()` therefore only publishes the desired state and posts a message; it
 * never waits for the device. `start()`, `pause()`, `flush()` and `stop()` inherit
 * from `AudioStream` and block until the state actually changed.
 *
 * Format handling
 * ---------------
 * Shared mode is the common case and it is opened with `AUTOCONVERTPCM`, so the
 * audio engine converts between the format the application asked for and the one
 * the endpoint mixes. When that is refused, the stream falls back to the endpoint
 * mix format and reports it through `getSampleRate()`, `getChannelCount()` and
 * `getFormat()`, exactly like the other platform implementations. Exclusive mode
 * is attempted first when `SharingMode::Exclusive` was requested and silently
 * degrades to shared when the endpoint cannot be opened exclusively.
 *
 * Samples are exchanged in the format `getFormat()` reports, which is the container
 * the device moves: an endpoint that mixes 24 valid bits inside a 32-bit container is
 * reported as `AudioSampleFormat::kInt32`, and the caller hands over the 32-bit
 * containers the driver expects, with the valid bits in the most significant ones,
 * which is what `wValidBitsPerSample` describes.
 */
class AudioStreamWasapi final : public AudioStream, public EventHandler::Callback {
public:
  /**
   * Creates a stream that is not opened yet. `AudioStreamBuilder::openStream()`
   * calls this and then `open()`.
   */
  static std::shared_ptr<AudioStream> Create(const AudioStreamBuilder& builder);

  explicit AudioStreamWasapi(const AudioStreamBuilder& builder) noexcept;

  ~AudioStreamWasapi() override;

  AudioStreamWasapi(const AudioStreamWasapi&) = delete;
  AudioStreamWasapi& operator=(const AudioStreamWasapi&) = delete;

  Result open() override;

  /**
   * Stops the device and releases every WASAPI interface, but leaves this object
   * re-openable: the stream returns to `StreamState::Uninitialized`.
   */
  Result release() override;

  /**
   * Releases the device and marks the stream closed. A closed stream cannot be
   * opened again.
   */
  Result close() override;

  Result requestStart() override;

  Result requestPause() override;

  Result requestFlush() override;

  Result requestStop() override;

  OpResult<uint32_t> write(const void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> read(void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  /**
   * WASAPI fixes the device buffer when the client is initialized, so this reports
   * the size actually in use rather than resizing it.
   */
  OpResult<uint32_t> setBufferSizeInFrames(uint32_t requestedFrames) override;

  uint32_t getBufferSizeInFrames() override;

  /**
   * Counts the buffers the device ran out of (render) or that had to be dropped
   * because the application did not read in time (capture).
   *
   * Only the blocking mode can observe those events, because in callback mode the
   * engine owns the buffer and a missed deadline is invisible to this stream.
   */
  OpResult<uint32_t> getXRunCount() override;

  bool isXRunCountSupported() const override;

  StreamState getState() override;

  Result waitForStateChange(StreamState inputState, StreamState* nextState, int64_t timeoutNanoseconds) override;

  Result getTimestamp(ClockMode mode, int64_t* framePosition, int64_t* timeNanoseconds) override;

  OpResult<double> calculateLatencyMillis() override;

  void* getUnderlyingStream() const override {
    return mAudioClient.Get();
  }

  /**
   * Dispatches a message on the stream thread or on the error thread. Both
   * handlers share this callback object, so the message codes below are unique
   * across the two.
   */
  bool handleMessage(Message* msg) override;

protected:
  /**
   * The frame counters are maintained by the thread that owns each one:
   *
   *  - render: frames submitted to the device are counted by the stream thread, and
   *    frames actually played by the endpoint are read from the device clock right
   *    here, because only the clock knows what left the pipeline;
   *  - capture: frames received from the device are counted by the stream thread and
   *    frames consumed by the application are counted by `read()` or by the data
   *    callback, so neither counter needs refreshing.
   */
  void updateFramesWritten() override;

  void updateFramesRead() override;

private:
  /** Messages handled on the stream thread and on the error thread. */
  enum : int {
    /** Services one device buffer and reschedules itself while the stream runs. */
    MSG_PUMP = 1,
    /** Starts the device. */
    MSG_START = 2,
    /** Pauses the device without discarding its buffer. */
    MSG_PAUSE = 3,
    /** Discards the buffered audio. */
    MSG_FLUSH = 4,
    /** Stops and resets the device. */
    MSG_STOP = 5,
    /** Delivers a device failure to the application's error callback. */
    MSG_NOTIFY_ERROR = 6,
  };

  // ------------------------------------------------------------------ opening

  /** Opens the default endpoint for the stream direction and activates its client. */
  Result openDevice();

  /**
   * Picks the device format: the application's request where it is specified, and
   * the endpoint mix format for every field the application left unspecified.
   */
  Result negotiateFormat();

  /** Initializes the audio client, falling back from exclusive to shared mode. */
  Result initializeClient();

  /** Sets the event handle and creates the render/capture client and the clock. */
  Result createStreamResources();

  /** Prepares the format fields that the stream reports to the application. */
  void adoptDeviceFormat(const WAVEFORMATEX& format);

  /** Fills in a `WAVEFORMATEXTENSIBLE` for a sample rate, channel count and format. */
  static void fillFormat(WAVEFORMATEXTENSIBLE* out,
    uint32_t sampleRate,
    uint32_t channels,
    AudioSampleFormat format,
    DWORD channelMask);

  /** Converts an endpoint mix format into the equivalent Coral sample format. */
  static AudioSampleFormat formatFromWaveFormat(const WAVEFORMATEX& format);

  // -------------------------------------------------------------- event thread

  /** Creates the stream thread and, when an error callback is set, the error thread. */
  Result startEventHandler();

  /** Quits and joins both event threads. Must not be called from either of them. */
  void quitEventHandler();

  /** Releases the WASAPI interfaces. Must be called after the threads are joined. */
  void releaseDevice();

  /** The teardown shared by `close()` and `release()`. */
  void teardown();

  // ---------------------------------------------------------------- device pump

  /** Publishes a state change and wakes up `waitForStateChange()`. */
  void setState(StreamState state);

  /** Publishes a state change. The caller holds `mLock`. */
  void setState_l(StreamState state);

  /**
   * Whether the state machine is still waiting for the command being handled.
   *
   * A command that a later request superseded is dropped instead of being applied,
   * which is what keeps a start and an immediate stop from fighting each other.
   */
  [[nodiscard]] bool isCommandExpected(StreamState expected);

  /** Clears the ring buffer and wakes up every caller waiting on it. */
  void resetRingBuffer();

  /** Posts `MSG_PUMP` unless the pump is already scheduled or the stream is not open. */
  void schedulePump();

  /** One iteration of the stream thread: wait for the device, service it, reschedule. */
  void pump();

  /** Services the render buffer. */
  void pumpRender();

  /** Drains every packet the capture client has ready. */
  void pumpCapture();

  /**
   * The frames of one chunk of the device buffer being serviced.
   *
   * The data callback is never handed more frames than the stream published through
   * `getFramesPerDataCallback()`: a device buffer that is larger than that is served by several
   * callbacks instead of by one oversized buffer. A stream that did not ask for a callback size
   * is served the device burst, which is a bound rather than a promise as well: a pump that
   * finds less space than one chunk hands over the frames it found.
   *
   * @param bufferFrames the frames the device buffer or the capture packet covers
   * @return the frames of one chunk, which is `bufferFrames` when the whole buffer already fits
   *         in a single callback
   */
  [[nodiscard]] uint32_t callbackChunkFrames(uint32_t bufferFrames) const;

  void onStart();

  void onPause();

  void onFlush();

  void onStop();

  /** Runs on the error thread: delivers the pending failure to the application. */
  void onNotifyError();

  /**
   * Records a device failure and schedules the notification. Called from the stream
   * thread when a WASAPI call reports that the device is gone or unusable.
   */
  void reportDeviceError(Result error, const char* reason);

  /** Raises the stream thread to real-time priority. Called on the stream thread. */
  void promotePumpThread();

  /** Restores the stream thread's priority. Called on the stream thread. */
  void demotePumpThread();

  // ------------------------------------------------------------------ data path

  /** Copies `frames` frames out of `source` into the ring buffer. Returns the frames stored. */
  size_t pushToRingBuffer(const uint8_t* source, size_t frames);

  /** Copies up to `frames` frames out of the ring buffer into `destination`. */
  size_t popFromRingBuffer(uint8_t* destination, size_t frames);

  /** Whether the application supplied a data callback instead of using read/write. */
  [[nodiscard]] bool isCallbackMode() const {
    return isDataCallbackSpecified();
  }

  /** Whether this stream plays audio (`true`) or records it (`false`). */
  [[nodiscard]] bool isRender() const {
    return getDirection() == Direction::Render;
  }

  static Result resultFromHresult(HRESULT hr);

  /** The number of frames a device period of `hns` covers at `sampleRate`. */
  static uint32_t framesFromHns(REFERENCE_TIME hns, uint32_t sampleRate);

  /** The duration in 100-nanosecond units of `frames` frames at `sampleRate`. */
  static REFERENCE_TIME hnsFromFrames(uint32_t frames, uint32_t sampleRate);

  /**
   * Reads the device clock.
   *
   * @param framePosition receives the frames the endpoint has played or captured, in the
   *                      timeline of the stream: the number keeps growing across a client
   *                      reset, which restarts the device clock at zero.
   * @param qpc           receives the performance counter value the position belongs to, or
   *                      zero while the endpoint has not recorded a position yet.
   * @return `Result::OK`, or the failure of the device clock.
   *
   * @discussion The caller holds `mClientLock`. `getTimestamp()` and `updateFramesRead()`
   * both read the clock through here, so the two report one basis rather than two: the
   * position is scaled from device clock ticks to frames at the stream's rate in one place.
   */
  Result readDeviceClock(int64_t& framePosition, uint64_t& qpc);

  /** Converts a performance counter value into nanoseconds since boot. */
  static int64_t qpcToNanos(uint64_t qpc);

  /** Converts a performance counter value into nanoseconds on the realtime clock. */
  static int64_t qpcToRealtimeNanos(uint64_t qpc);

  /** Whether the calling thread already has COM initialized for WASAPI. */
  static void ensureComInitialized();

  /** The stream thread and the handler that owns the device. */
  std::unique_ptr<EventThread> mThread;
  /**
   * A non-owning reference to this stream, kept alive for as long as a handler may
   * dispatch to it.
   *
   * A handler holds its callback weakly, so the reference has to be kept alive here.
   * It does not own the stream — that belongs to whoever called
   * `AudioStreamBuilder::openStream()` — and the no-op deleter is what keeps it from
   * destroying the stream a second time. Declared before the handlers, so it outlives
   * them.
   */
  std::shared_ptr<EventHandler::Callback> mCallbackRef;
  std::shared_ptr<EventHandler> mHandler;

  /** The error thread and its handler. Only created when an error callback is set. */
  std::unique_ptr<EventThread> mErrorThread;
  std::shared_ptr<EventHandler> mErrorHandler;

  // ------------------------------------------------------------------- WASAPI

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> mDeviceEnumerator;
  Microsoft::WRL::ComPtr<IMMDevice> mDevice;
  Microsoft::WRL::ComPtr<IAudioClient> mAudioClient;
  Microsoft::WRL::ComPtr<IAudioRenderClient> mRenderClient;
  Microsoft::WRL::ComPtr<IAudioCaptureClient> mCaptureClient;
  Microsoft::WRL::ComPtr<IAudioClock> mAudioClock;

  /** Signalled by the audio engine when a buffer is ready to be serviced. */
  HANDLE mAudioEvent = nullptr;

  /** The format the device was initialized with. */
  WAVEFORMATEXTENSIBLE mDeviceFormat{};

  /** `true` when the audio engine performs the format conversion. */
  bool mEngineConverts = false;

  /** Shared mode or exclusive mode, as actually established. */
  SharingMode mEstablishedSharingMode = SharingMode::Shared;

  /** How long the stream thread is willing to wait for the device, in milliseconds. */
  uint32_t mPumpTimeoutMillis = 20;

  /** Guards the WASAPI interfaces while a member function is using them. */
  mutable std::shared_mutex mClientLock;

  /**
   * Counts the times the device interfaces were released.
   *
   * A callback runs with `mClientLock` dropped, so the stream can be released and opened
   * again from inside it. The interfaces it would find afterwards belong to another device
   * buffer, which the frames of the block being served do not describe: the pump compares
   * this counter across the callback and drops the block when it changed.
   */
  std::atomic<uint64_t> mClientGeneration{ 0 };

  // -------------------------------------------------------------------- state

  /** The device buffer size in frames, which WASAPI fixes at initialization. */
  uint32_t mDeviceBufferFrames = 0;

  /** The buffer capacity the application asked for, before the device was consulted. */
  uint32_t mRequestedCapacityInFrames = kUnspecified;

  /**
   * The callback frame count the application asked for, before the device was consulted.
   *
   * `mFramesPerCallback` is overwritten with the burst when nothing was asked for, so the
   * request is remembered here for the next `open()` of a released stream.
   */
  AudioFrameCount mRequestedFramesPerCallback = kUnspecified;

  /**
   * The format the application asked for, before the device was consulted.
   *
   * What the stream reports describes the device it is opened on, and a released stream reports
   * none of it, so the request is kept here instead: `negotiateFormat()` reads it on every
   * `open()`, which is what makes a stream that is opened again describe the same device as the
   * first time rather than whatever the endpoint happens to be mixing.
   */
  uint64_t mRequestedSampleRate = kUnspecified;
  uint32_t mRequestedChannelCount = kUnspecified;
  AudioSampleFormat mRequestedFormat = AudioSampleFormat::kDefault;

  /**
   * The sharing mode the application asked for.
   *
   * What the stream reports is the mode that was actually established, so the request is kept
   * here and consulted on every `open()`: a released stream tries exclusive mode again instead
   * of remembering the shared mode it fell back to.
   */
  SharingMode mRequestedSharingMode = SharingMode::Shared;

  std::atomic<StreamState> mState{ StreamState::Uninitialized };

  /** Wakes `waitForStateChange()` up when the stream thread moves the state machine. */
  std::condition_variable mStateChanged;

  /** Set once, so the application sees at most one error notification per stream. */
  std::atomic<bool> mFatalErrorReported{ false };

  /** The failure waiting to be delivered on the error thread. */
  Result mPendingError = Result::OK;

  /** Set while `MSG_PUMP` is queued or running, so the pump is never queued twice. */
  std::atomic<bool> mPumpScheduled{ false };

  /** The ring buffer that decouples the caller from the stream thread in blocking mode. */
  AudioRingBuffer mRingBuffer;

  /** Guards the ring buffer's condition variables. The buffer itself is single producer. */
  std::mutex mBufferLock;
  std::condition_variable mBufferSpaceAvailable;
  std::condition_variable mBufferDataAvailable;

  /** Zeroed frames handed to the device or to the callback when a packet is silent. */
  std::vector<uint8_t> mSilenceBuffer;

  /**
   * Set when a stop or a flush could not reset the client, which can leave audio queued in
   * it. The next start retries the reset before it hands the buffer back to the device.
   *
   * Only the stream thread touches this.
   */
  bool mDeviceBufferStale = false;

  /** Set after an oversized silent capture packet has been reported once. Stream thread only. */
  bool mReportedOversizedSilencePacket = false;

  /** Frames the device ran out of, or that had to be dropped because nobody read them. */
  std::atomic<uint32_t> mXRunCount{ 0 };

  /**
   * The last device clock position seen, and the frames counted before the clock was
   * restarted by a flush or a stop.
   *
   * `IAudioClock::GetPosition()` restarts at zero when the client is reset, and
   * `getFramesRead()` is documented as monotonic, so what the previous clock run
   * reported is accumulated here instead of being lost.
   */
  std::atomic<uint64_t> mLastClockPosition{ 0 };
  std::atomic<int64_t> mFramesPlayedBase{ 0 };

  /** The stream thread's real-time scheduling state, valid while it is promoted. */
  AudioThreadPriority::Handle mThreadPriority;
};

} // namespace coral

#endif //CORAL_AUDIO_WIN32_AUDIO_STREAM_WASAPI_H
