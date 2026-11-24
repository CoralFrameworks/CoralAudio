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

#ifndef CORAL_PRIVATE_AUDIO_STREAM_CALLBACK_H
#define CORAL_PRIVATE_AUDIO_STREAM_CALLBACK_H

#include "private/audio/AudioStreamTypes.h"

#include <coral/audio/AudioTypes.h>

namespace coral {

class AudioStream;

/**
 * AudioStreamDataCallback defines a callback interface for moving data to/from a stream using `onAudioReady`
 * 2) being alerted when a stream has an error using `onError*` methods
 */
class AudioStreamDataCallback {
public:
  virtual ~AudioStreamDataCallback() = default;

  /**
   * A buffer is ready for processing.
   *
   * @param stream     pointer to the associated stream
   * @param audioData  buffer containing input data or a place to put output data
   * @param numFrames  number of frames to be processed
   * @return DataCallbackResult::Continue or DataCallbackResult::Stop
   */
  virtual DataCallbackResult onAudioReady(AudioStream* stream, void* audioData, AudioFrameCount numFrames) = 0;
};

/**
 * ErrorCallback defines a callback interface for being alerted when a stream has an error or
 * is disconnected sing `onError*` methods.
 *
 * Note: This callback is only fired when an AudioStreamCallback is set.
 * If you use AudioStream::write() you have to evaluate the return codes of
 * AudioStream::write() to notice errors in the stream.
 *
 * It is used with AudioStreamBuilder::setErrorCallback().
 */
class AudioStreamErrorCallback {
public:
  virtual ~AudioStreamErrorCallback() = default;

  /**
   * This will be called before other `onError` methods when an error occurs on
   * a stream, such as when the stream is disconnected.
   *
   * @return true if the stream has been stopped and closed, false if not
   */
  virtual bool onError(AudioStream* /* stream */, Result /* error */) {
    return false;
  }

  /**
   * This will be called when an error occurs on a stream, such as when the stream is disconnected,
   * and if onError() returns false (indicating that the error has not already been handled).
   */
  virtual void onErrorBeforeClose(AudioStream* /* stream */, Result /* error */) {
  }

  /**
   * This will be called when an error occurs on a stream, such as when the stream is disconnected,
   * and if onError() returns false (indicating that the error has not already been handled).
   */
  virtual void onErrorAfterClose(AudioStream* /* stream */, Result /* error */) {
  }
};

} // namespace coral

#endif //CORAL_PRIVATE_AUDIO_STREAM_CALLBACK_H
