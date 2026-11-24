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

#ifndef CORAL_PRIVATE_AUDIO_STREAM_H
#define CORAL_PRIVATE_AUDIO_STREAM_H

#include <coral/audio/AudioTypes.h>
#include <coral/audio/OpResult.h>

#include "private/audio/AudioStreamBase.h"
#include "private/audio/AudioStreamCallback.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace coral {

class AudioStreamBuilder;

/**
 * Base class for stream to capture or render audio data.
 */
class AudioStream : public AudioStreamBase {
public:
  /**
   * The default number of nanoseconds to wait for when performing state change
   * operations on the stream, such as `start` and `stop`.
   *
   * @see omre::AudioStream::start
   */
  static constexpr auto kDefaultTimeoutNanos = (2000 * Definitions::kNanosPerMillisecond);

  using DataCallback = AudioStreamDataCallback;
  using ErrorCallback = AudioStreamErrorCallback;

  AudioStream() = default;
  explicit AudioStream(const AudioStreamBuilder &builder) noexcept;

  /**
   * Open a stream based on the current settings.
   */
  virtual Result open() {
    return Result::OK;
  }

  /**
   * Free the audio resources associated with a stream created.
   */
  virtual Result release() {
    return Result::ErrorUnimplemented;
  }

  /**
   * Close the stream an deallocate any resources from the open() call.
   */
  virtual Result close();

  /**
   * Start the stream. This will block until the stream has been started, an
   * error occurs or `timeoutNanoseconds` has been reached.
   */
  Result start(int64_t timeoutNanoseconds = kDefaultTimeoutNanos);

  /**
   * Pause the stream. This will block until the stream has been paused, an
   * error occurs or `timeoutNanoseconds` has been reached.
   */
  Result pause(int64_t timeoutNanoseconds = kDefaultTimeoutNanos);

  /**
   * Flush the stream. This will block until the stream has been flushed, an
   * error occurs or `timeoutNanoseconds` has been reached.
   */
  Result flush(int64_t timeoutNanoseconds = kDefaultTimeoutNanos);

  /**
   * Stop the stream. This will block until the stream has been stopped, an
   * error occurs or `timeoutNanoseconds` has been reached.
   */
  Result stop(int64_t timeoutNanoseconds = kDefaultTimeoutNanos);

  /**
   * Start the stream asynchronously.
   */
  virtual Result requestStart() = 0;

  /**
   * Pause the stream asynchronously.
   */
  virtual Result requestPause() = 0;

  /**
   * Flush the stream asynchronously.
   */
  virtual Result requestFlush() = 0;

  /**
   * Stop the stream asynchronously.
   */
  virtual Result requestStop() = 0;

  /**
   * Query the current state
   */
  virtual StreamState getState() = 0;

  /**
   * Write data from the supplied buffer into the stream. This method will block until to write
   * is complete, or it runs out of time.
   *
   * If `timeoutNanoseconds` is zero then this call will not wait.
   *
   * @param buffer             The address of the first sample.
   * @param numFrames          Number of frames to write. Only complete frames will be written.
   * @param timeoutNanoseconds Maximum number of nanoseconds to wait for completion.
   * @return a OpResult which has a result of Result::kOK and a value containing the number
   * of frames actually written, or result of Result::kError*.
   */
  virtual OpResult<uint32_t> write(const void *buffer, uint32_t numFrames, int64_t timeoutNanoseconds) {
    return Result::ErrorUnimplemented;
  }

  /**
   * Read data into the supplied buffer from the stream. This method will block until the read
   * is complete, or it runs out of time.
   *
   * If `timeoutNanoseconds` is zero then this call will not wait.
   *
   * @param buffer             The address of the first sample.
   * @param numFrames          Number of frames to read. Only complete frames will be read.
   * @param timeoutNanoseconds Maximum number of nanoseconds to wait for completion.
   * @return a OpResult which has a result of Result::kOK and a value containing the number
   * of frames actually read, or result of Result::kError*.
   */
  virtual OpResult<uint32_t> read(void *buffer, uint32_t numFrames, int64_t timeoutNanoseconds) {
    return Result::ErrorUnimplemented;
  }

