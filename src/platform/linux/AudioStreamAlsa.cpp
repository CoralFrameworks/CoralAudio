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

#include "AudioStreamAlsa.h"

#include <coral/audio/AudioUtils.h>

#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <time.h>

namespace coral {

namespace {

/** Period used when the application did not ask for a callback size. Roughly 10 ms at 48 kHz. */
constexpr uint32_t kDefaultPeriodFrames = 480;

/** Periods in the device buffer when the application did not ask for a capacity. */
constexpr uint32_t kDefaultBufferPeriods = 4;

/** Sample rate used when the application left it unspecified. */
constexpr uint64_t kDefaultSampleRate = 48000;

/** Channel count used when the application left it unspecified. */
constexpr uint32_t kDefaultChannelCount = 2;

/**
 * How long a pump waits for ALSA before looking at the state machine again, in milliseconds.
 *
 * A request published by `request*()` cannot be applied until the current pump returns, so this
 * also bounds how long `pause()` or `stop()` can take to be observed by the worker.
 */
constexpr int kDeviceWaitSliceMillis = 10;

bool isWouldBlock(const int error) {
  return error == -EAGAIN || error == -EWOULDBLOCK;
}

} // namespace anonymous

// =============================================================================
//  Construction
// =============================================================================

std::shared_ptr<AudioStream> AudioStreamAlsa::Create(const AudioStreamBuilder& builder) {
  return std::make_shared<AudioStreamAlsa>(builder);
}

AudioStreamAlsa::AudioStreamAlsa(const AudioStreamBuilder& builder) noexcept
  : AudioStream(builder),
    mRequestedFramesPerCallback(builder.getFramesPerDataCallback()),
    mRequestedBufferCapacity(builder.getBufferCapacityInFrames()),
    mRequestedSampleRate(builder.getSampleRate()),
    mRequestedChannelCount(builder.getChannelCount()),
    mRequestedFormat(builder.getFormat()) {
}

AudioStreamAlsa::~AudioStreamAlsa() {
  // Dropping a stream without closing it must not leave a thread or a PCM behind.
  close();
}

// =============================================================================
//  Opening
// =============================================================================

Result AudioStreamAlsa::open() {
  std::lock_guard<std::mutex> lifecycle(mLifecycleLock);

  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  if (mState.load() != StreamState::Uninitialized || mPcm != nullptr) {
    return Result::ErrorInvalidState;
  }

  const Result config = isValidConfig();
  if (config != Result::OK) {
    MINILOG_WARN("Invalid audio configuration: {}", convertToText(config));
    return config;
  }

  // The builder may leave any of these unspecified. ALSA needs concrete values, and the stream
  // reports what was actually accepted, so defaults are chosen here rather than guessed later.
  mSampleRate = mRequestedSampleRate != 0 ? mRequestedSampleRate : kDefaultSampleRate;
  mChannelCount = mRequestedChannelCount != 0 ? mRequestedChannelCount : kDefaultChannelCount;
  mFormat = (mRequestedFormat == AudioSampleFormat::kDefault || mRequestedFormat == AudioSampleFormat::kInvalid)
              ? AudioSampleFormat::kFloat
              : mRequestedFormat;
  mFramesPerCallback = mRequestedFramesPerCallback;
  mBufferCapacityInFrames = mRequestedBufferCapacity;

  mFramesRead.store(0);
  mFramesWritten.store(0);
  mXRunCount.store(0);
  mFatalErrorReported.store(false);
  mThreadPriority = AudioThreadPriority::Handle{};
  mExitRequested = false;
  mCommand = Command::None;

  Result result = openDevice();
  if (result != Result::OK) {
    setState(StreamState::Uninitialized);
    return result;
  }

  result = configureDevice();
  if (result != Result::OK) {
    if (mPcm != nullptr) {
      snd_pcm_close(mPcm);
      mPcm = nullptr;
    }
    setState(StreamState::Uninitialized);
    return result;
  }

  const size_t bytesPerFrame = getBytesPerFrame();
  mScratch.assign(static_cast<size_t>(mDevicePeriodFrames) * bytesPerFrame, 0);
  if (!mRingBuffer.init(mDeviceBufferFrames, mChannelCount)) {
    MINILOG_ERROR("The ALSA stream could not allocate a ring buffer of {} frames", mDeviceBufferFrames);
    snd_pcm_close(mPcm);
    mPcm = nullptr;
    setState(StreamState::Uninitialized);
    return Result::ErrorNoMemory;
  }

  // `getTimestamp(ClockMode::Realtime)` reports the same frame position as the monotonic clock, so
  // the offset between the two is captured once rather than queried on every call.
  timespec monotonic{};
  timespec realtime{};
  if (::clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0 && ::clock_gettime(CLOCK_REALTIME, &realtime) == 0) {
    mRealtimeOffsetNanos = static_cast<int64_t>(realtime.tv_sec - monotonic.tv_sec) * 1000000000LL +
                           static_cast<int64_t>(realtime.tv_nsec - monotonic.tv_nsec);
  } else {
    mRealtimeOffsetNanos = 0;
  }

  result = startWorker();
  if (result != Result::OK) {
    snd_pcm_close(mPcm);
    mPcm = nullptr;
    setState(StreamState::Uninitialized);
    return result;
  }

  calculateDefaultDelayBeforeCloseMillis();
  setState(StreamState::Open);

  MINILOG_DEBUG("AudioStreamAlsa opened: direction={}, device={}, sampleRate={}, channels={}, format={}, "
                "periodFrames={}, bufferFrames={}",
    convertToText(getDirection()), deviceName(), static_cast<int>(getSampleRate()),
    static_cast<int>(getChannelCount()), convertToText(getFormat()),
    static_cast<int>(mDevicePeriodFrames), static_cast<int>(mDeviceBufferFrames));
  return Result::OK;
}

Result AudioStreamAlsa::openDevice() {
  const snd_pcm_stream_t stream = isRender() ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
  const int error = snd_pcm_open(&mPcm, deviceName(), stream, SND_PCM_NONBLOCK);
  if (error < 0) {
    mPcm = nullptr;
    MINILOG_ERROR("Could not open the ALSA device '{}': {}", deviceName(), snd_strerror(error));
    return resultFromAlsa(error);
  }
  return Result::OK;
}

Result AudioStreamAlsa::configureDevice() {
  snd_pcm_hw_params_t* hardware = nullptr;
  snd_pcm_hw_params_alloca(&hardware);

  int error = snd_pcm_hw_params_any(mPcm, hardware);
  if (error < 0) {
    MINILOG_ERROR("ALSA could not describe the device parameters: {}", snd_strerror(error));
    return resultFromAlsa(error);
  }

  error = snd_pcm_hw_params_set_access(mPcm, hardware, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (error < 0) {
    MINILOG_ERROR("ALSA does not support interleaved access: {}", snd_strerror(error));
    return resultFromAlsa(error);
  }

  snd_pcm_format_t hardwareFormat = pcmFormatFor(mFormat);
  error = snd_pcm_hw_params_set_format(mPcm, hardware, hardwareFormat);
  if (error < 0 && mFormat == AudioSampleFormat::kFloat) {
    // A device that cannot do float in hardware is common on embedded Linux; the `default` plugin
    // converts, but a direct `hw:` device does not. Falling back to 16-bit keeps the stream usable
    // and is reported through getFormat(), so the caller can see what it got.
    hardwareFormat = SND_PCM_FORMAT_S16;
    error = snd_pcm_hw_params_set_format(mPcm, hardware, hardwareFormat);
    if (error >= 0) {
      mFormat = AudioSampleFormat::kInt16;
      MINILOG_DEBUG("The ALSA device does not accept float samples; using S16 instead");
    }
  }
  if (error < 0) {
    MINILOG_ERROR("ALSA does not accept the requested sample format: {}", snd_strerror(error));
    return Result::ErrorInvalidFormat;
  }

  error = snd_pcm_hw_params_set_channels(mPcm, hardware, mChannelCount);
  if (error < 0) {
    MINILOG_ERROR("ALSA does not accept {} channels: {}", mChannelCount, snd_strerror(error));
    return Result::ErrorInvalidFormat;
  }

  unsigned int sampleRate = static_cast<unsigned int>(mSampleRate);
  int direction = 0;
  error = snd_pcm_hw_params_set_rate_near(mPcm, hardware, &sampleRate, &direction);
  if (error < 0) {
    MINILOG_ERROR("ALSA does not accept a sample rate near {} Hz: {}", mSampleRate, snd_strerror(error));
    return Result::ErrorInvalidRate;
  }
  mSampleRate = sampleRate;

  snd_pcm_uframes_t periodFrames =
    mRequestedFramesPerCallback != 0 ? mRequestedFramesPerCallback : kDefaultPeriodFrames;
  if (periodFrames == 0) {
    periodFrames = kDefaultPeriodFrames;
  }

  snd_pcm_uframes_t bufferFrames =
    mRequestedBufferCapacity != 0 ? mRequestedBufferCapacity : periodFrames * kDefaultBufferPeriods;
  bufferFrames = std::max<snd_pcm_uframes_t>(bufferFrames, periodFrames * 2);

  direction = 0;
  error = snd_pcm_hw_params_set_period_size_near(mPcm, hardware, &periodFrames, &direction);
  if (error < 0) {
    MINILOG_ERROR("ALSA could not set a period size near {} frames: {}", periodFrames, snd_strerror(error));
    return resultFromAlsa(error);
  }

  error = snd_pcm_hw_params_set_buffer_size_near(mPcm, hardware, &bufferFrames);
  if (error < 0) {
    MINILOG_ERROR("ALSA could not set a buffer size near {} frames: {}", bufferFrames, snd_strerror(error));
    return resultFromAlsa(error);
  }

  error = snd_pcm_hw_params(mPcm, hardware);
  if (error < 0) {
    MINILOG_ERROR("ALSA refused the hardware parameters: {}", snd_strerror(error));
    return resultFromAlsa(error);
  }

  snd_pcm_uframes_t actualPeriod = 0;
  snd_pcm_uframes_t actualBuffer = 0;
  snd_pcm_hw_params_get_period_size(hardware, &actualPeriod, nullptr);
  snd_pcm_hw_params_get_buffer_size(hardware, &actualBuffer);
  if (actualPeriod == 0 || actualBuffer == 0 || actualPeriod > actualBuffer) {
    MINILOG_ERROR("ALSA reported an unusable period/buffer size: {} / {}", actualPeriod, actualBuffer);
    return Result::ErrorInternal;
  }

  mDevicePeriodFrames = static_cast<uint32_t>(actualPeriod);
  mDeviceBufferFrames = static_cast<uint32_t>(actualBuffer);

  // The value the application asked for is a bound, not a promise: a device that negotiated a
  // smaller period cannot deliver a larger callback, and the stream publishes what it will do.
  mFramesPerBurst = mDevicePeriodFrames;
  mFramesPerCallback = mRequestedFramesPerCallback != 0
                         ? std::min(mRequestedFramesPerCallback, mDevicePeriodFrames)
                         : mDevicePeriodFrames;
  mBufferCapacityInFrames = mDeviceBufferFrames;
  mBufferSizeInFrames = mDeviceBufferFrames;

  unsigned int actualChannels = 0;
  if (snd_pcm_hw_params_get_channels(hardware, &actualChannels) == 0 && actualChannels > 0) {
    mChannelCount = actualChannels;
  }

  snd_pcm_sw_params_t* software = nullptr;
  snd_pcm_sw_params_alloca(&software);
  error = snd_pcm_sw_params_current(mPcm, software);
  if (error < 0) {
    MINILOG_ERROR("ALSA could not read the software parameters: {}", snd_strerror(error));
    return resultFromAlsa(error);
  }

  // Playback starts once a period is queued, which is the first pump's write. Capture has no data
  // to wait for, so it starts immediately.
  const snd_pcm_uframes_t startThreshold = isRender() ? actualPeriod : 0;
  error = snd_pcm_sw_params_set_start_threshold(mPcm, software, startThreshold);
  if (error >= 0) {
    error = snd_pcm_sw_params_set_avail_min(mPcm, software, actualPeriod);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params_set_stop_threshold(mPcm, software, actualBuffer);
  }
  if (error >= 0) {
    error = snd_pcm_sw_params(mPcm, software);
  }
  if (error < 0) {
    MINILOG_ERROR("ALSA refused the software parameters: {}", snd_strerror(error));
    return resultFromAlsa(error);
  }

  return Result::OK;
}

Result AudioStreamAlsa::startWorker() {
  try {
    mWorker = std::thread(&AudioStreamAlsa::workerLoop, this);
  } catch (const std::system_error& error) {
    MINILOG_ERROR("Could not create the ALSA stream thread: {}", error.what());
    return Result::ErrorNoMemory;
  }
  return Result::OK;
}

void AudioStreamAlsa::stopWorker() {
  if (!mWorker.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mLock);
    mExitRequested = true;
    mCommandAvailable.notify_all();
  }
  if (std::this_thread::get_id() == mWorker.get_id()) {
    // Closing from the callback thread cannot join itself. The worker will observe the exit flag
    // when its current pump returns; the caller is expected not to do this (the same restriction
    // the other platform backends document), so the thread object is left for a later close().
    return;
  }
  mWorker.join();
}

// =============================================================================
//  Teardown
// =============================================================================

Result AudioStreamAlsa::release() {
  std::lock_guard<std::mutex> lifecycle(mLifecycleLock);

  if (mWorker.joinable() && std::this_thread::get_id() == mWorker.get_id()) {
    // The worker cannot join itself, and closing the PCM underneath it would be a use after free.
    // The documented contract is to stop and close from another thread; asking for the stop is the
    // most that can be done safely from here.
    MINILOG_ERROR("AudioStreamAlsa::release() was called from its own audio thread");
    requestStop();
    return Result::ErrorInvalidState;
  }

  if (mState.load() == StreamState::Uninitialized) {
    return Result::OK;
  }
  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }

