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

#include "AudioPlayerImpl.h"

#include "private/audio/AudioStreamBuilder.h"

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/Resample.h>
#include <core/time/SystemClock.h>

#include <core/event/EventLooper.h>

#include <media/PacketBuffer.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <chrono>
#include <utility>

namespace coral {

namespace {

/**
 * Quits an event thread and disposes of it.
 *
 * A thread that quits from its own looper thread can only be detached, and the detached thread goes
 * on touching its `EventThread` object while it unwinds, so destroying that object here would be a
 * use after free. The case arises when the delegate of the player destroys it from the callback
 * that reports the end of the playback, which runs on the message thread.
 */
void disposeEventThread(std::unique_ptr<EventThread>& thread) {
  if (thread == nullptr) {
    return;
  }
  const auto& looper = thread->getLooper();
  const bool self = looper != nullptr && looper->isCurrentThread();
  thread->quit();
  if (self) {
    (void) thread.release();
  } else {
    thread.reset();
  }
}

} // namespace anonymous

std::shared_ptr<AudioPlayerImpl> AudioPlayerImpl::Make(const std::string& url) {
  return std::make_shared<AudioPlayerImpl>(url);
}

AudioPlayerImpl::AudioPlayerImpl(std::string url) noexcept
  : AudioPlayer(std::move(url)),
    mNumberOfReading(0),
    mSeekTimeUs(Definitions::kTimeUnset),
    mStartTimeUs(0),
    mReadingEof(false),
    mSonic(nullptr) {
  mError = std::make_unique<Error>();
  startMessageThread();
}

AudioPlayerImpl::~AudioPlayerImpl() {
  // The message thread is stopped before the resources it reads are released.
  release();
}

// ==================================================================================================
// The message thread
// ==================================================================================================

void AudioPlayerImpl::startMessageThread() {
  if (mThread != nullptr && mThread->isAlive()) {
    return;
  }

  mThread = std::make_unique<EventThread>(getName() + ".MessageThread");
  mThread->start();
  if (mThread->getLooper() == nullptr) {
    MINILOG_ERROR("Failed to start the player thread");
    mThread.reset();
    return;
  }
  // A handler holds its callback weakly, so the reference has to be kept alive here. The deleter does
  // nothing, because the player is owned by its caller and not by the reference.
  mCallbackRef = std::shared_ptr<EventHandler::Callback>(this, [](EventHandler::Callback*) {
  });
  const std::lock_guard<std::mutex> guard(mHandlerLock);
  mHandler = EventHandler::create(mThread->getLooper(), mCallbackRef);
}

void AudioPlayerImpl::stopMessageThread() {
  mReadScheduled.store(false, std::memory_order_release);
  mCompletionScheduled.store(false, std::memory_order_release);

  std::shared_ptr<EventHandler> handler;
  {
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    handler = std::move(mHandler);
    mHandler = nullptr;
  }
  if (handler != nullptr) {
    // The events that were not handled belong to a player that is going away.
    handler->removeAllCallbacksAndMessages();
  }
  disposeEventThread(mThread);
  mCallbackRef.reset();
}

bool AudioPlayerImpl::sendEvent(const int what) {
  std::shared_ptr<EventHandler> handler;
  {
    // The message thread may be replaced while it posts, so the handler is only ever reached through
    // a copy taken under the lock.
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    handler = mHandler;
  }
  return handler != nullptr && handler->sendEmptyMessage(what);
}

bool AudioPlayerImpl::sendEventDelayed(const int what, const TimeDelta delay) {
  if (delay <= TimeDelta::zero()) {
    return sendEvent(what);
  }
  std::shared_ptr<EventHandler> handler;
  {
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    handler = mHandler;
  }
  return handler != nullptr && handler->sendEmptyMessageDelayed(what, delay);
}

bool AudioPlayerImpl::handleMessage(Message* msg) {
  switch (msg->what) {
    case MSG_START_PLAYBACK: {
      startInternal();
      break;
    }
    case MSG_RESUME_PLAYBACK: {
      resumeInternal();
      break;
    }
    case MSG_PAUSE_PLAYBACK: {
      pauseInternal();
      break;
    }
    case MSG_STOP_PLAYBACK: {
      stopInternal();
      break;
    }
    case MSG_SEEK: {
      seekInternal();
      break;
    }
    case MSG_UPDATE_RATE_AND_VOLUME: {
      maybeUpdateRateAndVolume();
      break;
    }
    case MSG_READ_AUDIO: {
      onReadAudioEvent();
      break;
    }
    case MSG_PLAYBACK_COMPLETED: {
      onPlaybackCompletedEvent();
      break;
    }
    default: {
      break;
    }
  }
  return true;
}

void AudioPlayerImpl::requestReadEvent() {
  if (mReadScheduled.exchange(true, std::memory_order_acq_rel)) {
    // A read event is already queued and it reads everything the ring buffer can take, so it covers
    // this block as well.
    return;
  }
  if (!sendEvent(MSG_READ_AUDIO)) {
    mReadScheduled.store(false, std::memory_order_release);
  }
}

void AudioPlayerImpl::requestCompletionEvent() {
  if (mCompletionScheduled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (!sendEvent(MSG_PLAYBACK_COMPLETED)) {
    mCompletionScheduled.store(false, std::memory_order_release);
  }
}

void AudioPlayerImpl::cancelPendingEvents() {
  std::shared_ptr<EventHandler> handler;
  {
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    handler = mHandler;
  }
  if (handler != nullptr) {
    // A read and a completion that were queued for the playback that is being stopped are dropped.
    handler->removeMessages(MSG_READ_AUDIO);
    handler->removeMessages(MSG_PLAYBACK_COMPLETED);
  }
  mReadScheduled.store(false, std::memory_order_release);
  mCompletionScheduled.store(false, std::memory_order_release);
}

void AudioPlayerImpl::onReadAudioEvent() {
  // The event is cleared before the reading, so that the render side can ask for another one while
  // this one refills the ring buffer.
  mReadScheduled.store(false, std::memory_order_release);
  if (!isPlaying()) {
    // The event was queued by a playback that was stopped in the meantime.
    return;
  }
  readAudioFromFile();
}

void AudioPlayerImpl::onPlaybackCompletedEvent() {
  mCompletionScheduled.store(false, std::memory_order_release);
  // checks if we should start the next playback loop.
  if (shouldNextLoop()) {
    startNextLooping();
  } else {
    notifyPlaybackFinished(true);
  }
}

void AudioPlayerImpl::release() {
  stopMessageThread();
  resetInternal();
}

bool AudioPlayerImpl::onPrepare() {
  // prepares audio file and output stream
  const auto error = getOutError();
  if (!initAudioFile(error)) {
    notifyPlaybackError(error);
    return false;
  }

  if (!initAudioStream(error)) {
    notifyPlaybackError(error);
    return false;
  }

  auto format = mAudioFile->getProcessingFormat();
  mPcmBuffer = AudioPCMBuffer::InitWithPCMFormat(format.value(), kDefaultNumberFrames);
  mStatus = Status::kPrepared;

  return true;
}

bool AudioPlayerImpl::onPlayDelay(const int64_t delayMs) {
  // when the player is playing, return immediately
  if (isPlaying()) {
    return true;
  }

  // if the status is completed or failed, the player is needs to reset before the start
  if (mStatus == Status::kCompleted || mStatus == Status::kFailed) {
    resetInternal();
  }

  // if the status is stopped, the player only needs to restart the playback task
  if (mStatus == Status::kStopped) {
    mStatus = Status::kPrepared;
  }

  // if the status is unknown, we should prepare the player first
  if (mStatus == Status::kIdle) {
    if (!prepareToPlay()) {
      return false;
    }
    mStatus = Status::kPrepared;
  }

  // when the player is prepared, starts playback task.
  if (mStatus == Status::kPrepared) {
    startPlayback(delayMs);
    return true;
  }

  // if the player is paused, just notify to playing
  if (mStatus == Status::kPaused) {
    mStatus = Status::kPlaying;
    sendEvent(MSG_RESUME_PLAYBACK);
    return true;
  }

  // unknown status
  return false;
}

void AudioPlayerImpl::onPause() {
  sendEvent(MSG_PAUSE_PLAYBACK);
}

void AudioPlayerImpl::onStop() {
  // The read and completion events that are pending belong to the playback that is being stopped.
  cancelPendingEvents();
  sendEvent(MSG_STOP_PLAYBACK);
}

void AudioPlayerImpl::onPlaybackRateUpdate() {
  if (mStatus == Status::kPlaying) {
    sendEvent(MSG_UPDATE_RATE_AND_VOLUME);
  }
}

void AudioPlayerImpl::onSeekUpdate(const int64_t timeUs) {
  mSeekTimeUs = timeUs;
  if (mStatus == Status::kIdle
      || mStatus == Status::kCompleted
      || mStatus == Status::kFailed) {
    resetInternal();
  } else {
    sendEvent(MSG_SEEK);
  }
}

void AudioPlayerImpl::onVolumeUpdate() {
  if (mStatus == Status::kPlaying) {
    sendEvent(MSG_UPDATE_RATE_AND_VOLUME);
  }
}

void AudioPlayerImpl::resetInternal() {
  mAudioFile.reset();
  mAudioStream.reset();
  mPcmBuffer.reset();
  mRingBuffer.reset();
  if (mSonic != nullptr) {
    sonicDestroyStream(mSonic);
    mSonic = nullptr;
  }
  mCurrentTimeUs = getStartPositionUs();
  mStatus = Status::kIdle;
  mReadingEof = false;
}

void AudioPlayerImpl::startPlayback(const int64_t delayMs) {
  sendEventDelayed(MSG_START_PLAYBACK, std::chrono::milliseconds(delayMs));
}

void AudioPlayerImpl::startInternal() {
  mStatus = Status::kPlaying;

  // first reading loop number
  if (mNumberOfLoops > 0) {
    mNumberOfReading = 1;
  }

  // notify the player is started.
  if (mDelegate != nullptr) {
    mDelegate->onStartPlaying(this, getDurationUs());
  }

  resumeInternal();
}

void AudioPlayerImpl::resumeInternal() {
  if (mSeekTimeUs != Definitions::kTimeUnset) {
    seekInternal();
  }
  // starts the AudioStream before reading from the AudioFile.
  mAudioStream->start();
  maybeUpdateRateAndVolume();
  readAudioFromFile();
}

void AudioPlayerImpl::pauseInternal() {
  if (mAudioStream != nullptr) {
    mAudioStream->pause();
  }
}

void AudioPlayerImpl::stopInternal() {
  if (mAudioStream != nullptr) {
    mAudioStream->stop();
  }
  mRingBuffer.reset();
}

void AudioPlayerImpl::seekInternal() {
  if (mSeekTimeUs == Definitions::kTimeUnset ||
      mAudioFile == nullptr ||
      mAudioFile->getProcessingFormat() == std::nullopt) {
    resetSeekTime();
    return;
  }

  // clear cache audio data
  mRingBuffer.reset();

  auto format = mAudioFile->getProcessingFormat();
  auto seekPosition = std::div(mSeekTimeUs * format->getSampleRate(), Definitions::kMicrosPerSecond);
  mAudioFile->setFramePosition(seekPosition.quot);
  mCurrentTimeUs = getSeekPositionUs();
  updateStartReadPosition();
  resetSeekTime();
}

void AudioPlayerImpl::resetSeekTime() {
  mSeekTimeUs = Definitions::kTimeUnset;
}

bool AudioPlayerImpl::canReadAudioFile() const {
  if (!mAudioStream || !mAudioFile || mReadingEof) {
    return false;
  }
  // The file is read until the cache holds half of what it can hold. The queued audio is what
  // covers a reading that is late, and the device buffer is not a measure of it: on an exclusive
  // stream the device buffer is one endpoint period, and reading no further than that would leave
  // the render thread with a few milliseconds to play as soon as the file is read a few
  // milliseconds late.
  return mRingBuffer.available() < mRingBuffer.capacity() / 2;
}

bool AudioPlayerImpl::isPlaybackFinished() const {
  return mReadingEof && mRingBuffer.available() == 0;
}

bool AudioPlayerImpl::shouldNextLoop() const {
  return (mNumberOfLoops < 0) || (mNumberOfLoops > 0 && mNumberOfReading < mNumberOfLoops);
}

void AudioPlayerImpl::startNextLooping() {
  auto error = getOutError();
  if (!initAudioFile(error)) {
    notifyPlaybackError(error);
    return;
  }
  if (mNumberOfLoops > 0) {
    mNumberOfReading = mNumberOfReading + 1;
  }
  resumeInternal();
}

bool AudioPlayerImpl::initAudioFile(Error* error) {
  if (!mAudioFile) {
    mAudioFile = AudioFile::InitForReading(mUrl, AudioSampleFormat::kDefault, true, &error);
    if (!mAudioFile) {
      return false;
    }
  }

  // process start position
  updateStartPosition();

  // calculate the duration of the AudioFile if needed
  const auto processFormat = mAudioFile->getProcessingFormat();
  if (mDurationUs == Definitions::kInvalidValue &&
      mAudioFile->getLength() > 0 &&
      processFormat != std::nullopt) {
    auto [durationUs, rem] =
      std::div(mAudioFile->getLength() * Definitions::kMicrosPerSecond, processFormat->getSampleRate());
    mDurationUs = durationUs;
    mNumberOfChannels = processFormat->getChannelCount();
  }

  return true;
}

bool AudioPlayerImpl::initAudioStream(Error* error) {
  const auto format = mAudioFile->getProcessingFormat();
  auto builder = AudioStreamBuilder();
  const auto result =
    builder.setDirection(Direction::Render)
           .setChannelCount(format->getChannelCount())
           .setSampleRate(static_cast<uint64_t>(format->getSampleRate()))
           .setFormat(AudioSampleFormat::kInt16)
           .setFramesPerDataCallback(kDefaultNumberFrames)
           .setBufferCapacityInFrames(kDefaultNumberFrames * kDefaultBurstSize)
           .setDataCallback(shared_from_this())
           .openStream(mAudioStream);
  if (result != Result::OK) {
    mAudioStream.reset();
    if (error != nullptr) {
      error->code = static_cast<ErrCode>(result);
      error->failureReason = "Failed to create AudioStream";
    }
    MINILOG_ERROR("Failed to create AudioStream: {}", convertToText(result));
    return false;
  }

  uint32_t maxCachedFrames = kDefaultCacheDuration * mAudioStream->getSampleRate();
  maxCachedFrames = std::max(maxCachedFrames, mAudioStream->getBufferCapacityInFrames());
  const auto frames = std::max(mAudioStream->getFramesPerDataCallback(), kDefaultNumberFrames);
  // The scratch buffer is resized rather than reserved: the render callback reads the frames of a
  // callback through its data().
  mAppBuffer.resize(static_cast<size_t>(frames) * format->getChannelCount());
  mRingBuffer.init(maxCachedFrames, format->getChannelCount());
  mSonic = sonicCreateStream(mAudioStream->getSampleRate(), mAudioStream->getChannelCount());
  if (isEnableRate()) {
    sonicSetRate(mSonic, mPlaybackRate);
  } else {
    sonicSetRate(mSonic, 1.0f);
  }
  sonicFlushStream(mSonic);

  return true;
}

void AudioPlayerImpl::readAudioFromFile() {
  // The playback triggers the reading by asking for a read event, and that event reads until the
  // ring buffer cannot take another block: the player therefore stays ahead of the audio it renders
  // without the callback ever waiting for the file.
  while (canReadAudioFile()) {
    if (!readNextAudioBlock()) {
      break;
    }
  }
}

bool AudioPlayerImpl::readNextAudioBlock() {
  // a.read audio pcm data from the AudioFile.
  auto error = getOutError();
  auto result = mAudioFile->readIntoBuffer(mPcmBuffer, &error);
  if (!result) {
    // checks if the AudioFile is end of stream
    if (mAudioFile->getFramePosition() >= mAudioFile->getLength()) {
      mReadingEof = true;
    } else {
      notifyPlaybackError(error);
      return false;
    }
  }
  // b.update reading position
  if (!mReadingEof) {
    mReadPosition += mPcmBuffer->getFrameLength();
  } else {
    mReadPosition = mAudioFile->getFramePosition();
  }
  // c.data conversion and process the speed and volume.
  if (!writeAudioBuffer(mPcmBuffer)) {
    // The ring buffer is full, which the next read event resumes from.
    MINILOG_DEBUG("Failed to store processed audio data");
    return false;
  }
  maybeStartAudioStream();
  return true;
}

void AudioPlayerImpl::maybeStartAudioStream() {
  if (!mAudioStream) {
    return;
  }
  const auto state = mAudioStream->getState();
  if (state != StreamState::Starting && state != StreamState::Started) {
    mAudioStream->requestStart();
  }
}

void AudioPlayerImpl::maybeUpdateRateAndVolume() {
  // checks if we should change playback rate and volume
  if (!mSonic || !isEnableRate()) {
    return;
  }
  if (isEnableRate() && mPlaybackRate != sonicGetRate(mSonic)) {
    sonicSetRate(mSonic, mPlaybackRate);
  }
  if (getVolume() != sonicGetVolume(mSonic)) {
    sonicSetVolume(mSonic, getVolume());
  }
}

bool AudioPlayerImpl::writeAudioBuffer(const std::unique_ptr<AudioPCMBuffer>& pcmBuffer) {
  const auto format = pcmBuffer->getFormat();
  const auto numFrames = pcmBuffer->getFrameLength();
  switch (format->getSampleFormat()) {
    case AudioSampleFormat::kFloat: {
      return mRingBuffer.write(static_cast<const float*>(pcmBuffer->rawData()), numFrames);
    }
    case AudioSampleFormat::kInt16: {
      return mRingBuffer.writeInt16(static_cast<const int16_t*>(pcmBuffer->rawData()), numFrames);
    }
    case AudioSampleFormat::kInt24: {
      return mRingBuffer.writeInt24(static_cast<const uint8_t*>(pcmBuffer->rawData()), numFrames);
    }
    case AudioSampleFormat::kInt32: {
      return mRingBuffer.writeInt32(static_cast<const int32_t*>(pcmBuffer->rawData()), numFrames);
    }
    default: {
      return false;
    }
  }
}

DataCallbackResult
AudioPlayerImpl::onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) {
  // Read stored data into the buffer provided.
  const auto shortData = static_cast<short*>(audioData);
  while (sonicSamplesAvailable(mSonic) < numFrames) {
    if (const auto result = mRingBuffer.readInt16(mAppBuffer.data(), numFrames); result > 0) {
      sonicWriteShortToStream(mSonic, mAppBuffer.data(), result);
    } else {
      break;
    }
  }
  const auto framesRead = sonicReadShortFromStream(mSonic, shortData, numFrames);
  updatePlaybackTimeUs();
  if (framesRead <= 0 && isPlaybackFinished()) {
    // The buffer is drained and the file was read to its end: the playback is completed.
    requestCompletionEvent();
  } else {
    // The frames that were rendered are gone from the ring buffer, so the file is asked for enough
    // audio to refill it.
    requestReadEvent();
  }
  if (framesRead > 0 || (isPlaying() && !isPlaybackFinished())) {
    return DataCallbackResult::Continue;
  }
  return DataCallbackResult::Stop;
}

void AudioPlayerImpl::updateStartPosition() {
  if (mStartTimeUs != Definitions::kUnspecified) {
    auto format = mAudioFile->getProcessingFormat();
    auto startPosition = std::div(mStartTimeUs * format->getSampleRate(),
      Definitions::kMicrosPerSecond);
    mAudioFile->setFramePosition(startPosition.quot);
  } else {
    mAudioFile->setFramePosition(0);
  }
  mCurrentTimeUs = getStartPositionUs();
  updateStartReadPosition();
}

void AudioPlayerImpl::updateStartReadPosition() {
  mReadingEof = false;
  if (mCurrentTimeUs != Definitions::kUnspecified) {
    auto currentPosition = std::div(
      mCurrentTimeUs * mAudioFile->getProcessingFormat()->getSampleRate(),
      Definitions::kMicrosPerSecond);
    mReadPosition = currentPosition.quot;
  } else {
    mReadPosition = 0;
  }
}

void AudioPlayerImpl::updatePlaybackTimeUs() {
  if (!mAudioFile) {
    return;
  }
  // If we are on seeking operation, the current time us is seek positionUs.
  if (mSeekTimeUs != Definitions::kTimeUnset) {
    mCurrentTimeUs = getSeekPositionUs();
  } else {
    const auto position = mReadPosition - static_cast<int64_t>(mRingBuffer.available());
    auto currentTime =
      std::div(position * Definitions::kMicrosPerSecond,
        mAudioFile->getProcessingFormat()->getSampleRate());
    mCurrentTimeUs = currentTime.quot;
  }
}

void AudioPlayerImpl::notifyPlaybackFinished(const bool successfully) {
  updatePlaybackTimeUs();
  mStatus = successfully ? Status::kCompleted : Status::kFailed;
  if (mDelegate != nullptr) {
    mDelegate->onFinishPlaying(this, successfully);
  }
  mCurrentTimeUs = getStartPositionUs();
  mReadPosition = 0;
  mReadingEof = false;
  mAudioFile.reset();
  if (mAudioStream != nullptr) {
    mAudioStream->stop();
  }
}

void AudioPlayerImpl::notifyPlaybackError(const Error* error) {
  updatePlaybackTimeUs();
  mStatus = Status::kFailed;
  if (mDelegate != nullptr) {
    mDelegate->onDecodeErrorOccur(this, error);
  }
  mCurrentTimeUs = getStartPositionUs();
  mAudioFile.reset();
  if (mAudioStream != nullptr) {
    mAudioStream->stop();
  }
}

} // namespace coral
