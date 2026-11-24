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

#include "private/audio/AudioStreamBuilder.h"

#include "private/audio/AudioStream.h"

#include <minilog/log.h>

namespace coral {

AudioStreamBuilder&
AudioStreamBuilder::setDirection(const Direction direction) {
  mDirection = direction;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setBufferCapacityInFrames(const uint32_t bufferCapacityInFrames) {
  mBufferCapacityInFrames = bufferCapacityInFrames;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setChannelCount(const AudioChannelCount channelCount) {
  mChannelCount = channelCount;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setSampleRate(const uint64_t sampleRate) {
  mSampleRate = sampleRate;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setDataCallback(const std::shared_ptr<AudioStreamDataCallback>& callback) {
  mDataCallback = callback;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setErrorCallback(const std::shared_ptr<AudioStreamErrorCallback>& callback) {
  mErrorCallback = callback;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setFormat(const AudioSampleFormat format) {
  mFormat = format;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setFramesPerDataCallback(const uint32_t framesPerCallback) {
  mFramesPerCallback = framesPerCallback;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setPerformanceMode(const PerformanceMode mode) {
  mPerformanceMode = mode;
  return *this;
}

AudioStreamBuilder&
AudioStreamBuilder::setSharingMode(const SharingMode mode) {
  mSharingMode = mode;
  return *this;
}

Result AudioStreamBuilder::openStream(std::shared_ptr<AudioStream>& sharedStream) {
  sharedStream.reset();
  AudioStream *streamptr;
  auto result = openStreamInternal(&streamptr);
  if (result == Result::OK) {
    sharedStream.reset(streamptr);
    // Save a weak_ptr in the stream for use with callbacks.
    streamptr->setWeakThis(sharedStream);
  }
  return result;
}

} // namespace coral
