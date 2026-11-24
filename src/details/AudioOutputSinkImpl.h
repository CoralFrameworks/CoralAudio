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

#ifndef CORAL_AUDIO_IO_AUDIO_OUTPUT_SINK_IMPL_H
#define CORAL_AUDIO_IO_AUDIO_OUTPUT_SINK_IMPL_H

#include <coral/audio/AudioOutputSink.h>
#include <coral/audio/Resample.h>

#include "private/audio/AudioStream.h"
#include "private/common/AudioRingBuffer.h"

#include <sonic.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace coral {

/**
 * The implementation of the AudioOutputSink that consumes audio data
 */
class AudioOutputSinkImpl final : public AudioOutputSink,
                                  public AudioStreamDataCallback,
                                  public std::enable_shared_from_this<AudioOutputSinkImpl> {
public:
  AudioOutputSinkImpl();

  ~AudioOutputSinkImpl() override;

  void setListener(const std::shared_ptr<Listener>& listener) override;

  int64_t getCurrentPositionUs(bool sourceEnded) override;

  void configure(const AudioFormat& format) override;

  void play() override;

  void handleDiscontinuity() override;

  bool handleBuffer(const void* buffer, size_t byteBufferSize, int64_t presentationTimeUs) override;

  void playToEndOfStream() override {
    endOfStream = true;
  }

  bool isEnded() override {
    return !opened || (endOfStream && !hasPendingData());
  }

  bool hasPendingData() override {
    return mRingBuffer.available() > 0;
  }

  bool hasCapacityForNextBuffer() override;

  void setRate(float rate) override;

  float getRate() const override {
    return mRate;
  }

  void setPitch(float pitch) override;

  float getPitch() const override {
    return mPitch;
  }

  void setVolume(float volume) override;

  float getVolume() const {
    return mVolume;
  }

  void pause() override;

  void flush() override;

  void release() override;

  DataCallbackResult onAudioReady(AudioStream* stream, void* audioData, AudioFrameCount numFrames) override;

private:
  bool isAudioStreamInitialized() const {
    return mAudioStream != nullptr;
  }

  void initializeAudioStream();

  /**
   * Returns the device's played-frames counter, or -1 when there is no stream to ask.
   */
  int64_t deviceFramesReadNow();

  static std::string getName() {
    return "AudioOutputSink";
  }

  static constexpr int kDefaultBurstsPerBuffer = 16;
  static constexpr AudioFrameCount kDefaultCacheDuration = 1;
  static constexpr AudioFrameCount kDefaultNumberFrames = 1152;

  std::shared_ptr<Listener> mListener;
  std::shared_ptr<AudioStream> mAudioStream;
  sonicStream mSonic;
  /**
   * Serializes the access to {@link #mSonic}, which is not thread safe: the data callback resamples
   * with it on the audio callback thread while the playback thread changes the playback parameters
   * (or flushes) with it. The callback thread is not the only writer: without this, a rate change
   * that lands in the middle of a resample makes sonic read its rate and its sample positions as
   * two different values and walk off the end of its sinc table (SIGSEGV in findSincCoefficient).
   */
  std::mutex mSonicLock;
  std::vector<short> mShortBuffer;
  std::vector<float> mFloatBuffer;
  AudioRingBuffer mRingBuffer;
  AudioConfiguration inputConfig;
  AudioConfiguration outputConfig;
  std::atomic<int64_t> framesWritten;
  std::atomic_bool opened;
  bool endOfStream;
  std::atomic<float> mPitch;
  std::atomic<float> mRate;
  std::atomic<float> mVolume;
  std::atomic_bool startMediaTimeUsNeedsSync;
  std::atomic_bool startMediaTimeUsNeedsInit;
  /**
   * Set once the renderer has asked the sink to play. The stream is opened later, on the first
   * buffer, so the request is remembered and applied when the stream is ready.
   */
  std::atomic_bool playRequested{ false };
  int64_t startPositionUs;

  /**
   * The device's played-frames counter when {@link #startPositionUs} was anchored, or -1 when no
   * anchor has been taken yet. The counter measures the whole stream and does not restart when the
   * client is reset, so the position advances by the difference, not by the counter itself.
   */
  int64_t deviceFramesRead;
  int64_t currentPositionUs;
};

} // namespace coral

#endif // CORAL_AUDIO_IO_AUDIO_OUTPUT_SINK_IMPL_H