  /**
   * This can be used to adjust the latency of the buffer by changing the threshold where
   * blocking will occur.
   * By combining this with getXRunCount(), the latency can be tuned at run-time for
   * each device.
   *
   * This cannot be set higher than getBufferCapacity().
   *
   * @param requestFrames requested number of frames that can be filled without blocking
   * @return the resulting buffer size in frames (obtained using value()) or an error
   * (obtained using error())
   */
  virtual OpResult<uint32_t> setBufferSizeInFrames(uint32_t requestFrames) {
    return Result::ErrorUnimplemented;
  }

  /**
   * An XRun is an Underrun or an Overrun.
   *
   * During playing, an underrun will occur if the stream is not written in time
   * and the system runs out of valid data.
   * During recording, an overrun will occur if the stream is not read in time
   * and there is no place to put the incoming data so it is discarded.
   *
   * An underrun or overrun can cause an audible "pop" or "glitch".
   *
   * @return a result which is either Result::OK with the xRun count as the
   * value, or a Result::Error* code
   */
  virtual OpResult<uint32_t> getXRunCount() {
    return Result::ErrorUnimplemented;
  }

  /**
   * Returns true if XRun counts are supported on the stream
   */
  virtual bool isXRunCountSupported() const = 0;

  /**
   * Query the number of frames that are read or written by the endpoint at one time.
   *
   * @return the burst size
   */
  [[nodiscard]] uint32_t getFramesPerBurst() const {
    return mFramesPerBurst;
  }

  /**
   * Get the number of bytes in each audio frame. This is calculated using the
   * channel count and the sample format. For example, a 2 channel floating
   * point stream will have 2 * 4 = 8 bytes per frame.
   *
   * @return number of bytes in each audio frame.
   */
  [[nodiscard]] uint32_t getBytesPerFrame() const {
    return mChannelCount * getBytesPerSample();
  }

  /**
   * Get the number of bytes per sample. This is calculated using the sample
   * format. For example, a stream using 16-bit integer samples will have 2
   * bytes per sample.
   *
   * @return the number of bytes per sample.
   */
  [[nodiscard]] uint32_t getBytesPerSample() const;

  /**
   * The number of audio frames read from the stream.
   * This monotonic counter will never get reset.
   *
   * @return the number of frames read so far
   */
  virtual int64_t getFramesRead();

  /**
   * The number of audio frames written into the stream.
   * This monotonic counter will never get reset.
   *
   * @return the number of frames written so far
   */
  virtual int64_t getFramesWritten();

  /**
   * Returns the number of frames of data currently in the buffer
   */
  OpResult<int32_t> getAvailableFrames();

  /**
   * Wait until the stream has a minimum amount of data available in its buffer.
   * This can be used with an EXCLUSIVE MMAP input stream to avoid reading data
   * too close to the DSP write position, which may cause glitches.
   *
   * The numFrames will be clipped internally against the BufferCapacity minus
   * BurstSize. This is to prevent trying to wait for more frames than could
   * possibly be available. In this case, the return value may be less than
   * numFrames. Note that there may still be glitching if numFrames is too high.
   *
   * @param numFrames requested minimum frames available
   * @param timeoutNanoseconds
   * @return number of frames available, kErrorTimeout
   */
  OpResult<int32_t> waitForAvailableFrames(uint32_t numFrames, int64_t timeoutNanoseconds);

