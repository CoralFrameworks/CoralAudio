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

#ifndef CORAL_AUDIO_ANDROID_AUDIO_STREAM_ANDROID_H
#define CORAL_AUDIO_ANDROID_AUDIO_STREAM_ANDROID_H

#include "private/audio/AudioStream.h"
#include "private/audio/AudioStreamBuilder.h"

#include "Definitions.h"

#include <memory>

namespace coral {

class AudioStreamAndroid : public AudioStream {
public:
  static std::shared_ptr<AudioStream> Create(const AudioStreamBuilder &builder);

  virtual AudioApi getAudioApi() const = 0;

protected:
  explicit AudioStreamAndroid(const AudioStreamBuilder &builder) noexcept
      : AudioStream(builder) {
  }

  //!< Stream channel mask. Only active on Android 32+
  ChannelMask mChannelMask = ChannelMask::Unspecified;

  //!< Stream audio device ID
  int32_t mDeviceId = kUnspecified;

  //!< Privacy Sensitive Mode. Only active on Android 30+
  PrivacySensitiveMode mPrivacySensitiveMode = PrivacySensitiveMode::Unspecified;

  //!< Stream session ID allocation strategy. For android, Only active on API 28+
  SessionId mSessionId = SessionId::None;

  //!< Stream usage. For Android, only active on API 28+
  Usage mUsage = Usage::Media;

  //!< Control the name of the package creating the stream. Only active on Android 31+
  std::string mPackageName;
  //!< Control the attribution tag of the context creating the stream. Only active on Android 31+
  std::string mAttributionTag;

  //!< Hardware channel count. Only specified on Android 34+ AAudio streams
  int32_t mHardwareChannelCount = kUnspecified;
  //!< Hardware sample rate. Only specified on Android 34+ AAudio streams
  int32_t mHardwareSampleRate = kUnspecified;
  //!< Hardware format. Only specified on Android 34+ AAudio streams
  AudioSampleFormat mHardwareFormat = AudioSampleFormat::kDefault;
};

} // namespace coral

#endif //CORAL_AUDIO_ANDROID_AUDIO_STREAM_ANDROID_H
