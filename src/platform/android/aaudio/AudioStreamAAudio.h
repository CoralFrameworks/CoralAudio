/*
 * Copyright 2016 The Android Open Source Project
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

#ifndef CORAL_AUDIO_ANDROID_STREAM_AAUDIO_H
#define CORAL_AUDIO_ANDROID_STREAM_AAUDIO_H

#include "AAudioLoader.h"
#include "platform/android/AudioStreamAndroid.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace coral {

/**
 * Implementation of AudioStream that uses AAudio.
 *
 * Do not create this class directly.
 * Use an AudioStream::Builder to create one.
 */
class AudioStreamAAudio : public AudioStreamAndroid {
public:
  explicit AudioStreamAAudio(const AudioStreamBuilder& builder) noexcept;

  ~AudioStreamAAudio() override;

  /**
   * Returns whether if AAudio is supported on this device.
   */
  static bool isSupported();

  // These functions override methods in AudioStream.
  // See AudioStream for documentation.
  Result open() override;

  Result release() override;

  Result close() override;

  Result requestStart() override;

  Result requestPause() override;

  Result requestFlush() override;

  Result requestStop() override;

  OpResult<uint32_t> write(const void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> read(void* buffer, uint32_t numFrames, int64_t timeoutNanoseconds) override;

  OpResult<uint32_t> setBufferSizeInFrames(uint32_t requestedFrames) override;

  uint32_t getBufferSizeInFrames() override;

  OpResult<uint32_t> getXRunCount() override;

  bool isXRunCountSupported() const override { return true; }

  OpResult<double> calculateLatencyMillis() override;

  Result waitForStateChange(StreamState currentState, StreamState* nextState, int64_t timeoutNanoseconds) override;

  Result getTimestamp(ClockMode mode, int64_t* framePosition, int64_t* timeNanoseconds) override;

  StreamState getState() override;

  DataCallbackResult callOnAudioReady(AAudioStream* stream, void* audioData, int32_t numFrames);

  bool isMMapUsed();

  AudioApi getAudioApi() const override {
    return AudioApi::AAudio;
  }

protected:
  static void internalErrorCallback(AAudioStream* stream, void* userData, aaudio_result_t error);

  void* getUnderlyingStream() const override {
    return mAAudioStream.load();
  }

  void updateFramesRead() override;

  void updateFramesWritten() override;

  void logUnsupportedAttributes();

private:
  // Must call under mLock. And stream must NOT be nullptr.
  Result requestStop_l(AAudioStream* stream);

  /**
   * Launch a thread that will stop the stream.
   */
  void launchStopThread();

private:
  static const char* getName() { return "AudioStreamAAudio"; }

  std::atomic<bool> mCallbackThreadEnabled;
  std::atomic<bool> mStopThreadAllowed{ false };

  // pointer to the underlying 'C' AAudio stream, valid if open, null if closed
  std::atomic<AAudioStream*> mAAudioStream{ nullptr };
  std::shared_mutex mAAudioStreamLock; // to protect mAAudioStream while closing

  static AAudioLoader* mLibLoader;
};

} // namespace coral

#endif // CORAL_AUDIO_ANDROID_STREAM_AAUDIO_H