  /**
   * Wait until the stream's current state no longer matches the input state.
   * The input state is passed to avoid race conditions caused by the state
   * changing between calls.
   *
   * Note that generally applications do not need to call this. It is considered
   * an advanced technique and is mostly used for testing.
   *
   * ```
   * int64_t timeoutNanos = 500 * kNanosPerMillisecond; // arbitrary 1/2 second
   * State currentState = stream->getState();
   * State nextState = State::kUnknown;
   * while (result == Result::kOK && currentState != State::kPaused) {
   *     result = stream->waitForStateChange(currentState, &nextState, timeoutNanos);
   *     currentState = nextState;
   * }
   * ```
   *
   * If the state does not change within the timeout period then it will
   * return ErrorTimeout. This is true even if timeoutNanoseconds is zero.
   *
   * @param inputState The state we want to change away from.
   * @param nextState Pointer to a variable that will be set to the new state.
   * @param timeoutNanoseconds The maximum time to wait in nanoseconds.
   * @return Result::kOK or a Result::kError.
   */
  virtual Result waitForStateChange(StreamState inputState, StreamState *nextState, int64_t timeoutNanoseconds) = 0;

  /**
   * The clock mode
   */
  enum class ClockMode : uint8_t {
    Monotonic,
    Realtime,
  };

  /**
   * Get the estimated time that the frame at `framePosition` entered or left
   * the audio processing pipeline.
   *
   * This can be used to coordinate events and interactions with the external
   * environment, and to estimate the latency of an audio stream.
   *
   * The time is based on the implementation's best effort, using whatever
   * knowledge is available to the system, but cannot account for any delay
   * unknown to the implementation.
   *
   * Note that due to issues in Android before R, we recommend NOT calling
   * this method from a data callback.
   *
   * @param mode the type of clock to use e.g. ClockMode::kMonotonic
   * @param framePosition the frame number to query
   * @param timeNanoseconds an output parameter which will contain the presentation timestamp
   */
  virtual Result getTimestamp(const ClockMode mode, int64_t *framePosition, int64_t *timeNanoseconds) {
    return Result::ErrorUnimplemented;
  }

  Result getTimestamp(const ClockMode mode = ClockMode::Monotonic) {
    return getTimestamp(mode, nullptr, nullptr);
  }

  /**
   * Calculate the latency of a stream based on getTimestamp().
   *
   * Output latency is the time it takes for a given frame to travel from the
   * app to some type of digital-to-analog converter. If the DAC is external,
   * for example in a USB interface or a TV connected by HDMI, then there may be
   * additional latency that the Android device is unaware of.
   *
   * Input latency is the time it takes to a given frame to travel from an
   * analog-to-digital converter (ADC) to the app.
   *
   * Note that the latency of an OUTPUT stream will increase abruptly when you
   * write data to it and then decrease slowly over time as the data is
   * consumed.
   *
   * The latency of an INPUT stream will decrease abruptly when you read data
   * from it and then increase slowly over time as more data arrives.
   *
   * The latency of an OUTPUT stream is generally higher than the INPUT latency
   * because an app generally tries to keep the OUTPUT buffer full and the INPUT
   * buffer empty.
   *
   * Note that due to issues in Android before R, we recommend NOT calling
   * this method from a data callback.
   *
   * @return a OpResult which has a result of Result::kOK and a value containing the latency
   * in milliseconds, or a result of Result::kError*.
   */
  virtual OpResult<double> calculateLatencyMillis() {
    return Result::ErrorUnimplemented;
  }

  /**
   * Returns last result passed from an error callback
   */
  virtual Result getLastErrorCallbackResult() const {
    return mErrorCallbackResult;
  }

  /**
   * Returns the error callback object for this stream, if set.
   * For internal use only.
   */
  [[nodiscard]] const std::shared_ptr<ErrorCallback> &getErrorCallback() const {
    return mErrorCallback;
  }

  /**
   * Set a weak_ptr to this stream from the shared_ptr so that we can
   * later use a shared_ptr in the error callback.
   */
  void setWeakThis(const std::shared_ptr<AudioStream> &sharedStream) {
    mWeakThis = sharedStream;
  }

