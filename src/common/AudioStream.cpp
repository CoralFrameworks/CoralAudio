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

#include "private/audio/AudioStream.h"
#include "private/audio/AudioStreamBuilder.h"

#include <coral/audio/AudioUtils.h>

#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <thread>

namespace coral {

AudioStream::AudioStream(const AudioStreamBuilder &builder) noexcept
    : AudioStreamBase(builder) {
}

Result AudioStream::close() {
  // Update local counters so they can be read after the close.
  updateFramesWritten();
  updateFramesRead();
  return Result::OK;
}

Result AudioStream::start(int64_t timeoutNanoseconds) {
  Result result = requestStart();
  if (result != Result::OK)
    return result;
  if (timeoutNanoseconds <= 0)
    return result;
  return waitForStateTransition(StreamState::Starting, StreamState::Started, timeoutNanoseconds);
}

Result AudioStream::pause(int64_t timeoutNanoseconds) {
  Result result = requestPause();
  if (result != Result::OK)
    return result;
  if (timeoutNanoseconds <= 0)
    return result;
  return waitForStateTransition(StreamState::Pausing, StreamState::Paused,
      timeoutNanoseconds);
}

Result AudioStream::flush(int64_t timeoutNanoseconds) {
  Result result = requestFlush();
  if (result != Result::OK)
    return result;
  if (timeoutNanoseconds <= 0)
    return result;
  return waitForStateTransition(StreamState::Flushing, StreamState::Flushed,
      timeoutNanoseconds);
}

Result AudioStream::stop(int64_t timeoutNanoseconds) {
  Result result = requestStop();
  if (result != Result::OK)
    return result;
  if (timeoutNanoseconds <= 0)
    return result;
  return waitForStateTransition(StreamState::Stopping, StreamState::Stopped,
      timeoutNanoseconds);
}

uint32_t AudioStream::getBytesPerSample() const {
  return MediaUtils::getBytesPerSample(mFormat);
}

int64_t AudioStream::getFramesRead() {
  updateFramesRead();
  return mFramesRead;
}

int64_t AudioStream::getFramesWritten() {
  updateFramesWritten();
  return mFramesWritten;
}

OpResult<int32_t> AudioStream::getAvailableFrames() {
  const auto readCounter = getFramesRead();
  if (readCounter < 0) {
    return OpResult<int32_t>::Create(readCounter);
  }
  const auto writeCounter = getFramesWritten();
  if (writeCounter < 0) {
    return OpResult<int32_t>::Create(writeCounter);
  }
  const auto framesAvailable = static_cast<int32_t>(writeCounter - readCounter);
  return OpResult(framesAvailable);
}

OpResult<int32_t>
AudioStream::waitForAvailableFrames(uint32_t numFrames, const int64_t timeoutNanoseconds) {
  if (numFrames == 0) {
    return Result::OK;
  }
  // Make sure we don't try to wait for more frames than the buffer can hold.
  // Subtract framesPerBurst because this is often called from a callback
  // and we don't want to be sleeping if the buffer is close to overflowing.
  const uint32_t maxAvailableFrames = getBufferCapacityInFrames() - getFramesPerBurst();
  numFrames = std::min(numFrames, maxAvailableFrames);
  // The capacity should never be less than one burst. But clip to zero just in case.
  numFrames = std::max(static_cast<uint32_t>(0), numFrames);

  int64_t framesAvailable = 0;
  const int64_t burstInNanos = getFramesPerBurst() * Definitions::kNanosPerSecond / getSampleRate();
  bool ready = false;
  const int64_t deadline = AudioUtils::getNanoseconds() + timeoutNanoseconds;
  do {
    OpResult<int32_t> result = getAvailableFrames();
    if (!result) return result;
    framesAvailable = result.value();
    ready = (framesAvailable >= numFrames);
    if (!ready) {
      if (AudioUtils::getNanoseconds() > deadline) {
        break;
      }
      AudioUtils::sleepForNanos(burstInNanos);
    }
  } while (!ready);
  return (!ready)
      ? OpResult<int32_t>(Result::ErrorTimeout)
      : OpResult(static_cast<int32_t>(framesAvailable));
}

Result AudioStream::waitForStateTransition(const StreamState startingState,
    const StreamState endingState,
    const int64_t timeoutNanoseconds) {
  StreamState state;
  {
    std::lock_guard<std::mutex> lock(mLock);
    state = getState();
    if (state == StreamState::Closed) {
      return Result::ErrorClosed;
    } else if (state == StreamState::Disconnected) {
      return Result::ErrorDisconnected;
    }
  }

  auto nextState = state;

  // TODO Should this be a while()?!
  if (state == startingState && state != endingState) {
    if (const auto result = waitForStateChange(state, &nextState, timeoutNanoseconds); result != Result::OK) {
      return result;
    }
  }

  if (nextState != endingState) {
    return Result::ErrorInvalidState;
  }
  return Result::OK;
}

DataCallbackResult
AudioStream::fireDataCallback(void *audioData, const uint32_t numFrames) {
  if (!isDataCallbackEnabled()) {
    MINILOG_ERROR("Called with data callback disabled!");
    return DataCallbackResult::Stop; // Should not be getting called
  }

  // Call the app to do the work.
  DataCallbackResult result;
  if (mDataCallback) {
    result = mDataCallback->onAudioReady(this, audioData, numFrames);
  } else {
    result = onDefaultCallback(audioData, numFrames);
  }
  // We might get called after returning stop. So block that here.
  setDataCallbackEnabled(result == DataCallbackResult::Continue);

  return result;
}

void AudioStream::calculateDefaultDelayBeforeCloseMillis() {
  // Calculate delay time before close based on burst duration.
  // Start with a burst duration then add 1 msec as a safety margin.
  mDelayBeforeCloseMillis = std::clamp(static_cast<uint32_t>(1 + (mFramesPerBurst * 1000) / getSampleRate()),
      kMinDelayBeforeCloseMillis,
      kMaxDelayBeforeCloseMillis);
}

void AudioStream::sleepBeforeClose() const {
  if (mDelayBeforeCloseMillis > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(mDelayBeforeCloseMillis * 1000));
  }
}

} // namespace coral
