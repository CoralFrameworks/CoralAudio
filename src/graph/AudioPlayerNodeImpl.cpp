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

#include "private/graph/AudioPlayerNodeImpl.h"

#include "private/graph/AudioEngineImpl.h"
#include "private/graph/AudioGraphUtils.h"

#include <coral/audio/AudioFile.h>
#include <coral/audio/AudioUtils.h>
#include <core/event/EventLooper.h>

#include <minilog/log.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace coral {

namespace {

/**
 * Quits an event thread and disposes of it.
 *
 * A thread that quits from its own looper thread can only be detached, and the detached
 * thread goes on touching its `EventThread` object while it unwinds, so destroying that
 * object here would be a use after free. The case arises when a completion handler stops
 * or destroys the player from inside the node's own IO thread.
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

AudioPlayerNodeImpl::AudioPlayerNodeImpl() {
  mError = std::make_unique<Error>();
}

AudioPlayerNodeImpl::~AudioPlayerNodeImpl() {
  onReset();
}

void AudioPlayerNodeImpl::onReset() {
  // A node is reset while the engine is stopped, so nothing can be rendering: the node's
  // own IO thread is stopped here before its state is torn down.
  stopIOThread();
  clearSchedule();
  flushRingBuffer();
  mSeekRequest = false;
  mSourceDone = false;
  mPendingSilenceFrames = 0;
  mSourcePosition = 0;
  mCompletionFired = false;
  mPreparedFrameCount = kFrameCountInvalid;
  mPlayedBackDeadline = {};
  mPlaybackStartTime = {};
  mDelayedStart = false;
  mScheduleRenderedBase = 0;
  mScheduleQueuedFrames = 0;
  mResetLastRenderTimePending.store(false, std::memory_order_release);
  mRingBufferReady.store(false, std::memory_order_release);
  mStatus.store(Status::kStatusIdle, std::memory_order_release);
  mError = std::make_unique<Error>();
}

// ---------------------------------------------------------------------------------------
//  Scheduling
// ---------------------------------------------------------------------------------------

void AudioPlayerNodeImpl::scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer,
  const std::optional<AudioTime>& when,
  const BufferOptions options,
  const CompletionCallbackType callbackType,
  CompletionHandler completionHandler) {
  if (buffer == nullptr) {
    MINILOG_ERROR("Failed to schedule the buffer: the buffer is null");
    return;
  }

  {
    const std::lock_guard<std::mutex> guard(mScheduleLock);
    mPendingSchedule = Schedule{};
    mPendingSchedule.buffer = std::move(buffer);
    mPendingSchedule.when = when;
    mPendingSchedule.options = options;
    mPendingSchedule.callbackType = callbackType;
    mPendingSchedule.handler = std::move(completionHandler);
  }

  // The IO thread takes the schedule over on its next tick.
  requestProducerTick(0);
}

void AudioPlayerNodeImpl::scheduleFile(const std::shared_ptr<AudioFile>& file,
  const std::optional<AudioTime>& when,
  const CompletionCallbackType callbackType,
  CompletionHandler completionHandler) {
  if (file == nullptr) {
    MINILOG_ERROR("Failed to schedule the file: the file is null");
    return;
  }
  scheduleSegment(file, 0, kFrameCountInvalid, when, callbackType, std::move(completionHandler));
}

void AudioPlayerNodeImpl::scheduleSegment(const std::shared_ptr<AudioFile>& file,
  const AudioFramePosition startFrame,
  const AudioFrameCount numberFrames,
  const std::optional<AudioTime>& when,
  const CompletionCallbackType callbackType,
  CompletionHandler completionHandler) {
  if (file == nullptr) {
    MINILOG_ERROR("Failed to schedule the segment: the file is null");
    return;
  }

  {
    const std::lock_guard<std::mutex> guard(mScheduleLock);
    mPendingSchedule = Schedule{};
    mPendingSchedule.file = file;
    mPendingSchedule.when = when;
    mPendingSchedule.callbackType = callbackType;
    mPendingSchedule.handler = std::move(completionHandler);
    mPendingSchedule.startFrame = std::max<AudioFramePosition>(startFrame, 0);
    mPendingSchedule.numberFrames = numberFrames;
  }

  requestProducerTick(0);
}

void AudioPlayerNodeImpl::clearSchedule() {
  {
    const std::lock_guard<std::mutex> guard(mScheduleLock);
    mPendingSchedule = Schedule{};
  }
  mActiveSchedule = Schedule{};
  mScheduleActive = false;
  mSourceDone = false;
}

void AudioPlayerNodeImpl::flushRingBuffer() {
  mRingBuffer.reset();
  // The frames that were buffered are gone, so the next frame that is queued is the next
  // one the graph will render.
  mQueuedFrames.store(mRenderedFrames.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------
//  Time conversion
// ---------------------------------------------------------------------------------------

AudioFramePosition AudioPlayerNodeImpl::convertFrames(const AudioFramePosition frames,
  const Float64 from,
  const Float64 to) {
  if (from <= 0 || to <= 0 || from == to) {
    return frames;
  }
  return static_cast<AudioFramePosition>(static_cast<Float64>(frames) * to / from);
}

std::optional<AudioTime>
AudioPlayerNodeImpl::nodeTimeForPlayerTime(const std::optional<AudioTime>& playerTime) const {
  // The documentation is explicit: a player that is not playing has no time mapping.
  const auto sampleRate = mSampleRate.load(std::memory_order_relaxed);
  if (!playerTime.has_value() || sampleRate <= 0 || !isPlaying()) {
    return std::nullopt;
  }
  const auto frames = convertFrames(playerTime->getSampleTime(), playerTime->getSampleRate(), sampleRate);
  return AudioTime(mStartNodeTime.load(std::memory_order_relaxed) + frames, sampleRate, 0);
}

std::optional<AudioTime>
AudioPlayerNodeImpl::playerTimeForNodeTime(const std::optional<AudioTime>& nodeTime) const {
  const auto sampleRate = mSampleRate.load(std::memory_order_relaxed);
  if (!nodeTime.has_value() || sampleRate <= 0 || !isPlaying()) {
    return std::nullopt;
  }
  const auto frames = convertFrames(nodeTime->getSampleTime(), nodeTime->getSampleRate(), sampleRate);
  return AudioTime(frames - mStartNodeTime.load(std::memory_order_relaxed), sampleRate, 0);
}

// ---------------------------------------------------------------------------------------
//  Preparation and the output format
// ---------------------------------------------------------------------------------------

std::optional<AudioFormat> AudioPlayerNodeImpl::resolveProcessingFormat() const {
  // The producer is stopped whenever this runs, so the active schedule is stable.
  // Callers must not hold `mScheduleLock`.
  if (mActiveSchedule.buffer != nullptr) {
    return mActiveSchedule.buffer->getFormat();
  }
  if (mActiveSchedule.file != nullptr) {
    return mActiveSchedule.file->getProcessingFormat();
  }

  const std::lock_guard<std::mutex> guard(mScheduleLock);
  if (mPendingSchedule.buffer != nullptr) {
    return mPendingSchedule.buffer->getFormat();
  }
  if (mPendingSchedule.file != nullptr) {
    return mPendingSchedule.file->getProcessingFormat();
  }
  return std::nullopt;
}

void AudioPlayerNodeImpl::configureOutputFormat(const std::optional<AudioFormat>& outputFormat) {
  // Only the client thread runs this, and only while the producer is stopped and the
  // engine is not rendering, so the ring buffer can be rebuilt safely.
  mRingBufferReady.store(false, std::memory_order_release);

  auto format = outputFormat;
  if (!format.has_value() || !format->isValid()) {
    // Playback may be prepared before the engine is, so the source's own format stands in
    // for the graph's. The engine's prepare replaces it with the real one.
    format = resolveProcessingFormat();
  }
  if (!format.has_value() || !format->isValid()) {
    MINILOG_ERROR("Failed to prepare the player node: the output format is unknown");
    return;
  }

  mOutputFormat = format;
  mSampleRate.store(format->getSampleRate(), std::memory_order_relaxed);

  const auto channels = std::max(format->getChannelCount(), 1u);
  const auto durationFrames = static_cast<AudioFrameCount>(
    format->getSampleRate() * static_cast<Float64>(kRingBufferDurationMs) / Definitions::kMillisPerSecond);
  const auto capacityFrames = std::max(durationFrames, kProducerBlockFrames * kMaxNumberPacket);
  mRingBuffer.init(capacityFrames, channels);
  mInterleaveScratch.assign(static_cast<size_t>(kProducerBlockFrames) * channels, 0.0f);
  mDeinterleaveScratch.assign(
    static_cast<size_t>(std::max(mMaxRenderFrames, kDefaultNumberFrames)) * channels, 0.0f);

  // The decode side depends on the source's processing format, so the producer rebuilds it
  // when it activates a schedule.
  mProcessingFormat.reset();
  mPcmBuffer.reset();
  mConvertedBuffer.reset();
  mConverter.reset();
  mFormatsMatch = false;

  mQueuedFrames.store(mRenderedFrames.load(std::memory_order_relaxed), std::memory_order_relaxed);
  mRingBufferReady.store(true, std::memory_order_release);

  // The frames that were buffered are gone with the old ring buffer, so the schedule that
  // was being served is armed again to replay from its start.
  if (mActiveSchedule.valid()) {
    const std::lock_guard<std::mutex> guard(mScheduleLock);
    if (!mPendingSchedule.valid()) {
      mPendingSchedule = std::move(mActiveSchedule);
    }
  }
  mScheduleActive = false;
  mSourceDone = false;
  mSeekRequest = false;
  mPendingSilenceFrames = 0;
  mSourcePosition = 0;
}

void AudioPlayerNodeImpl::prepareBeforeRendering() {
  // The engine is stopped here: neither the render thread nor this node's producer may
  // touch the ring buffer while it is rebuilt.
  const auto wasRunning = mRunning.load(std::memory_order_acquire);
  stopIOThread();

  configureOutputFormat(renderFormat());

  AudioNodeImpl::prepareBeforeRendering();

  // Restart the producer when the player was working, or when it is expected to work as
  // soon as the engine renders.
  const auto status = mStatus.load(std::memory_order_acquire);
  if (wasRunning || status == Status::kStatusPlaying || status == Status::kStatusPrepared) {
    ensureProducer();
  }
}

void AudioPlayerNodeImpl::prepare(const uint32_t frameCount) {
  mPreparedFrameCount = frameCount;

  {
    const std::lock_guard<std::mutex> guard(mScheduleLock);
    if (!mPendingSchedule.valid()) {
      // Nothing is scheduled: there is nothing to prepare. The active schedule belongs to
      // the IO thread, which is not running here.
      MINILOG_DEBUG("The player node has no scheduled source to prepare");
    }
  }

  // Rebuilding the ring buffer is only safe while the engine is stopped.
  const auto engine = getEngine();
  const bool rendering = engine != nullptr && engine->isRunning();
  if (!mRingBufferReady.load(std::memory_order_acquire) && !rendering) {
    stopIOThread();
    configureOutputFormat(std::nullopt);
  }

  const auto status = mStatus.load(std::memory_order_acquire);
  if (status == Status::kStatusIdle) {
    // Preparing is what makes the producer willing to buffer.
    mStatus.store(Status::kStatusPrepared, std::memory_order_release);
  }

  if (!mRingBufferReady.load(std::memory_order_acquire)) {
    // Without an output format there is nothing to buffer yet; the engine's prepare will
    // establish one.
    return;
  }

  ensureProducer();
  if (frameCount == 0 || mRingBuffer.capacity() == 0) {
    return;
  }

  // "The number of sample frames of data to be prepared before returning": wait, for a
  // bounded time, until the producer has buffered them.
  const auto wanted = std::min<size_t>(frameCount, mRingBuffer.capacity());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPrepareTimeoutMs);
  while (mRingBuffer.available() < wanted && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---------------------------------------------------------------------------------------
//  Transport
// ---------------------------------------------------------------------------------------

void AudioPlayerNodeImpl::playback(const int64_t delayMs) {
  if (isPlaying()) {
    return;
  }

  auto status = mStatus.load(std::memory_order_acquire);
  if (status == Status::kStatusCompleted || status == Status::kStatusFailed) {
    // A finished player starts over from a clean timeline. The schedules the client posted
    // are kept, because only `stop` clears them: the active one is armed again so that it
    // replays from its start.
    stopIOThread();
    flushRingBuffer();
    if (mActiveSchedule.valid()) {
      const std::lock_guard<std::mutex> guard(mScheduleLock);
      if (!mPendingSchedule.valid()) {
        mPendingSchedule = std::move(mActiveSchedule);
      }
    }
    mScheduleActive = false;
    mRenderedFrames.store(0, std::memory_order_relaxed);
    mQueuedFrames.store(0, std::memory_order_relaxed);
    mStartNodeTime.store(0, std::memory_order_relaxed);
    mScheduleRenderedBase = 0;
    mScheduleQueuedFrames = 0;
    mPendingSilenceFrames = 0;
    mSourcePosition = 0;
    mSeekRequest = false;
    mSourceDone = false;
    mCompletionFired = false;
    mPlayedBackDeadline = {};
    mPlaybackStartTime = {};
    mDelayedStart.store(false, std::memory_order_release);
    clearLastRenderTime();
    mStatus.store(Status::kStatusIdle, std::memory_order_release);
    status = Status::kStatusIdle;
  }

  if (status == Status::kStatusIdle) {
    prepare(mPreparedFrameCount);
  }

  if (delayMs > 0) {
    // The player prepares (and keeps buffering) until the requested start time arrives, so
    // the graph renders silence until it is due.
    mPlaybackStartTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    mDelayedStart = true;
    if (mStatus.load(std::memory_order_acquire) == Status::kStatusPlaying) {
      mStatus.store(Status::kStatusPrepared, std::memory_order_release);
    }
  } else {
    mDelayedStart = false;
    mStatus.store(Status::kStatusPlaying, std::memory_order_release);
  }

  ensureProducer();
}

void AudioPlayerNodeImpl::pause() {
  // The producer stops topping the buffer up and the render side stops draining it, so the
  // node's sample time does not advance until playback resumes.
  mStatus.store(Status::kStatusPaused, std::memory_order_release);
}

void AudioPlayerNodeImpl::clearLastRenderTime() {
  const auto engine = getEngine();
  if (engine == nullptr || !engine->isRunning()) {
    mResetLastRenderTimePending.store(false, std::memory_order_release);
    resetLastRenderTime();
    return;
  }
  // The render thread owns the last render time while the graph renders; it clears it on its
  // next pass, after any render that is already running has finished.
  mResetLastRenderTimePending.store(true, std::memory_order_release);
}

void AudioPlayerNodeImpl::stop() {
  // Stopping clears every scheduled event, resets the sample time to zero and leaves the node
  // in the state it starts in, as AVFoundation documents. The status flips first so that the
  // render side stops draining and the producer stops buffering, then the producer is joined:
  // no frame can be queued while the state below is reset.
  mStatus.store(Status::kStatusIdle, std::memory_order_release);
  stopIOThread();

  // A schedule that is stopped reports its completion once. It is reported after the reset,
  // so the handler sees an initial player and content it schedules from it is not wiped by
  // the rest of this call.
  const auto completionType = mActiveSchedule.callbackType;
  const auto completionHandler = mScheduleActive ? mActiveSchedule.handler : CompletionHandler{};
  const bool completionPending = mScheduleActive && !mCompletionFired && completionHandler != nullptr;

  clearSchedule();
  flushRingBuffer();

  mRenderedFrames.store(0, std::memory_order_relaxed);
  mQueuedFrames.store(0, std::memory_order_relaxed);
  mStartNodeTime.store(0, std::memory_order_relaxed);
  mScheduleRenderedBase = 0;
  mScheduleQueuedFrames = 0;
  mPendingSilenceFrames = 0;
  mSourcePosition = 0;
  mSeekRequest = false;
  mSourceDone = false;
  mCompletionFired = false;
  mPlayedBackDeadline = {};
  mPlaybackStartTime = {};
  mDelayedStart.store(false, std::memory_order_release);
  mPreparedFrameCount = kFrameCountInvalid;
  mStatus.store(Status::kStatusIdle, std::memory_order_release);

  // The node's last render time belongs to the timeline that was just reset. The render
  // thread owns that field; while the graph renders, it clears it on its next pass instead of
  // the client thread touching it concurrently.
  clearLastRenderTime();

  if (mError != nullptr) {
    mError->clear();
  }

  if (completionPending) {
    // The schedule has already been cleared, so a handler that stops the player again cannot
    // report it twice. The handler runs last: content it schedules and starts stays in place.
    completionHandler(completionType);
  }
}

// ---------------------------------------------------------------------------------------
//  The producer: the node's IO thread
// ---------------------------------------------------------------------------------------

void AudioPlayerNodeImpl::startIOThread() {
  if (mIOThread != nullptr && mIOThread->isAlive()) {
    return;
  }

  mIOThread = std::make_unique<EventThread>("AudioPlayerNode.IOThread");
  mIOThread->start();
  if (mIOThread->getLooper() == nullptr) {
    MINILOG_ERROR("Failed to start the player node IO thread");
    mIOThread.reset();
    return;
  }
  {
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    mIOHandler = EventHandler::create(mIOThread->getLooper(), nullptr);
  }
  mRunning.store(true, std::memory_order_release);
}

void AudioPlayerNodeImpl::stopIOThread() {
  mRunning.store(false, std::memory_order_release);

  std::shared_ptr<EventHandler> handler;
  {
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    handler = std::move(mIOHandler);
    mIOHandler = nullptr;
  }
  if (handler != nullptr) {
    handler->removeAllCallbacksAndMessages();
  }

  mTickScheduled.store(false, std::memory_order_release);
  disposeEventThread(mIOThread);
}

void AudioPlayerNodeImpl::ensureProducer() {
  startIOThread();
  requestProducerTick(0);
}

void AudioPlayerNodeImpl::requestProducerTick(const int64_t delayMs) {
  std::shared_ptr<EventHandler> handler;
  {
    // The IO thread may be posting its next tick while the client thread stops the player,
    // so the handler is only ever reached through a copy taken under the lock.
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    handler = mIOHandler;
  }
  if (handler == nullptr) {
    return;
  }
  if (mTickScheduled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (delayMs <= 0) {
    handler->post([this] { onProducerTick(); });
  } else {
    handler->postDelay([this] { onProducerTick(); }, std::chrono::milliseconds(delayMs));
  }
}

int64_t AudioPlayerNodeImpl::nextTickDelayMs() const {
  const auto now = std::chrono::steady_clock::now();
  if (mDelayedStart.load(std::memory_order_acquire) && now < mPlaybackStartTime) {
    // Come back when playback is due.
    return std::clamp<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(mPlaybackStartTime - now).count(), 1, 50);
  }

  // Otherwise come back as soon as the ring buffer has room for another block.
  const auto sampleRate = mSampleRate.load(std::memory_order_relaxed);
  const auto framesPerMs = sampleRate > 0 ? sampleRate / Definitions::kMillisPerSecond : 0;
  if (framesPerMs > 0 && mRingBuffer.space() < kProducerBlockFrames) {
    return std::clamp<int64_t>(static_cast<int64_t>(kProducerBlockFrames / (2 * framesPerMs)), 2, 20);
  }
  return 0;
}

void AudioPlayerNodeImpl::onProducerTick() {
  mTickScheduled.store(false, std::memory_order_release);
  {
    static std::atomic_int sProducerTickCount{ 0 };
    if (sProducerTickCount.fetch_add(1, std::memory_order_relaxed) < 5) {
      MINILOG_DEBUG("The producer of the player node ticked, running={}", mRunning.load(std::memory_order_relaxed));
    }
  }
  if (!mRunning.load(std::memory_order_acquire)) {
    return;
  }

  // 1. Take over a schedule the client posted since the last tick.
  activateSchedule();

  // 2. A playback that was asked to start at a specific time starts when it is due.
  if (mDelayedStart.load(std::memory_order_acquire) && std::chrono::steady_clock::now() >= mPlaybackStartTime) {
    mDelayedStart.store(false, std::memory_order_release);
    mStatus.store(Status::kStatusPlaying, std::memory_order_release);
  }

  // 3. Keep the ring buffer topped up while the player is active. A paused player buffers
  //    nothing, and a stopped one has nothing to buffer.
  const auto status = mStatus.load(std::memory_order_acquire);
  if (status == Status::kStatusPlaying || status == Status::kStatusPrepared) {
    produceFrames();
  }

  // 4. Move the completion state of the active schedule forward.
  updateCompletion();

  // 5. Come back while there is more to do. A paused, stopped or completed player goes
  //    idle and is woken by the next schedule, play, pause or stop call.
  if (!mRunning.load(std::memory_order_acquire)) {
    return;
  }
  const auto nextStatus = mStatus.load(std::memory_order_acquire);
  if ((nextStatus == Status::kStatusPlaying || nextStatus == Status::kStatusPrepared) &&
      mRingBufferReady.load(std::memory_order_acquire)) {
    requestProducerTick(nextTickDelayMs());
  }
}

void AudioPlayerNodeImpl::activateSchedule() {
  Schedule schedule;
  {
    const std::lock_guard<std::mutex> guard(mScheduleLock);
    if (!mPendingSchedule.valid()) {
      return;
    }
    schedule = std::move(mPendingSchedule);
    mPendingSchedule = Schedule{};
  }

  // A new schedule takes over from whatever was queued: the frames that have not been
  // rendered yet are dropped, so the new content starts at the node's current time.
  flushRingBuffer();

  mActiveSchedule = std::move(schedule);
  mScheduleActive = true;
  mSourceDone = false;
  mSeekRequest = mActiveSchedule.file != nullptr;
  mSourcePosition = mActiveSchedule.startFrame;
  mPendingSilenceFrames = 0;
  mCompletionFired = false;
  mPlayedBackDeadline = {};
  mScheduleQueuedFrames = 0;
  mScheduleRenderedBase = mRenderedFrames.load(std::memory_order_relaxed);

  const auto sampleRate = mSampleRate.load(std::memory_order_relaxed);
  const auto queued = mQueuedFrames.load(std::memory_order_relaxed);
  if (mActiveSchedule.when.has_value()) {
    // A scheduled start time is a node time, and the node's timeline is the number of
    // frames that have been queued, because the queued frames are rendered in order.
    const auto startFrames =
      convertFrames(mActiveSchedule.when->getSampleTime(), mActiveSchedule.when->getSampleRate(), sampleRate);
    mPendingSilenceFrames = std::max<AudioFramePosition>(0, startFrames - queued);
    mStartNodeTime.store(startFrames, std::memory_order_relaxed);
  } else {
    mStartNodeTime.store(queued, std::memory_order_relaxed);
  }

  // The source decides the processing format, and therefore the converter the producer
  // needs to reach the output format.
  const auto processingFormat = resolveProcessingFormat();
  const auto outputFormat = mOutputFormat;
  if (!processingFormat.has_value() || !processingFormat->isValid() || !outputFormat.has_value() ||
      !outputFormat->isValid()) {
    MINILOG_ERROR("Failed to start the scheduled source: the audio format is unknown");
    mSourceDone = true;
    return;
  }

  mFormatsMatch = processingFormat.value() == outputFormat.value();
  mProcessingFormat = processingFormat;
  mPcmBuffer = AudioPCMBuffer::InitWithPCMFormat(processingFormat.value(), kProducerBlockFrames);
  mConverter.reset();
  mConvertedBuffer.reset();

  if (mFormatsMatch) {
    mConvertedBuffer = AudioPCMBuffer::InitWithPCMFormat(outputFormat.value(), kProducerBlockFrames);
  } else {
    mConverter = AudioConverter::InitFromFormat(processingFormat.value(), outputFormat.value());
    if (mConverter != nullptr) {
      // A file with more channels than the graph is mixed down rather than truncated.
      mConverter->setDownMix(true);
    }
    const auto ratio =
      outputFormat->getSampleRate() / std::max<Float64>(processingFormat->getSampleRate(), 1.0);
    const auto capacity = static_cast<AudioFrameCount>(
      static_cast<Float64>(kProducerBlockFrames) * (std::max<Float64>(ratio, 1.0) + 1.0));
    mConvertedBuffer = AudioPCMBuffer::InitWithPCMFormat(outputFormat.value(), capacity);
  }

  if (mPcmBuffer == nullptr || mConvertedBuffer == nullptr ||
      (!mFormatsMatch && mConverter == nullptr)) {
    MINILOG_ERROR("Failed to start the scheduled source: the audio formats cannot be converted");
    mSourceDone = true;
    return;
  }

  // The interleaved staging buffer has to hold whatever the conversion produced.
  const auto channels = std::max(outputFormat->getChannelCount(), 1u);
  mInterleaveScratch.assign(
    static_cast<size_t>(std::max(mConvertedBuffer->getFrameCapacity(), kProducerBlockFrames)) * channels, 0.0f);

  // Room for one produced block is what the producer reserves before it converts, including the
  // frames a fractional sample rate ratio adds on top of the block that was read.
  const auto ratio = outputFormat->getSampleRate() / std::max<Float64>(processingFormat->getSampleRate(), 1.0);
  mQueueBlockFrames = static_cast<AudioFrameCount>(
                        static_cast<Float64>(kProducerBlockFrames) * std::max<Float64>(ratio, 1.0)) +
    2;
}

AudioFrameCount AudioPlayerNodeImpl::produceFrames() {
  if (!mRingBufferReady.load(std::memory_order_acquire) || mRingBuffer.capacity() == 0) {
    return 0;
  }

  AudioFrameCount queued = 0;
  // A block that is converted can be longer than the block that was read from the file, so the
  // room has to be reserved for what the conversion will produce, not for what it consumes.
  while (mRingBuffer.space() >= mQueueBlockFrames) {
    // Silence first: it is what delays a scheduled start time, and it keeps the node's
    // timeline in step with the graph's.
    if (mPendingSilenceFrames > 0) {
      const auto frames = static_cast<AudioFrameCount>(
        std::min<AudioFramePosition>(mPendingSilenceFrames, kProducerBlockFrames));
      queueSilence(frames);
      mPendingSilenceFrames -= frames;
      queued += frames;
      continue;
    }

    if (!mScheduleActive || mSourceDone) {
      break;
    }

    const auto produced = produceNextBlock(kProducerBlockFrames);
    if (produced == 0) {
      break;
    }
    queued += produced;
  }
  return queued;
}

AudioFrameCount AudioPlayerNodeImpl::produceNextBlock(const AudioFrameCount maxFrames) {
  if (!mScheduleActive || mPcmBuffer == nullptr) {
    mSourceDone = true;
    return 0;
  }

  // ---- a file, or a segment of one ----
  if (mActiveSchedule.file != nullptr) {
    const auto file = mActiveSchedule.file;
    const auto length = file->getLength();
    auto end = length;
    if (mActiveSchedule.numberFrames != kFrameCountInvalid) {
      end = std::min<AudioFramePosition>(length, mActiveSchedule.startFrame + mActiveSchedule.numberFrames);
    }

    if (mSeekRequest) {
      file->setFramePosition(mActiveSchedule.startFrame);
      if (mConverter != nullptr) {
        // The resampler keeps state that a seek invalidates.
        mConverter->reset();
      }
      mSourcePosition = mActiveSchedule.startFrame;
      mSeekRequest = false;
    }

    if (mSourcePosition >= end) {
      mSourceDone = true;
      return 0;
    }

    const auto wanted = static_cast<AudioFrameCount>(std::min<AudioFramePosition>(maxFrames, end - mSourcePosition));
    auto error = getOutError();
    if (!file->readIntoBuffer(mPcmBuffer, wanted, &error)) {
      const auto position = file->getFramePosition();
      if (position >= end || position >= length) {
        mSourceDone = true;
        return 0;
      }
      logError(ErrCode::kUnknown, "Failed to read audio into the buffer from the AudioFile",
        error != nullptr ? error->toString().c_str() : "");
      mStatus.store(Status::kStatusFailed, std::memory_order_release);
      mSourceDone = true;
      return 0;
    }

    const auto frames = mPcmBuffer->getFrameLength();
    if (frames == 0) {
      mSourceDone = true;
      return 0;
    }
    mSourcePosition += frames;
    return queueProcessingBuffer(frames);
  }

  // ---- a scheduled buffer ----
  const auto buffer = mActiveSchedule.buffer.get();
  if (buffer == nullptr) {
    mSourceDone = true;
    return 0;
  }
  const auto total = buffer->getFrameLength();
  if (total <= 0) {
    mSourceDone = true;
    return 0;
  }

  auto position = mSourcePosition;
  if (position >= total) {
    if (mActiveSchedule.options == BufferOptions::kBufferLoops) {
      position = 0;
    } else {
      mSourceDone = true;
      return 0;
    }
  }

  const auto frames = static_cast<AudioFrameCount>(std::min<AudioFramePosition>(maxFrames, total - position));
  const auto copied = AudioGraphUtils::copySameFormatPCMBuffer(*buffer, static_cast<AudioFrameCount>(position),
    *mPcmBuffer, 0, frames);
  if (copied == 0) {
    mSourceDone = true;
    return 0;
  }
  mSourcePosition = position + copied;
  return queueProcessingBuffer(copied);
}

void AudioPlayerNodeImpl::queueSilence(const AudioFrameCount frames) {
  const auto channels = mRingBuffer.channels();
  const auto count = static_cast<size_t>(frames) * channels;
  if (mInterleaveScratch.size() < count) {
    return;
  }
  std::fill_n(mInterleaveScratch.begin(), static_cast<std::ptrdiff_t>(count), 0.0f);
  if (!mRingBuffer.write(mInterleaveScratch.data(), frames)) {
    return;
  }
  mQueuedFrames.fetch_add(frames, std::memory_order_release);
  // The silence is part of what the schedule has to render: without counting it here, a
  // schedule that starts later would look finished as soon as its delay had been rendered.
  mScheduleQueuedFrames += frames;
}

AudioFrameCount AudioPlayerNodeImpl::queueProcessingBuffer(const AudioFrameCount frames) {
  if (frames == 0) {
    return 0;
  }

  const AudioPCMBuffer* source = nullptr;
  if (mFormatsMatch) {
    mPcmBuffer->setFrameLength(frames);
    source = mPcmBuffer.get();
  } else {
    if (mConverter == nullptr || mConvertedBuffer == nullptr) {
      mSourceDone = true;
      return 0;
    }
    mPcmBuffer->setFrameLength(frames);
    if (const auto error = mConverter->convertToBuffer(mPcmBuffer, mConvertedBuffer); error.has_value()) {
      logError(ErrCode::kUnknown, "Failed to convert the player node's audio", error->toString().c_str());
      mStatus.store(Status::kStatusFailed, std::memory_order_release);
      mSourceDone = true;
      return 0;
    }
    if (mConvertedBuffer->getFrameLength() == 0) {
      // The converter needs more input before it can produce output.
      return 0;
    }
    source = mConvertedBuffer.get();
  }

  const auto interleaved = AudioGraphUtils::toInterleavedFloat(*source, mInterleaveScratch.data(),
    source->getFrameLength());
  if (interleaved == 0) {
    return 0;
  }
  // The producer reserved room for a whole converted block, so a short write means the
  // conversion grew past its reservation. What does not fit is dropped and not accounted for,
  // which keeps the reported queue in step with what the graph will actually render.
  auto toWrite = interleaved;
  if (const auto space = static_cast<AudioFrameCount>(mRingBuffer.space()); toWrite > space) {
    MINILOG_WARN("The player node's producer dropped {} frames: the ring buffer has no room", toWrite - space);
    toWrite = space;
  }
  if (toWrite == 0 || !mRingBuffer.write(mInterleaveScratch.data(), toWrite)) {
    return 0;
  }

  mQueuedFrames.fetch_add(toWrite, std::memory_order_release);
  mScheduleQueuedFrames += toWrite;
  return toWrite;
}

// ---------------------------------------------------------------------------------------
//  Completion
// ---------------------------------------------------------------------------------------

void AudioPlayerNodeImpl::fireCompletion(const CompletionCallbackType callbackType) {
  if (mCompletionFired) {
    return;
  }
  mCompletionFired = true;
  const auto handler = mActiveSchedule.handler;
  if (handler) {
    handler(callbackType);
  }
}

void AudioPlayerNodeImpl::updateCompletion() {
  if (!mScheduleActive) {
    return;
  }

  const auto rendered = mRenderedFrames.load(std::memory_order_acquire);
  const bool renderedAll = (rendered - mScheduleRenderedBase) >= mScheduleQueuedFrames;
  const bool drained = mSourceDone && renderedAll;

  if (drained) {
    mStatus.store(Status::kStatusCompleted, std::memory_order_release);
  }

  if (mCompletionFired || !mActiveSchedule.handler) {
    return;
  }

  switch (mActiveSchedule.callbackType) {
    case CompletionCallbackType::kConsumed:
      // The source has been handed over to the graph.
      if (mSourceDone) {
        fireCompletion(CompletionCallbackType::kConsumed);
      }
      break;

    case CompletionCallbackType::kRendered:
      if (drained) {
        fireCompletion(CompletionCallbackType::kRendered);
      }
      break;

    case CompletionCallbackType::kPlayedBack:
    default:
      if (!drained) {
        break;
      }
      if (mPlayedBackDeadline == TimePoint{}) {
        // "Finished playing" means the frames have reached the output, which is the render
        // plus whatever latency the device adds. Without a device there is nothing to add.
        const auto latencySeconds = static_cast<Float64>(getLatency() + getOutputPresentationLatency());
        const auto delayMs = std::clamp(
          static_cast<int64_t>(latencySeconds * static_cast<Float64>(Definitions::kMillisPerSecond)), int64_t{ 0 },
          static_cast<int64_t>(Definitions::kMillisPerSecond));
        mPlayedBackDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
      }
      if (std::chrono::steady_clock::now() >= mPlayedBackDeadline) {
        fireCompletion(CompletionCallbackType::kPlayedBack);
      }
      break;
  }
}

// ---------------------------------------------------------------------------------------
//  The consumer: the render graph
// ---------------------------------------------------------------------------------------

void AudioPlayerNodeImpl::renderBlock(const AudioBufferList* const* /* inputs */,
  const uint32_t /* numInputs */,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& when) {
  // A player node has no inputs: it generates its own frames. The render buffer is silent
  // before this call, so a player that has nothing to render leaves silence behind.
  if (outputData == nullptr || numFrames == 0) {
    return;
  }

  if (mResetLastRenderTimePending.exchange(false, std::memory_order_acq_rel)) {
    // A stop reset the timeline while the graph was rendering: this thread owns the node's
    // last render time, so it clears it here instead of the client thread touching it.
    resetLastRenderTime();
  }

  // A player that is not playing does not advance its sample time, so the frames stay in
  // the ring buffer for the resume.
  if (mStatus.load(std::memory_order_acquire) != Status::kStatusPlaying) {
    return;
  }
  if (!mRingBufferReady.load(std::memory_order_acquire) || mRingBuffer.capacity() == 0) {
    return;
  }

  const auto channels = mRingBuffer.channels();
  const auto available = static_cast<AudioFrameCount>(std::min<size_t>(mRingBuffer.available(), numFrames));
  if (available == 0 || mDeinterleaveScratch.size() < static_cast<size_t>(numFrames) * channels) {
    // The producer has not buffered the next block yet.
    return;
  }

  const auto read = static_cast<AudioFrameCount>(mRingBuffer.read(mDeinterleaveScratch.data(), available));
  if (read == 0) {
    return;
  }
  if (AudioGraphUtils::interleavedFloatIntoBufferList(mDeinterleaveScratch.data(), *outputData, read) > 0) {
    const auto position = mRenderedFrames.fetch_add(read, std::memory_order_release) + read;
    updateLastRenderTime(position, renderSampleRate(when));
  }
}

Error* AudioPlayerNodeImpl::getOutError() const {
  if (mError) {
    mError->clear();
  }
  return mError.get();
}

} // namespace coral