  /**
   * Only for debugging. Do not use in production.
   * If you need to call this method something is wrong.
   * If you think you need it for production then please let us know
   * so we can modify audio so that you don't need this.
   *
   * @return nullptr or a pointer to a stream from the system API
   */
  virtual void *getUnderlyingStream() const {
    return nullptr;
  }

protected:
  /**
   * Update mFramesWritten.
   * For internal use only.
   */
  virtual void updateFramesWritten() = 0;

  /**
   * Update mFramesRead.
   * For internal use only.
   */
  virtual void updateFramesRead() = 0;

  /**
   * This is used to detect more than one error callback from a stream.
   * These were bugs in some versions of Android that caused multiple error callbacks.
   *
   * Calling this sets an atomic<bool> true and returns the previous value.
   *
   * @return false on first call, true on subsequent calls
   */
  bool wasErrorCallbackCalled() {
    return mErrorCallbackCalled.exchange(true);
  }

  /**
   * Wait for a transition from one state to another.
   * @return OK if the endingState was observed, or ErrorUnexpectedState
   *   if any state that was not the startingState or endingState was observed
   *   or ErrorTimeout.
   */
  virtual Result
  waitForStateTransition(StreamState startingState, StreamState endingState, int64_t timeoutNanoseconds);

  /**
   * Override this to provide a default for when the application did not specify a callback.
   */
  virtual DataCallbackResult onDefaultCallback(void * /* audioData  */, uint32_t /* numFrames */) {
    return DataCallbackResult::Stop;
  }

  /**
   * Override this to provide your own behaviour for the audio callback
   *
   * @param audioData container array which audio frames will be written into or read from
   * @param numFrames number of frames which were read/written
   * @return the result of the callback: stop or continue
   */
  virtual DataCallbackResult fireDataCallback(void *audioData, uint32_t numFrames);

  /**
   * Returns true if callbacks may be called
   */
  [[nodiscard]] bool isDataCallbackEnabled() const {
    return mDataCallbackEnabled;
  }

  /**
   * This can be set false internally to prevent callbacks
   * after DataCallbackResult::Stop has been returned.
   */
  void setDataCallbackEnabled(const bool enabled) {
    mDataCallbackEnabled = enabled;
  }

  /**
   * This should only be called as a stream is being opened.
   * Otherwise, we might override setDelayBeforeCloseMillis().
   */
  void calculateDefaultDelayBeforeCloseMillis();

  /**
   * Try to avoid a race condition when closing.
   */
  void sleepBeforeClose() const;

  /**
   * Time to sleep in order to prevent a race condition with a callback after a close().
   * Two milliseconds may be enough but 10 msec is even safer.
   */
  static constexpr uint32_t kMinDelayBeforeCloseMillis = 10;
  static constexpr uint32_t kMaxDelayBeforeCloseMillis = 100;

  /*
   * Make a shared_ptr that will prevent this stream from being deleted.
   */
  std::shared_ptr<AudioStream> lockWeakThis() const {
    return mWeakThis.lock();
  }

  std::mutex mLock; // for synchronizing start/stop/close

  //!< weak pointer to this object
  std::weak_ptr<AudioStream> mWeakThis;

  //!< Number of frames which will be copied to/from the audio device in a single read/write operation
  AudioFrameCount mFramesPerBurst = kUnspecified;

  uint32_t mDelayBeforeCloseMillis = kMinDelayBeforeCloseMillis;

  //!< Number of frames which have been read from the stream.
  std::atomic<int64_t> mFramesRead{};
  //!< Number of frames which have been written into the stream
  std::atomic<int64_t> mFramesWritten{};

  //!< Stream content type. For Android, only active on API 28+
  ContentType mContentType = ContentType::Music;

  Result mErrorCallbackResult = Result::OK;

private:
  std::atomic<bool> mDataCallbackEnabled{false};
  std::atomic<bool> mErrorCallbackCalled{false};
};

} // namespace coral

#endif //CORAL_PRIVATE_AUDIO_STREAM_H