  {
    std::lock_guard<std::mutex> lock(mLock);
    mState.store(StreamState::Closing);
    mExitRequested = true;
    mCommandAvailable.notify_all();
  }
  mStateChanged.notify_all();
  notifyBufferWaiters();

  stopWorker();

  if (mPcm != nullptr) {
    snd_pcm_drop(mPcm);
    snd_pcm_close(mPcm);
    mPcm = nullptr;
  }

  resetRingBuffer();
  mScratch.clear();
  mThreadPriority = AudioThreadPriority::Handle{};
  mDevicePeriodFrames = 0;
  mDeviceBufferFrames = 0;

  setState(StreamState::Uninitialized);
  return Result::OK;
}

Result AudioStreamAlsa::close() {
  std::lock_guard<std::mutex> lifecycle(mLifecycleLock);

  if (mWorker.joinable() && std::this_thread::get_id() == mWorker.get_id()) {
    // See release(): joining the calling thread is impossible, and the PCM must stay alive.
    MINILOG_ERROR("AudioStreamAlsa::close() was called from its own audio thread");
    requestStop();
    return Result::ErrorInvalidState;
  }

  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }

  {
    std::lock_guard<std::mutex> lock(mLock);
    mState.store(StreamState::Closing);
    mExitRequested = true;
    mCommandAvailable.notify_all();
  }
  mStateChanged.notify_all();
  notifyBufferWaiters();

  stopWorker();

  if (mPcm != nullptr) {
    snd_pcm_drop(mPcm);
    snd_pcm_close(mPcm);
    mPcm = nullptr;
  }

  resetRingBuffer();
  mScratch.clear();
  mThreadPriority = AudioThreadPriority::Handle{};
  mDevicePeriodFrames = 0;
  mDeviceBufferFrames = 0;

  setState(StreamState::Closed);
  return Result::OK;
}

