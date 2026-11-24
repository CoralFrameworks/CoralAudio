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

#include "AudioRecorderImpl.h"

#include "private/audio/AudioStreamBuilder.h"
#include "private/audio/AudioStreamTypes.h"

#include <coral/audio/Resample.h>
#include <core/time/SystemClock.h>

#include <core/event/EventLooper.h>

#include <media/Utilities.h>
#include <minilog/log.h>
#include <utils/Path.h>

#include <algorithm>
#include <chrono>
#include <utility>

using namespace utils;

namespace coral {

namespace {

/**
 * Quits an event thread and disposes of it.
 *
 * A thread that quits from its own looper thread can only be detached, and the detached thread goes
 * on touching its `EventThread` object while it unwinds, so destroying that object here would be a
 * use after free. The case arises when the delegate of the recorder releases it from the callback
 * that reports the end of the recording, which runs on the message thread.
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

std::shared_ptr<AudioRecorderImpl>
AudioRecorderImpl::Make(const std::string& url, MediaFileType fileType, const AudioFormat& format) {
  const auto sampleFormat = format.getSampleFormat();
  if (fileType == MediaFileType::kMediaFileTypeWav) {
    // We only supports 16bit when recording audio data into wav file.
    if (sampleFormat != AudioSampleFormat::kInt16) {
      MINILOG_ERROR("Failed to configure the recorder: {}", convertToText(sampleFormat));
      return {};
    }
  } else {
    if (sampleFormat != AudioSampleFormat::kInt16 &&
        sampleFormat != AudioSampleFormat::kFloat) {
      MINILOG_ERROR("Failed to configure the recorder: {}", convertToText(sampleFormat));
      return {};
    }
  }
  return std::make_shared<AudioRecorderImpl>(url, fileType, format);
}

AudioRecorderImpl::AudioRecorderImpl(std::string url, const MediaFileType fileType, AudioFormat format)
  : AudioRecorder(std::move(url), fileType, std::move(format)) {
  mError = std::make_unique<Error>();
  startMessageThread();
}

AudioRecorderImpl::~AudioRecorderImpl() {
  // The message thread is stopped before the resources it works with are released.
  stopMessageThread();
  release();
}

// ==================================================================================================
// The message thread
// ==================================================================================================

void AudioRecorderImpl::startMessageThread() {
  if (mThread != nullptr && mThread->isAlive()) {
    return;
  }

  mThread = std::make_unique<EventThread>(getName() + ".MessageThread");
  mThread->start();
  if (mThread->getLooper() == nullptr) {
    MINILOG_ERROR("Failed to start the recorder thread");
    mThread.reset();
    return;
  }
  // A handler holds its callback weakly, so the reference has to be kept alive here. The deleter does
  // nothing, because the recorder is owned by its caller and not by the reference.
  mCallbackRef = std::shared_ptr<EventHandler::Callback>(this, [](EventHandler::Callback*) {
  });
  const std::lock_guard<std::mutex> guard(mHandlerLock);
  mHandler = EventHandler::create(mThread->getLooper(), mCallbackRef);
}

void AudioRecorderImpl::stopMessageThread() {
  mWriteScheduled.store(false, std::memory_order_release);

  std::shared_ptr<EventHandler> handler;
  {
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    handler = std::move(mHandler);
    mHandler = nullptr;
  }
  if (handler != nullptr) {
    // The events that were not handled belong to a recorder that is going away.
    handler->removeAllCallbacksAndMessages();
  }
  disposeEventThread(mThread);
  mCallbackRef.reset();
}

bool AudioRecorderImpl::sendEvent(const int what) {
  std::shared_ptr<EventHandler> handler;
  {
    // The message thread may be replaced while it posts, so the handler is only ever reached through
    // a copy taken under the lock.
    const std::lock_guard<std::mutex> guard(mHandlerLock);
    handler = mHandler;
  }
  return handler != nullptr && handler->sendEmptyMessage(what);
}

bool AudioRecorderImpl::sendEventDelayed(const int what, const TimeDelta delay) {
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

bool AudioRecorderImpl::isOnMessageThread() const {
  if (mThread == nullptr) {
    return false;
  }
  const auto& looper = mThread->getLooper();
  return looper != nullptr && looper->isCurrentThread();
}

bool AudioRecorderImpl::handleMessage(Message* msg) {
  switch (msg->what) {
    case MSG_PREPARE: {
      onPrepare();
      break;
    }
    case MSG_START_RECORDING: {
      startInternal();
      break;
    }
    case MSG_WRITE_AUDIO: {
      // The event is cleared before the writing, so that the capture side can ask for another one
      // while this one drains the buffer.
      mWriteScheduled.store(false, std::memory_order_release);
      writePendingAudioData();
      break;
    }
    case MSG_STOP_RECORDING: {
      stopInternal();
      break;
    }
    default: {
      break;
    }
  }
  return true;
}

void AudioRecorderImpl::requestWriteEvent() {
  if (mWriteScheduled.exchange(true, std::memory_order_acq_rel)) {
    // A write event is already queued and it drains the whole ring buffer, so it covers this block.
    return;
  }
  if (!sendEvent(MSG_WRITE_AUDIO)) {
    mWriteScheduled.store(false, std::memory_order_release);
  }
}

void AudioRecorderImpl::writePendingAudioData() {
  while (hasRemainingAudioData()) {
    if (!resampleAndWriteToFile()) {
      // Either the file could not be written, which was reported to the delegate, or the block that
      // was read could not be turned into output frames: another pass would make no progress.
      break;
    }
  }
}

void AudioRecorderImpl::release() {
  if (mAudioStream) {
    mAudioStream->close();
  }
  if (mAudioFile) {
    mAudioFile->release();
  }
  mAudioStream.reset();
  mAudioFile.reset();
}

bool AudioRecorderImpl::isRecording() const {
  return mStatus == Status::kRecording
         && (mAudioStream != nullptr && mAudioStream->getState() == StreamState::Started);
}

DataCallbackResult
AudioRecorderImpl::onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) {
  if (isRecording()) {
    storeAudioData(stream, audioData, numFrames);
    // waiting for available frames.
    stream->waitForAvailableFrames(numFrames, Definitions::kNanosPerSecond);
    return DataCallbackResult::Continue;
  } else {
    return DataCallbackResult::Stop;
  }
}

void AudioRecorderImpl::onPrepare() {
#ifndef NDEBUG
  MINILOG_DEBUG("prepare for recoding...");
#endif
  const auto error = getOutError();
  if (!initAudioFileAndStream(error)) {
    mStatus = Status::kFailed;
    notifyRecordingFailed(error);
  } else {
    mStatus = Status::kPrepared;
  }
}

void AudioRecorderImpl::onPrepareAsync() {
#ifndef NDEBUG
  MINILOG_DEBUG("async prepare for recoding...");
#endif
  sendEvent(MSG_PREPARE);
}

void AudioRecorderImpl::onStartRecording(const int64_t timeUs, const int64_t durationUs) {
  mMaxRecordDurationUs = durationUs;
  if (mStatus != Status::kRecording) {
    mStatus = Status::kRecording;
    const auto delayUs = std::max(timeUs - getDeviceCurrentTimeUs(), int64_t(0));
    sendEventDelayed(MSG_START_RECORDING, std::chrono::microseconds(delayUs));
  }
}

void AudioRecorderImpl::onStop() {
  mStatus = Status::kStopped;
  if (isOnMessageThread()) {
    // A caller that stops the recorder from the message thread, e.g. a delegate that stops it when
    // the recording ends, cannot wait for that thread: the flush runs here.
    stopInternal();
  } else {
    mStopBarrier.reset(1);
    if (!sendEvent(MSG_STOP_RECORDING)) {
      mStopBarrier.latch();
    }
    // Stopping waits for the buffered audio to reach the file, which the stop event reports.
    mStopBarrier.await();
  }
  release();
}

void AudioRecorderImpl::startInternal() {
  if (mAudioStream != nullptr) {
    mAudioStream->start();
  }
  writePendingAudioData();
}

void AudioRecorderImpl::stopInternal() {
  if (mAudioStream != nullptr) {
    mAudioStream->stop();
  }
  // The capture stream is stopped, so the frames that are still buffered are the last ones: they are
  // all written before the file is closed.
  writePendingAudioData();
  release();
  notifyRecordingFinish(mStatus != Status::kFailed);
}

bool AudioRecorderImpl::initAudioFileAndStream(Error* error) {
  auto builder = AudioStreamBuilder();
  auto result = builder.setDirection(Direction::Capture)
                       .setChannelCount(mAudioFormat.getChannelCount())
                       .setSampleRate(mAudioFormat.getSampleRate())
                       .setFormat(mAudioFormat.getSampleFormat())
                       .setDataCallback(shared_from_this())
                       .openStream(mAudioStream);
  if (result != Result::OK) {
    mAudioStream.reset();
    handleError(error, static_cast<ErrCode>(result),
      "Failed to create and open audio stream!", convertToText(result));
    return false;
  }

  mAudioFile = AudioFile::initForWriting(mUrl, mFileType, mAudioFormat, &error);
  if (!mAudioFile) {
    logError(ErrCode::kNotInit, "Failed to create AudioFile for recording");
    return false;
  }

  const auto processingFormat = mAudioFile->getProcessingFormat();

  // checks if we should resample audio data
  const auto outputConfig = AudioConfiguration(processingFormat->getSampleRate(),
    processingFormat->getChannelCount(),
    processingFormat->getSampleFormat());
  const auto inputConfig = AudioConfiguration(mAudioStream->getSampleRate(),
    mAudioStream->getChannelCount(),
    mAudioStream->getFormat());
  if (inputConfig != outputConfig) {
    mResample = CreateBuiltinResampleFactory()->createResample(inputConfig, outputConfig);
    if (!mResample) {
      logError(ErrCode::kNotInit, "Failed to create resample");
      return false;
    }
    MINILOG_ERROR("Needs resample input: {}, output: {}", convertToText(inputConfig.sampleFormat),
      convertToText(outputConfig.sampleFormat));
  }

  uint32_t maxNumFrames = kMaxCacheDuration * mAudioStream->getSampleRate();
  maxNumFrames = std::max(maxNumFrames, mAudioStream->getBufferCapacityInFrames());
  const auto framesPerCallback = std::max(mAudioStream->getFramesPerBurst(), kDefaultFrameSize);
  // The scratch buffer is resized rather than reserved: it is written through its data(), and the
  // resampler reads more frames than a single callback of the stream may hand over.
  mAppBuffer.resize(framesPerCallback * mAudioStream->getBytesPerFrame());
  mRingBuffer.init(maxNumFrames, mAudioStream->getChannelCount());
  mCaptureFormat = mAudioStream->getFormat();

  // only interleaved formats are supported for the saved file
  const auto bufferFormat =
    AudioFormat::InitWithCommonFormat(processingFormat->getSampleFormat(),
      processingFormat->getSampleRate(),
      processingFormat->getChannelCount(),
      true);
  mPcmBuffer = AudioPCMBuffer::InitWithPCMFormat(bufferFormat.value(), framesPerCallback);
  if (!processingFormat->isInterleaved()) {
    MINILOG_WARN("Ignore non interleaved setting when recording.");
  }
  return true;
}

void AudioRecorderImpl::storeAudioData(const AudioStream* stream,
  const void* audioData,
  const AudioFrameCount numFrames) {
  if (mStatus == Status::kRecording && stream != nullptr && numFrames > 0) {
    bool result = false;
    switch (stream->getFormat()) {
      case AudioSampleFormat::kFloat: {
        result = mRingBuffer.write(static_cast<const float*>(audioData), numFrames);
        break;
      }
      case AudioSampleFormat::kInt16: {
        result = mRingBuffer.writeInt16(static_cast<const int16_t*>(audioData), numFrames);
        break;
      }
      case AudioSampleFormat::kInt24: {
        result = mRingBuffer.writeInt24(static_cast<const uint8_t*>(audioData), numFrames);
        break;
      }
      case AudioSampleFormat::kInt32: {
        result = mRingBuffer.writeInt32(static_cast<const int32_t*>(audioData), numFrames);
        break;
      }
      default: {
        break;
      }
    }
    if (result) {
      const auto bufferSize = numFrames * stream->getBytesPerFrame();
      mCurrentTimeUs +=
        MediaUtils::getPcmDurationUs(stream->getSampleRate(),
          stream->getChannelCount(),
          stream->getFormat(),
          bufferSize);
      if (mMaxRecordDurationUs > 0 && mMaxRecordDurationUs <= mCurrentTimeUs) {
        mStatus = Status::kStopped;
      }
      // The frames that were stored are what the write event is for.
      requestWriteEvent();
    }
  }
}

bool AudioRecorderImpl::resampleAndWriteToFile() {
  if (!mAudioFile) {
    return false;
  }
  AudioFrameCount framesToWrite = 0;
  if (mResample != nullptr) {
    AudioFrameCount framesRead = 0;
    switch (mCaptureFormat) {
      case AudioSampleFormat::kFloat: {
        framesRead = mRingBuffer.read(reinterpret_cast<float*>(mAppBuffer.data()), kDefaultFrameSize);
        break;
      }
      case AudioSampleFormat::kInt16: {
        framesRead = mRingBuffer.readInt16(reinterpret_cast<int16_t*>(mAppBuffer.data()), kDefaultFrameSize);
        break;
      }
      case AudioSampleFormat::kInt24: {
        framesRead = mRingBuffer.readInt24(mAppBuffer.data(), kDefaultFrameSize);
        break;
      }
      case AudioSampleFormat::kInt32: {
        framesRead = mRingBuffer.readInt32(reinterpret_cast<int32_t*>(mAppBuffer.data()), kDefaultFrameSize);
        break;
      }
      default: {
        break;
      }
    }
    if (framesRead > 0) {
      framesToWrite = mResample->resample(mAppBuffer.data(), framesRead,
          static_cast<uint8_t*>(mPcmBuffer->rawData()), kDefaultFrameSize);
    }
  } else {
    switch (mCaptureFormat) {
      case AudioSampleFormat::kFloat: {
        framesToWrite = mRingBuffer.read(static_cast<float*>(mPcmBuffer->rawData()), kDefaultFrameSize);
        break;
      }
      case AudioSampleFormat::kInt16: {
        framesToWrite = mRingBuffer.readInt16(static_cast<int16_t*>(mPcmBuffer->rawData()), kDefaultFrameSize);
        break;
      }
      case AudioSampleFormat::kInt24: {
        framesToWrite = mRingBuffer.readInt24(static_cast<uint8_t*>(mPcmBuffer->rawData()), kDefaultFrameSize);
        break;
      }
      case AudioSampleFormat::kInt32: {
        framesToWrite = mRingBuffer.readInt32(static_cast<int32_t*>(mPcmBuffer->rawData()), kDefaultFrameSize);
        break;
      }
      default: {
        break;
      }
    }
  }
  if (framesToWrite <= 0) {
    // The block that was read could not be turned into output frames, e.g. because the resampler
    // needs more input: the next write event tries again with the frames that follow.
    return false;
  }

  mPcmBuffer->setFrameLength(framesToWrite);
  auto error = getOutError();
  if (!mAudioFile->writeFromBuffer(mPcmBuffer, &error)) {
    notifyRecordingFailed(error);
    return false;
  }
  return true;
}

bool AudioRecorderImpl::hasRemainingAudioData() const {
  return mRingBuffer.available() > 0;
}

void AudioRecorderImpl::notifyRecordingFinish(bool successfully) {
  mStatus = Status::kCompleted;
  if (mDelegate != nullptr) {
    mDelegate->onFinishRecording(this, successfully);
  }
  mStopBarrier.latch();
}

void AudioRecorderImpl::notifyRecordingFailed(const Error* error) {
  mStatus = Status::kFailed;
  if (mDelegate != nullptr) {
    mDelegate->onEncodeErrorOccur(this, error);
  }
  mStopBarrier.latch();
}

void AudioRecorderImpl::handleError(Error* outError,
  const ErrCode errCode,
  const std::string& errorMsg,
  const std::string& description) {
  if (outError) {
    outError->code = errCode;
    outError->failureReason = errorMsg;
    outError->description = description;
  }
#ifndef NDEBUG
  logError(errCode, errorMsg, description);
#endif
}

void AudioRecorderImpl::logError(const ErrCode errCode,
  const std::string& errorMsg,
  const std::string& description) {
  MINILOG_ERROR("name: {}, error: {}, msg: {}, description: {}",
    getName(),
    static_cast<int32_t>(errCode),
    errorMsg,
    (description.empty() ? "" : description));
}

} // namespace coral
