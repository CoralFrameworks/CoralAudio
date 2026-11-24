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

#include "AudioOutputSinkImpl.h"

#include "private/audio/AudioStreamBuilder.h"

#include <coral/audio/Resample.h>

#include <media/Types.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <cstdlib>

namespace coral {

AudioOutputSinkImpl::AudioOutputSinkImpl()
  : framesWritten(0),
    opened(false),
    endOfStream(false),
    mSonic(nullptr),
    mPitch(1.0f),
    mRate(1.0f),
    mVolume(1.0f),
    startMediaTimeUsNeedsInit(false),
    startPositionUs(0),
    deviceFramesRead(-1),
    currentPositionUs(CURRENT_POSITION_NOT_SET) {
}

AudioOutputSinkImpl::~AudioOutputSinkImpl() {
  release();
}

void AudioOutputSinkImpl::setListener(const std::shared_ptr<Listener>& listener) {
  mListener = listener;
}

int64_t AudioOutputSinkImpl::deviceFramesReadNow() {
  if (!isAudioStreamInitialized()) {
    return -1;
  }
  return mAudioStream->getFramesRead();
}

int64_t AudioOutputSinkImpl::getCurrentPositionUs(bool sourceEnded) {
  if (!isAudioStreamInitialized() || startMediaTimeUsNeedsInit) {
    return CURRENT_POSITION_NOT_SET;
  }

  if (outputConfig.sampleRate <= 0) {
    return CURRENT_POSITION_NOT_SET;
  }
  // ExoPlayer's DefaultAudioSink semantics: the position is anchored at the media time the frames
  // written so far start at, advanced by the frames that have been played back since that anchor
  // and clamped to the frames written since it. Counting the frames written alone would report the
  // buffered audio as if it had already been played, which made the position run past the duration.
  auto playedFrames = framesWritten.load();
  if (deviceFramesRead >= 0) {
    const auto framesRead = mAudioStream->getFramesRead();
    if (framesRead > 0) {
      // The device counter measures the whole stream, so what advances the position is what it grew
      // by since the anchor, not what it reports: it does not restart when the client is reset. The
      // frames written since the anchor stay the upper bound, as ExoPlayer clamps them too. The
      // counter is not usable on every platform (it was observed to stay at or below zero on
      // AAudio), in which case the frames written are all there is to go by.
      playedFrames = std::min(playedFrames, std::max(int64_t{ 0 }, framesRead - deviceFramesRead));
    }
  }
  currentPositionUs =
    startPositionUs
    + std::div(playedFrames * Definitions::kMicrosPerSecond, outputConfig.sampleRate).quot;
  return currentPositionUs;
}

void AudioOutputSinkImpl::configure(const AudioFormat& format) {
  if (format.getSampleFormat() != AudioSampleFormat::kFloat &&
      format.getSampleFormat() != AudioSampleFormat::kInt16) {
    MINILOG_ERROR("Only support 16-bit data or floating data for the AudioOutput: {}",
      convertToText(format.getSampleFormat()));
    return;
  }

  // Configuration for the input sample data
  inputConfig.sampleRate = format.getSampleRate();
  inputConfig.channelCount = format.getChannelCount();
  inputConfig.sampleFormat = format.getSampleFormat();

  // Configuration for the AudioStream
  outputConfig.sampleRate = format.getSampleRate();
  outputConfig.channelCount = format.getChannelCount();
  if (mEnableFloatOutput && format.getSampleFormat() == AudioSampleFormat::kFloat) {
    outputConfig.sampleFormat = AudioSampleFormat::kFloat;
  } else {
    outputConfig.sampleFormat = AudioSampleFormat::kInt16;
  }
  initializeAudioStream();
}

void AudioOutputSinkImpl::initializeAudioStream() {
  auto builder = AudioStreamBuilder();
  const auto result =
    builder.setDirection(Direction::Render)
           .setChannelCount(outputConfig.channelCount)
           .setSampleRate(outputConfig.sampleRate)
           .setFormat(outputConfig.sampleFormat)
           .setDataCallback(shared_from_this())
           .openStream(mAudioStream);
  if (result != Result::OK) {
    mAudioStream.reset();
    MINILOG_ERROR("Failed to create AudioStream: {}", convertToText(result));
    return;
  }

  // use sonic to handle rate and volume processing
  mSonic = sonicCreateStream(mAudioStream->getSampleRate(), mAudioStream->getChannelCount());
  sonicSetPitch(mSonic, mPitch);
  sonicSetRate(mSonic, mRate);
  sonicSetVolume(mSonic, mVolume);
  sonicFlushStream(mSonic);

  // create audio fifo
  uint32_t maxCachedFrames = kDefaultCacheDuration * mAudioStream->getSampleRate();
  maxCachedFrames = std::max(maxCachedFrames, mAudioStream->getBufferCapacityInFrames());
  // The scratch buffers are read into through their data(), and the callback is handed at most
  // the number of frames the stream reports as its callback size, so each is sized for that many
  // frames of its own sample type: a callback of `frames` frames carries `frames * channels`
  // samples of it, whatever the bytes per frame of the device format are.
  const auto channels = std::max<uint32_t>(mAudioStream->getChannelCount(), 1u);
  const auto frames = std::max(mAudioStream->getFramesPerDataCallback(), kDefaultNumberFrames);
  mShortBuffer.resize(static_cast<size_t>(frames) * channels);
  mFloatBuffer.resize(static_cast<size_t>(frames) * channels);
  mRingBuffer.init(maxCachedFrames, mAudioStream->getChannelCount());

  framesWritten = 0;
  opened = true;
  endOfStream = false;
  startPositionUs = 0;
  currentPositionUs = CURRENT_POSITION_NOT_SET;

  // The renderer can ask the sink to play before the stream exists: the stream is opened on the
  // first buffer, and a source that has its data ready can reach the first buffer after the
  // renderer was started. Without this the device is never started and silently consumes nothing,
  // which leaves the queue full and the playback position stuck at zero.
  if (playRequested) {
    mAudioStream->start();
  }
}

void AudioOutputSinkImpl::play() {
  playRequested = true;
  if (isAudioStreamInitialized()) {
    mAudioStream->start();
  }
}

void AudioOutputSinkImpl::handleDiscontinuity() {
  // Force resynchronization after a skipped buffer.
  startMediaTimeUsNeedsSync = true;
}

bool AudioOutputSinkImpl::handleBuffer(const void* buffer,
  const size_t byteBufferSize,
  const int64_t presentationTimeUs) {

  if (!opened || endOfStream || !hasCapacityForNextBuffer()) {
    return false;
  }

  if (!isAudioStreamInitialized()) {
    initializeAudioStream();
  }

  if (startMediaTimeUsNeedsInit) {
    // The first buffer written after a flush (or after a reconfiguration) starts the new position
    // timeline. The frames written before it do not belong to that timeline and are not counted.
    startPositionUs = std::max(static_cast<int64_t>(0), presentationTimeUs);
    framesWritten = 0;
    deviceFramesRead = deviceFramesReadNow();
    startMediaTimeUsNeedsInit = false;
    startMediaTimeUsNeedsSync = false;
    currentPositionUs = CURRENT_POSITION_NOT_SET;
  } else if (startMediaTimeUsNeedsSync) {
    // A discontinuity without a reconfiguration: the frames written before this buffer sit after it
    // on the same timeline, so the position is anchored at the media time they start at, which is
    // what makes this buffer land at `presentationTimeUs`. ExoPlayer does the same in
    // `resetSyncParams`: `startMediaTimeUs = bufferTimeUs - framesToDurationUs(writtenFrames)`.
    // Without the anchor the frames written before the discontinuity count towards a position that
    // they were never rendered at.
    const auto framesToDurationUs = std::div(
        framesWritten.load() * Definitions::kMicrosPerSecond, outputConfig.sampleRate).quot;
    startPositionUs = std::max(static_cast<int64_t>(0), presentationTimeUs - framesToDurationUs);
    framesWritten = 0;
    deviceFramesRead = deviceFramesReadNow();
    startMediaTimeUsNeedsSync = false;
    currentPositionUs = CURRENT_POSITION_NOT_SET;
  }

  const auto bytesPerFrame = MediaUtils::getBytesPerFrame(inputConfig.sampleFormat, inputConfig.channelCount);
  const auto numFrames = byteBufferSize / bytesPerFrame;
  switch (inputConfig.sampleFormat) {
    case AudioSampleFormat::kFloat: {
      return mRingBuffer.write(static_cast<const float*>(buffer), numFrames);
    }
    case AudioSampleFormat::kInt16: {
      return mRingBuffer.writeInt16(static_cast<const int16_t*>(buffer), numFrames);
    }
    case AudioSampleFormat::kInt24: {
      return mRingBuffer.writeInt24(static_cast<const uint8_t*>(buffer), numFrames);
    }
    case AudioSampleFormat::kInt32: {
      return mRingBuffer.writeInt32(static_cast<const int32_t*>(buffer), numFrames);
    }
    default: {
      return false;
    }
  }
}

DataCallbackResult
AudioOutputSinkImpl::onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) {
  int32_t framesRead = 0;
  {
    // The playback thread changes the rate, the pitch and the volume, and flushes the stream, all
    // through mSonic. Sonic is not thread safe, so its use is serialized: without this, a rate
    // change that lands in the middle of a resample makes sonic read its rate and its sample
    // positions as two different values and walk off the end of its sinc table (SIGSEGV in
    // findSincCoefficient, on the audio callback thread).
    std::lock_guard<std::mutex> lock(mSonicLock);
    if (mSonic != nullptr) {
      // Read stored data into the buffer provided.
      while (sonicSamplesAvailable(mSonic) < numFrames) {
        if (outputConfig.sampleFormat == AudioSampleFormat::kInt16) {
          if (const auto framesGot = mRingBuffer.readInt16(mShortBuffer.data(), numFrames); framesGot > 0) {
            framesWritten += static_cast<int64_t>(framesGot);
            sonicWriteShortToStream(mSonic, mShortBuffer.data(), static_cast<int>(framesGot));
          } else {
            break;
          }
        } else {
          if (const auto framesGot = mRingBuffer.read(mFloatBuffer.data(), numFrames); framesGot > 0) {
            framesWritten += static_cast<int64_t>(framesGot);
            sonicWriteFloatToStream(mSonic, mFloatBuffer.data(), static_cast<int>(framesGot));
          } else {
            break;
          }
        }
      }
      if (outputConfig.sampleFormat == AudioSampleFormat::kInt16) {
        framesRead = sonicReadShortFromStream(mSonic, static_cast<short*>(audioData), static_cast<int>(numFrames));
      } else {
        framesRead = sonicReadFloatFromStream(mSonic, static_cast<float*>(audioData), static_cast<int>(numFrames));
      }
    }
  }
  // checks whether the cache buffer is too small
  if (framesRead == 0 && mRingBuffer.available() < numFrames) {
    if (mListener != nullptr) {
      mListener->onOffloadBufferEmptying();
    }
  }
  if (isEnded()) {
    return framesRead > 0 ? DataCallbackResult::Continue : DataCallbackResult::Stop;
  }
  return DataCallbackResult::Continue;
}