// =============================================================================
//  State machine
// =============================================================================

StreamState AudioStreamAlsa::getState() {
  return mState.load();
}

Result AudioStreamAlsa::waitForStateChange(const StreamState inputState,
  StreamState* nextState,
  const int64_t timeoutNanoseconds) {
  std::unique_lock<std::mutex> lock(mLock);
  StreamState state = mState.load();

  if (state == StreamState::Closed) {
    if (nextState != nullptr) {
      *nextState = state;
    }
    return Result::ErrorClosed;
  }

  if (state == inputState && timeoutNanoseconds > 0) {
    mStateChanged.wait_for(lock, std::chrono::nanoseconds(timeoutNanoseconds),
      [this, inputState] { return mState.load() != inputState; });
    state = mState.load();
  }

  if (nextState != nullptr) {
    *nextState = state;
  }
  if (state == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  return state != inputState ? Result::OK : Result::ErrorTimeout;
}

Result AudioStreamAlsa::requestStart() {
  std::lock_guard<std::mutex> lock(mLock);

  switch (mState.load()) {
    case StreamState::Open:
    case StreamState::Paused:
    case StreamState::Stopped:
    case StreamState::Flushed:
      break;
    case StreamState::Starting:
    case StreamState::Started:
      return Result::OK;
    case StreamState::Closed:
      return Result::ErrorClosed;
    default:
      return Result::ErrorInvalidState;
  }
  if (mPcm == nullptr || !mWorker.joinable()) {
    return Result::ErrorClosed;
  }

  if (isCallbackMode()) {
    setDataCallbackEnabled(true);
  }
  mState.store(StreamState::Starting);
  mCommand = Command::Start;
  mCommandAvailable.notify_one();
  mStateChanged.notify_all();
  notifyBufferWaiters();
  return Result::OK;
}

Result AudioStreamAlsa::requestPause() {
  std::lock_guard<std::mutex> lock(mLock);

  switch (mState.load()) {
    case StreamState::Started:
    case StreamState::Starting:
      break;
    case StreamState::Pausing:
    case StreamState::Paused:
      return Result::OK;
    case StreamState::Closed:
      return Result::ErrorClosed;
    default:
      return Result::ErrorInvalidState;
  }
  if (mPcm == nullptr || !mWorker.joinable()) {
    return Result::ErrorClosed;
  }

  mState.store(StreamState::Pausing);
  mCommand = Command::Pause;
  mCommandAvailable.notify_one();
  mStateChanged.notify_all();
  notifyBufferWaiters();
  return Result::OK;
}

Result AudioStreamAlsa::requestFlush() {
  std::lock_guard<std::mutex> lock(mLock);

  switch (mState.load()) {
    case StreamState::Starting:
    case StreamState::Started:
    case StreamState::Pausing:
    case StreamState::Paused:
      break;
    case StreamState::Flushing:
    case StreamState::Flushed:
      return Result::OK;
    case StreamState::Closed:
      return Result::ErrorClosed;
    default:
      return Result::ErrorInvalidState;
  }
  if (mPcm == nullptr || !mWorker.joinable()) {
    return Result::ErrorClosed;
  }

  mState.store(StreamState::Flushing);
  mCommand = Command::Flush;
  mCommandAvailable.notify_one();
  mStateChanged.notify_all();
  notifyBufferWaiters();
  return Result::OK;
}

Result AudioStreamAlsa::requestStop() {
  std::lock_guard<std::mutex> lock(mLock);

  switch (mState.load()) {
    case StreamState::Open:
    case StreamState::Starting:
    case StreamState::Started:
    case StreamState::Pausing:
    case StreamState::Paused:
    case StreamState::Flushing:
    case StreamState::Flushed:
    case StreamState::Disconnected:
      break;
    case StreamState::Stopping:
    case StreamState::Stopped:
      return Result::OK;
    case StreamState::Closed:
      return Result::ErrorClosed;
    default:
      return Result::ErrorInvalidState;
  }
  if (mPcm == nullptr || !mWorker.joinable()) {
    return Result::ErrorClosed;
  }

  mState.store(StreamState::Stopping);
  mCommand = Command::Stop;
  mCommandAvailable.notify_one();
  mStateChanged.notify_all();
  notifyBufferWaiters();
  return Result::OK;
}

void AudioStreamAlsa::setState(const StreamState state) {
  {
    std::lock_guard<std::mutex> lock(mLock);
    mState.store(state);
  }
  mStateChanged.notify_all();
  notifyBufferWaiters();
}

void AudioStreamAlsa::notifyBufferWaiters() {
  mBufferSpaceAvailable.notify_all();
  mBufferDataAvailable.notify_all();
}

// =============================================================================
//  Worker
// =============================================================================

void AudioStreamAlsa::workerLoop() {
  for (;;) {
    Command command = Command::None;
    {
      std::unique_lock<std::mutex> lock(mLock);
      mCommandAvailable.wait(lock, [this] { return mExitRequested || mCommand != Command::None; });
      if (mExitRequested) {
        break;
      }
      command = mCommand;
      mCommand = Command::None;
    }

    switch (command) {
      case Command::Start:
        applyStart();
        break;
      case Command::Pause:
        applyPause();
        break;
      case Command::Flush:
        applyFlush();
        break;
      case Command::Stop:
        applyStop();
        break;
      case Command::None:
        break;
    }

    while (mState.load() == StreamState::Started) {
      if (isRender()) {
        pumpRender();
      } else {
        pumpCapture();
      }
    }
  }

  // A worker that leaves while promoted would keep the priority for a thread that is about to die;
  // dropping it here is what makes the promotion reversible by construction.
  demoteWorker();
}

void AudioStreamAlsa::applyStart() {
  if (mState.load() != StreamState::Starting) {
    return;
  }

  int error = snd_pcm_prepare(mPcm);
  if (error < 0) {
    reportDeviceError(resultFromAlsa(error), "snd_pcm_prepare");
    return;
  }

  // Playback starts on the first write (the start threshold is one period); capture has nothing to
  // wait for, so it is started explicitly here.
  if (!isRender()) {
    error = snd_pcm_start(mPcm);
    if (error < 0) {
      reportDeviceError(resultFromAlsa(error), "snd_pcm_start");
      return;
    }
  }

  promoteWorker();
  if (isCallbackMode()) {
    setDataCallbackEnabled(true);
  }

  // A close or release can publish `Closing` while the device is being prepared above. That
  // transition must win: entering `Started` afterwards would make the worker pump a device whose
  // owner is waiting to join it.
  {
    std::lock_guard<std::mutex> lock(mLock);
    if (mExitRequested || mState.load() != StreamState::Starting) {
      return;
    }
    mState.store(StreamState::Started);
  }
  mStateChanged.notify_all();
  notifyBufferWaiters();
}

void AudioStreamAlsa::applyPause() {
  if (mState.load() != StreamState::Pausing) {
    return;
  }

  if (mPcm != nullptr) {
    snd_pcm_drop(mPcm);
    snd_pcm_prepare(mPcm);
  }
  demoteWorker();
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  setState(StreamState::Paused);
}

void AudioStreamAlsa::applyFlush() {
  if (mState.load() != StreamState::Flushing) {
    return;
  }

  if (mPcm != nullptr) {
    snd_pcm_drop(mPcm);
    snd_pcm_prepare(mPcm);
  }
  resetRingBuffer();
  demoteWorker();
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  setState(StreamState::Flushed);
}

void AudioStreamAlsa::applyStop() {
  if (mState.load() != StreamState::Stopping) {
    return;
  }

  if (mPcm != nullptr) {
    snd_pcm_drop(mPcm);
    snd_pcm_prepare(mPcm);
  }
  resetRingBuffer();
  demoteWorker();
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  setState(StreamState::Stopped);
}

// =============================================================================
//  Device pump
// =============================================================================

void AudioStreamAlsa::pumpRender() {
  if (mPcm == nullptr) {
    return;
  }

  const snd_pcm_sframes_t available = snd_pcm_avail_update(mPcm);
  if (available < 0) {
    handleAlsaError(static_cast<int>(available), "snd_pcm_avail_update");
    return;
  }
  if (available == 0) {
    waitForDevice(kDeviceWaitSliceMillis);
    return;
  }

  const uint32_t frames = static_cast<uint32_t>(
    std::min<snd_pcm_sframes_t>(available, static_cast<snd_pcm_sframes_t>(mDevicePeriodFrames)));
  if (frames == 0) {
    return;
  }

  const size_t bytesPerFrame = getBytesPerFrame();
  uint8_t* data = mScratch.data();
  bool stopRequested = false;

  if (isCallbackMode()) {
    if (isDataCallbackEnabled()) {
      const uint32_t chunkFrames = callbackChunkFrames(frames);
      for (uint32_t offset = 0; offset < frames && !stopRequested; offset += chunkFrames) {
        const uint32_t chunk = std::min(chunkFrames, frames - offset);
        const DataCallbackResult result =
          fireDataCallback(data + static_cast<size_t>(offset) * bytesPerFrame, chunk);
        if (result == DataCallbackResult::Stop) {
          // The remainder of the device period is silence rather than the previous period's audio.
          std::memset(data + static_cast<size_t>(offset + chunk) * bytesPerFrame, 0,
            static_cast<size_t>(frames - offset - chunk) * bytesPerFrame);
          stopRequested = true;
        }
      }
    } else {
      std::memset(data, 0, static_cast<size_t>(frames) * bytesPerFrame);
    }
  } else {
    size_t filled = 0;
    {
      std::lock_guard<std::mutex> lock(mBufferLock);
      filled = popFromRingBuffer(data, frames);
    }
    if (filled < frames) {
      // Nothing left to play: the tail is silence, and a period the device had to be given without
      // data is an underrun. The count is a diagnostic, not a fatal error.
      std::memset(data + filled * bytesPerFrame, 0, static_cast<size_t>(frames - filled) * bytesPerFrame);
      mXRunCount.fetch_add(1);
    }
    mBufferSpaceAvailable.notify_all();
  }

  uint32_t written = 0;
  while (written < frames) {
    const snd_pcm_sframes_t result = snd_pcm_writei(mPcm,
      data + static_cast<size_t>(written) * bytesPerFrame, frames - written);
    if (isWouldBlock(static_cast<int>(result))) {
      waitForDevice(kDeviceWaitSliceMillis);
      if (mState.load() != StreamState::Started) {
        break;
      }
      continue;
    }
    if (result < 0) {
      if (!handleAlsaError(static_cast<int>(result), "snd_pcm_writei")) {
        break;
      }
      continue;
    }
    written += static_cast<uint32_t>(result);
  }

  if (written > 0) {
    // In callback mode the callback produced the frames; in blocking mode `write()` already counted
    // them as accepted. What the device accepted is what `getFramesRead()` reports either way.
    if (isCallbackMode()) {
      mFramesWritten.fetch_add(written);
    }
    mFramesRead.fetch_add(written);
  }

  if (stopRequested) {
    requestStop();
  }
}

void AudioStreamAlsa::pumpCapture() {
  if (mPcm == nullptr) {
    return;
  }

  const snd_pcm_sframes_t available = snd_pcm_avail_update(mPcm);
  if (available < 0) {
    handleAlsaError(static_cast<int>(available), "snd_pcm_avail_update");
    return;
  }
  if (available == 0) {
    waitForDevice(kDeviceWaitSliceMillis);
    return;
  }

  const uint32_t frames = static_cast<uint32_t>(
    std::min<snd_pcm_sframes_t>(available, static_cast<snd_pcm_sframes_t>(mDevicePeriodFrames)));
  if (frames == 0) {
    return;
  }

  const snd_pcm_sframes_t result = snd_pcm_readi(mPcm, mScratch.data(), frames);
  if (isWouldBlock(static_cast<int>(result))) {
    return;
  }
  if (result < 0) {
    handleAlsaError(static_cast<int>(result), "snd_pcm_readi");
    return;
  }
  if (result == 0) {
    return;
  }

  const uint32_t received = static_cast<uint32_t>(result);
  const size_t bytesPerFrame = getBytesPerFrame();
  uint8_t* data = mScratch.data();
  bool stopRequested = false;

  if (isCallbackMode()) {
    if (isDataCallbackEnabled()) {
      const uint32_t chunkFrames = callbackChunkFrames(received);
      for (uint32_t offset = 0; offset < received && !stopRequested; offset += chunkFrames) {
        const uint32_t chunk = std::min(chunkFrames, received - offset);
        if (fireDataCallback(data + static_cast<size_t>(offset) * bytesPerFrame, chunk) ==
            DataCallbackResult::Stop) {
          stopRequested = true;
        }
      }
      // In this mode the callback consumes the frames as they arrive, so the frames received and
      // the frames consumed are the same, and both counters report them.
      mFramesWritten.fetch_add(received);
      mFramesRead.fetch_add(received);
    }
  } else {
    size_t stored = 0;
    {
      std::lock_guard<std::mutex> lock(mBufferLock);
      stored = pushToRingBuffer(data, received);
    }
    if (stored < received) {
      // The application did not read in time; what does not fit is discarded.
      mXRunCount.fetch_add(1);
    }
    mFramesWritten.fetch_add(received);
    mBufferDataAvailable.notify_all();
  }

  if (stopRequested) {
    requestStop();
  }
}

void AudioStreamAlsa::waitForDevice(const int timeoutMillis) {
  if (mPcm == nullptr) {
    return;
  }
  const int result = snd_pcm_wait(mPcm, timeoutMillis);
  if (result < 0) {
    handleAlsaError(result, "snd_pcm_wait");
  }
}

bool AudioStreamAlsa::handleAlsaError(const int error, const char* operation) {
  if (isWouldBlock(error)) {
    return true;
  }

  if (error == -EPIPE || error == -ESTRPIPE) {
    mXRunCount.fetch_add(1);
    const int recovered = snd_pcm_recover(mPcm, error, 1);
    if (recovered < 0) {
      MINILOG_ERROR("ALSA could not recover from {}: {}", operation, snd_strerror(recovered));
      reportDeviceError(resultFromAlsa(recovered), operation);
      return false;
    }
    MINILOG_DEBUG("ALSA recovered from an xrun/suspend during {}", operation);
    return true;
  }

  MINILOG_ERROR("ALSA {} failed: {}", operation, snd_strerror(error));
  reportDeviceError(resultFromAlsa(error), operation);
  return false;
}

void AudioStreamAlsa::reportDeviceError(const Result error, const char* operation) {
  if (mFatalErrorReported.exchange(true)) {
    return;
  }

  mErrorCallbackResult = error;
  MINILOG_ERROR("ALSA stream failed in {}: {}", operation, convertToText(error));
  setState(StreamState::Disconnected);

  const auto callback = getErrorCallback();
  if (callback == nullptr) {
    MINILOG_DEBUG("No ALSA error callback was set, so the failure was only logged");
    return;
  }

  // The documented reaction to an error is to stop and close the stream, and close() joins the
  // worker. Running that on a detached thread keeps the worker free to return from its pump.
  auto shared = lockWeakThis();
  if (!shared) {
    return;
  }
  try {
    std::thread([shared, error] {
      auto* stream = shared.get();
      const auto errorCallback = stream->getErrorCallback();
      if (errorCallback == nullptr) {
        return;
      }
      if (errorCallback->onError(stream, error)) {
        return;
      }
      errorCallback->onErrorBeforeClose(stream, error);
      stream->close();
      errorCallback->onErrorAfterClose(stream, error);
    }).detach();
  } catch (const std::system_error& exception) {
    MINILOG_ERROR("Could not start the ALSA error thread: {}", exception.what());
  }
}

void AudioStreamAlsa::promoteWorker() {
  if (mThreadPriority.isValid()) {
    return;
  }
  mThreadPriority = AudioThreadPriority::promoteCurrentThread(mFramesPerBurst,
    static_cast<uint32_t>(getSampleRate()));
  if (!mThreadPriority.isValid()) {
    MINILOG_DEBUG("The ALSA worker could not be raised to real-time priority");
  }
}

void AudioStreamAlsa::demoteWorker() {
  if (!mThreadPriority.isValid()) {
    return;
  }
  if (!AudioThreadPriority::demoteCurrentThread(mThreadPriority)) {
    MINILOG_DEBUG("The ALSA worker priority could not be restored");
  }
  mThreadPriority = AudioThreadPriority::Handle{};
}

// =============================================================================
//  Data path
// =============================================================================

uint32_t AudioStreamAlsa::callbackChunkFrames(const uint32_t frames) const {
  const uint32_t chunk = mFramesPerCallback != kUnspecified ? mFramesPerCallback : mDevicePeriodFrames;
  if (chunk == 0 || chunk >= frames) {
    return frames;
  }
  return chunk;
}

size_t AudioStreamAlsa::pushToRingBuffer(const uint8_t* source, const size_t frames) {
  if (source == nullptr || frames == 0) {
    return 0;
  }
  const size_t fit = std::min(frames, mRingBuffer.space());
  if (fit == 0) {
    return 0;
  }

  bool stored = false;
  switch (getFormat()) {
    case AudioSampleFormat::kFloat:
      stored = mRingBuffer.write(reinterpret_cast<const float*>(source), fit);
      break;
    case AudioSampleFormat::kInt16:
      stored = mRingBuffer.writeInt16(reinterpret_cast<const int16_t*>(source), fit);
      break;
    case AudioSampleFormat::kInt24:
      stored = mRingBuffer.writeInt24(source, fit);
      break;
    case AudioSampleFormat::kInt32:
      stored = mRingBuffer.writeInt32(reinterpret_cast<const int32_t*>(source), fit);
      break;
    default:
      break;
  }
  return stored ? fit : 0;
}

size_t AudioStreamAlsa::popFromRingBuffer(uint8_t* destination, const size_t frames) {
  if (destination == nullptr || frames == 0) {
    return 0;
  }
  switch (getFormat()) {
    case AudioSampleFormat::kFloat:
      return mRingBuffer.read(reinterpret_cast<float*>(destination), frames);
    case AudioSampleFormat::kInt16:
      return mRingBuffer.readInt16(reinterpret_cast<int16_t*>(destination), frames);
    case AudioSampleFormat::kInt24:
      return mRingBuffer.readInt24(destination, frames);
    case AudioSampleFormat::kInt32:
      return mRingBuffer.readInt32(reinterpret_cast<int32_t*>(destination), frames);
    default:
      return 0;
  }
}

void AudioStreamAlsa::resetRingBuffer() {
  {
    std::lock_guard<std::mutex> lock(mBufferLock);
    mRingBuffer.reset();
  }
  notifyBufferWaiters();
}

OpResult<uint32_t> AudioStreamAlsa::write(const void* buffer,
  const uint32_t numFrames,
  const int64_t timeoutNanoseconds) {
  if (buffer == nullptr) {
    return Result::ErrorNull;
  }
  if (!isRender() || isCallbackMode()) {
    return Result::ErrorInvalidState;
  }
  if (mState.load() != StreamState::Started) {
    return Result::ErrorInvalidState;
  }
  if (numFrames == 0) {
    return OpResult(0u);
  }

  const size_t bytesPerFrame = getBytesPerFrame();
  const auto* source = static_cast<const uint8_t*>(buffer);
  const int64_t deadline = AudioUtils::getNanoseconds() + std::max<int64_t>(timeoutNanoseconds, 0);

  uint32_t written = 0;
  std::unique_lock<std::mutex> lock(mBufferLock);
  while (written < numFrames) {
    const size_t accepted =
      pushToRingBuffer(source + static_cast<size_t>(written) * bytesPerFrame, numFrames - written);
    if (accepted > 0) {
      written += static_cast<uint32_t>(accepted);
      continue;
    }

    if (timeoutNanoseconds <= 0 || mState.load() != StreamState::Started) {
      break;
    }
    const int64_t remaining = deadline - AudioUtils::getNanoseconds();
    if (remaining <= 0) {
      break;
    }
    mBufferSpaceAvailable.wait_for(lock, std::chrono::nanoseconds(remaining), [this] {
      return mRingBuffer.space() > 0 || mState.load() != StreamState::Started;
    });
  }
  lock.unlock();

  if (written > 0) {
    mFramesWritten.fetch_add(written);
    return OpResult(written);
  }
  return Result::ErrorTimeout;
}

OpResult<uint32_t> AudioStreamAlsa::read(void* buffer,
  const uint32_t numFrames,
  const int64_t timeoutNanoseconds) {
  if (buffer == nullptr) {
    return Result::ErrorNull;
  }
  if (isRender() || isCallbackMode()) {
    return Result::ErrorInvalidState;
  }
  if (mState.load() != StreamState::Started) {
    return Result::ErrorInvalidState;
  }
  if (numFrames == 0) {
    return OpResult(0u);
  }

  const size_t bytesPerFrame = getBytesPerFrame();
  auto* destination = static_cast<uint8_t*>(buffer);
  const int64_t deadline = AudioUtils::getNanoseconds() + std::max<int64_t>(timeoutNanoseconds, 0);

  uint32_t framesRead = 0;
  std::unique_lock<std::mutex> lock(mBufferLock);
  while (framesRead < numFrames) {
    const size_t received =
      popFromRingBuffer(destination + static_cast<size_t>(framesRead) * bytesPerFrame, numFrames - framesRead);
    if (received > 0) {
      framesRead += static_cast<uint32_t>(received);
      continue;
    }

    if (timeoutNanoseconds <= 0 || mState.load() != StreamState::Started) {
      break;
    }
    const int64_t remaining = deadline - AudioUtils::getNanoseconds();
    if (remaining <= 0) {
      break;
    }
    mBufferDataAvailable.wait_for(lock, std::chrono::nanoseconds(remaining), [this] {
      return mRingBuffer.available() > 0 || mState.load() != StreamState::Started;
    });
  }
  lock.unlock();

  if (framesRead > 0) {
    mFramesRead.fetch_add(framesRead);
    return OpResult(framesRead);
  }
  return Result::ErrorTimeout;
}

// =============================================================================
//  Queries
// =============================================================================

OpResult<uint32_t> AudioStreamAlsa::setBufferSizeInFrames(const uint32_t requestedFrames) {
  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  if (requestedFrames > mBufferCapacityInFrames) {
    MINILOG_DEBUG("setBufferSizeInFrames({}) exceeds the capacity of {}", requestedFrames,
      mBufferCapacityInFrames);
  }
  // ALSA fixes the buffer size when the hardware parameters are applied, so the only honest answer
  // is the size in use. It is not a failure: the caller learns the real value either way.
  return OpResult(mBufferSizeInFrames);
}

OpResult<uint32_t> AudioStreamAlsa::getXRunCount() {
  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  return OpResult(mXRunCount.load());
}

bool AudioStreamAlsa::isXRunCountSupported() const {
  return true;
}

Result AudioStreamAlsa::getTimestamp(const ClockMode mode,
  int64_t* framePosition,
  int64_t* timeNanoseconds) {
  if (mPcm == nullptr) {
    return Result::ErrorClosed;
  }

  snd_pcm_uframes_t available = 0;
  snd_htimestamp_t timestamp{};
  const int error = snd_pcm_htimestamp(mPcm, &available, &timestamp);
  if (error < 0) {
    // A plugin without timestamp support (the `null` device, for example) still has a useful frame
    // position; the wall clock is the best time that can be attached to it.
    if (::clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
      return Result::ErrorInternal;
    }
  }

  snd_pcm_sframes_t delay = 0;
  if (snd_pcm_delay(mPcm, &delay) < 0) {
    delay = 0;
  }

  int64_t frame = isRender() ? mFramesRead.load() : mFramesWritten.load();
  if (isRender() && delay > 0) {
    frame -= static_cast<int64_t>(delay);
  }
  frame = std::max<int64_t>(frame, 0);

  if (framePosition != nullptr) {
    *framePosition = frame;
  }
  if (timeNanoseconds != nullptr) {
    int64_t nanos = static_cast<int64_t>(timestamp.tv_sec) * 1000000000LL + timestamp.tv_nsec;
    if (mode == ClockMode::Realtime) {
      nanos += mRealtimeOffsetNanos;
    }
    *timeNanoseconds = nanos;
  }
  return Result::OK;
}

OpResult<double> AudioStreamAlsa::calculateLatencyMillis() {
  if (mPcm == nullptr) {
    return Result::ErrorClosed;
  }

  snd_pcm_sframes_t delay = 0;
  const int error = snd_pcm_delay(mPcm, &delay);
  if (error < 0) {
    return resultFromAlsa(error);
  }

  const auto sampleRate = getSampleRate();
  if (sampleRate == 0) {
    return Result::ErrorInvalidState;
  }
  const int64_t frames = std::max<int64_t>(delay, 0) + static_cast<int64_t>(mFramesPerBurst);
  return OpResult<double>(static_cast<double>(frames) * 1000.0 / static_cast<double>(sampleRate));
}

void AudioStreamAlsa::updateFramesWritten() {
  // Every frame counted here is counted by the thread that moved it, so there is nothing to poll.
}

void AudioStreamAlsa::updateFramesRead() {
  // Every frame counted here is counted by the thread that moved it, so there is nothing to poll.
}

// =============================================================================
//  Helpers
// =============================================================================

snd_pcm_format_t AudioStreamAlsa::pcmFormatFor(const AudioSampleFormat format) {
  switch (format) {
    case AudioSampleFormat::kInt16:
      return SND_PCM_FORMAT_S16;
    case AudioSampleFormat::kInt24:
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
      return SND_PCM_FORMAT_S24_3BE;
#else
      return SND_PCM_FORMAT_S24_3LE;
#endif
    case AudioSampleFormat::kInt32:
      return SND_PCM_FORMAT_S32;
    case AudioSampleFormat::kFloat:
    case AudioSampleFormat::kDefault:
    default:
      return SND_PCM_FORMAT_FLOAT;
  }
}

Result AudioStreamAlsa::resultFromAlsa(const int error) {
  switch (error) {
    case -ENOENT:
    case -ENODEV:
    case -EBUSY:
    case -EACCES:
    case -EPERM:
    case -ECONNREFUSED:
      return Result::ErrorUnavailable;
    case -EINVAL:
      return Result::ErrorIllegalArgument;
    case -ENOMEM:
      return Result::ErrorNoMemory;
    default:
      return Result::ErrorInternal;
  }
}

const char* AudioStreamAlsa::deviceName() {
  const char* const configured = std::getenv("CORAL_ALSA_DEVICE");
  return configured != nullptr && configured[0] != '\0' ? configured : "default";
}

} // namespace coral
