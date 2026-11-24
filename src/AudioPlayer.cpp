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

#include "details/AudioPlayerImpl.h"

#include <core/time/SystemClock.h>

#include <utils/Path.h>

using namespace utils;

namespace coral {

std::shared_ptr<AudioPlayer> AudioPlayer::Create(const std::string& url) {
  if (Path(url).exists()) {
    return AudioPlayerImpl::Make(url);
  }
  return {};
}

AudioPlayer::AudioPlayer(std::string url) noexcept
  : mStatus(Status::kIdle),
    mUrl(std::move(url)),
    mNumberOfChannels(0),
    mVolume(1.0f),
    mCurrentTimeUs(0),
    mDurationUs(Definitions::kInvalidValue),
    mNumberOfLoops(0),
    mEnableRate(false),
    mPlaybackRate(1.0f) {

}

void AudioPlayer::setDelegate(std::unique_ptr<Delegate> delegate) {
  if (delegate != nullptr) {
    mDelegate = std::move(delegate);
  } else {
    mDelegate.reset();
  }
}

bool AudioPlayer::prepareToPlay() {
  if (mStatus == Status::kPrepared) {
    return true;
  }

  // if the url is not available, failed to prepare.
  if (mUrl.empty()) {
    return false;
  }

  return onPrepare();
}

bool AudioPlayer::play() {
  return onPlayDelay(0);
}

bool AudioPlayer::playAtTime(const int64_t timeUs) {
  const auto delayMs = (timeUs / Definitions::kMicrosPerMillisecond) - SystemClock::CurrentTimeMillis();
  return onPlayDelay(delayMs);
}

void AudioPlayer::pause() {
  if (mStatus == Status::kPaused) {
    return;
  }
  if (mStatus == Status::kPlaying) {
    mStatus = Status::kPaused;
    onPause();
  }
}

void AudioPlayer::stop() {
  if (mStatus == Status::kStopped) {
    return;
  }
  mStatus = Status::kStopped;
  onStop();
}

void AudioPlayer::seekTo(const int64_t timeUs) {
  if (mCurrentTimeUs == timeUs) {
    return;
  }
  onSeekUpdate(timeUs);
}

void AudioPlayer::setVolume(const float volume) {
  if (getVolume() != volume) {
    mVolume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
    onVolumeUpdate();
  }
}

void AudioPlayer::setEnableRate(const bool enableRate) {
  mEnableRate = enableRate;
}

void AudioPlayer::setRate(const float rate) {
  if (mPlaybackRate != rate) {
    mPlaybackRate = rate;
    onPlaybackRateUpdate();
  }
}

void AudioPlayer::setNumberOfLoops(const int loopCount) {
  mNumberOfLoops = loopCount;
}

} // namespace coral