void AudioOutputSinkImpl::setRate(const float rate) {
  if (mRate != rate) {
    mRate = rate;
    std::lock_guard<std::mutex> lock(mSonicLock);
    if (mSonic != nullptr) {
      sonicSetRate(mSonic, mRate);
    }
  }
}

void AudioOutputSinkImpl::setPitch(const float pitch) {
  if (mPitch != pitch) {
    mPitch = pitch;
    std::lock_guard<std::mutex> lock(mSonicLock);
    if (mSonic != nullptr) {
      sonicSetPitch(mSonic, mPitch);
    }
  }
}

void AudioOutputSinkImpl::setVolume(const float volume) {
  if (mVolume != volume) {
    mVolume = volume;
    std::lock_guard<std::mutex> lock(mSonicLock);
    if (mSonic != nullptr) {
      sonicSetVolume(mSonic, mVolume);
    }
  }
}

void AudioOutputSinkImpl::pause() {
  if (mAudioStream) {
    mAudioStream->pause();
  }
}

void AudioOutputSinkImpl::flush() {
  mRingBuffer.reset();
  {
    // Serialized with the resampling in the data callback, which is using the same stream.
    std::lock_guard<std::mutex> lock(mSonicLock);
    if (mSonic) {
      sonicFlushStream(mSonic);
    }
  }
  // Re-arm the media time, as ExoPlayer's DefaultAudioSink does: the first buffer written after the
  // flush establishes the new start of the position timeline and restarts the frame count. Without
  // this the position keeps counting from the frames written before the flush, so after a seek the
  // sink keeps reporting the old position (and the video renderer, which is driven by this clock,
  // stops rendering the frames that now sit in the future).
  startMediaTimeUsNeedsInit = true;
  startMediaTimeUsNeedsSync = false;
  currentPositionUs = CURRENT_POSITION_NOT_SET;
  endOfStream = false;
  framesWritten = 0;
  // Discard what the device still holds: without this the audio that was queued before the seek
  // keeps playing, so the position reported by getCurrentPositionUs() (and the whole media clock
  // that is driven by it) restarts from the old position instead of the seek target.
  if (isAudioStreamInitialized()) {
    mAudioStream->flush();
  }
}

void AudioOutputSinkImpl::release() {
  if (mAudioStream) {
    // Closing waits for the data callback to return, so no callback is resampling any more once
    // this has returned.
    mAudioStream->close();
  }
  mAudioStream.reset();
  {
    std::lock_guard<std::mutex> lock(mSonicLock);
    if (mSonic) {
      sonicDestroyStream(mSonic);
      mSonic = nullptr;
    }
  }
  mRingBuffer.reset();
  opened = false;
  framesWritten = 0;
  startPositionUs = 0;
  currentPositionUs = CURRENT_POSITION_NOT_SET;
}

bool AudioOutputSinkImpl::hasCapacityForNextBuffer() {
  if (mAudioStream == nullptr) {
    return false;
  }
  return mRingBuffer.space() > kDefaultNumberFrames;
}

} // namespace coral

