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

#include <coral/audio/AudioOutputSink.h>

#include "details/AudioOutputSinkImpl.h"

#include <media/Utilities.h>

namespace coral {

std::shared_ptr<AudioOutputSink> AudioOutputSink::CreateOutput() {
  return std::make_shared<AudioOutputSinkImpl>();
}

bool AudioOutputSink::supportsFormat(const std::shared_ptr<MediaFormat>& format) {
  return getFormatSupport(format) != FORMAT_UNSUPPORTED;
}

AudioOutputSink::FormatSupport
AudioOutputSink::getFormatSupport(const std::shared_ptr<MediaFormat>& format) {
  if (MimeTypes::AUDIO_RAW == format->getMimeType()
      || MimeTypes::AUDIO_AAC == format->getMimeType()) {
    if (!MediaUtils::isEncodingLinearPcm(format->getSampleFormat())) {
      return FORMAT_UNSUPPORTED;
    }
    if (format->getSampleFormat() == AudioSampleFormat::kInt16
        || (mEnableFloatOutput && format->getSampleFormat() == AudioSampleFormat::kFloat)) {
      return FORMAT_SUPPORTED_DIRECTLY;
    }
    // We can resample all linear PCM encodings to 16-bit integer PCM, which AudioTrack is guaranteed to support.
    return FORMAT_SUPPORTED_WITH_TRANSCODING;
  }
  //  if (mEnableOffload
  //      && !offloadDisabledUntilNextConfiguration
  //      && isOffloadedPlaybackSupported(format, audioAttributes)) {
  //    return FORMAT_SUPPORTED_DIRECTLY;
  //  }
  //  if (isPassthroughPlaybackSupported(format, audioCapabilities)) {
  //    return FORMAT_SUPPORTED_DIRECTLY;
  //  }
  return FORMAT_UNSUPPORTED;
}

} // namespace coral
