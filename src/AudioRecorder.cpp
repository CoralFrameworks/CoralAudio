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

#include "details/AudioRecorderImpl.h"

#include <core/time/SystemClock.h>

#include <minilog/log.h>
#include <utils/Path.h>

#include <algorithm>
#include <utility>

using namespace utils;

namespace coral {

std::shared_ptr<AudioRecorder>
AudioRecorder::Make(const std::string& url, const MediaFileType fileType, const AudioFormat& format) {
  return AudioRecorderImpl::Make(url, fileType, format);
}

AudioRecorder::AudioRecorder(std::string url, const MediaFileType fileType, AudioFormat format) noexcept
  : mStatus(Status::kIdle),
    mUrl(std::move(url)),
    mFileType(fileType),
    mAudioFormat(std::move(format)),
    mCurrentTimeUs(0),
    mMaxRecordDurationUs(Definitions::kTimeUnset) {

}

void AudioRecorder::setDelegate(std::unique_ptr<Delegate> delegate) {
  mDelegate = std::move(delegate);
}

bool AudioRecorder::prepareToRecord() {
  if (mUrl.empty()) {
    return false;
  }
  if (mStatus == Status::kPrepared) {
    return true;
  }
  onPrepare();
  return mStatus == Status::kPrepared;
}

void AudioRecorder::prepareAsync() {
  if (mStatus == Status::kPrepared) {
    return;
  }
  onPrepareAsync();
}

bool AudioRecorder::record() {
  if (mStatus != Status::kPrepared) {
    return false;
  }
  if (isRecording()) {
    return true;
  }
  onStartRecording(0, Definitions::kTimeUnset);
  return true;
}

bool AudioRecorder::recordAtTime(const int64_t timeUs) {
  if (mStatus != Status::kPrepared) {
    return false;
  }
  if (isRecording()) {
    return true;
  }
  onStartRecording(timeUs, Definitions::kTimeUnset);
  return true;
}

bool AudioRecorder::recordForDuration(const int64_t durationUs) {
  if (mStatus != Status::kPrepared) {
    MINILOG_ERROR("Failed to record: the recorder is not prepared!");
    return false;
  }
  if (isRecording()) {
    MINILOG_WARN("The AudioRecorder has been recording.");
    return true;
  }
  onStartRecording(0, durationUs);
  return true;
}

bool AudioRecorder::recordAtTimeForDuration(const int64_t timeUs, const int64_t durationUs) {
  if (mStatus != Status::kPrepared) {
    return false;
  }
  if (isRecording()) {
    return true;
  }
  onStartRecording(timeUs, durationUs);
  return true;
}

void AudioRecorder::pause() {
  if (mStatus == Status::kPaused) {
    return;
  }
  if (mStatus == Status::kRecording) {
    mStatus = Status::kPaused;
  }
}

void AudioRecorder::stop() {
  if (mStatus == Status::kStopped) {
    return;
  }
  onStop();
}

bool AudioRecorder::deleteRecording() {
  if (isRecording() || mStatus == Status::kIdle) {
    return false;
  }
  return deleteFile();
}

int64_t AudioRecorder::getDeviceCurrentTimeUs() {
  return SystemClock::CurrentTimeMicros();
}

bool AudioRecorder::deleteFile() {
  if (mStatus == Status::kRecording) {
    return false;
  }
  if (auto path = Path(mUrl); path.isFile()) {
    if (path.exists()) {
      return path.removeFile();
    }
    return true;
  }
  MINILOG_ERROR("Failed to delete file, the url({}) is not a file", mUrl);
  return false;
}

} // namespace coral
