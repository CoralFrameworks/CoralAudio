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

#include "aaudio/AAudioExtensions.h"
#include "aaudio/AudioStreamAAudio.h"
#include "opensles/AudioStreamInputOpenSLES.h"
#include "opensles/AudioStreamOutputOpenSLES.h"

#include "QuirksManager.h"
#include "Utilities.h"

#include <minilog/log.h>

namespace coral {

constexpr int knumBurstsForLowLatencyStreams = 2;

AudioStream* AudioStreamBuilder::build() const {
  AudioStream *stream = nullptr;
  if (isAAudioRecommended()) {
    stream = new AudioStreamAAudio(*this);
  } else {
    if (getDirection() == Direction::Render) {
      stream = new AudioStreamOutputOpenSLES(*this);
    } else if (getDirection() == Direction::Capture) {
      stream = new AudioStreamInputOpenSLES(*this);
    }
  }
  return stream;
}


Result AudioStreamBuilder::openStreamInternal(AudioStream **streamPP) {
  auto result = isValidConfig();
  if (result != Result::OK) {
    MINILOG_WARN("Invalid config. Error: {}", convertToText(result));
    return result;
  }

  if (streamPP == nullptr) {
    return Result::ErrorNull;
  }

  *streamPP = nullptr;

  AudioStream *streamP = nullptr;

  // Maybe make a FilterInputStream.
  AudioStreamBuilder childBuilder(*this);

  if (streamP == nullptr) {
    streamP = build();
    if (streamP == nullptr) {
      return Result::ErrorNull;
    }
  }

  // If MMAP has a problem in this case then disable it temporarily.
  bool wasMMapOriginallyEnabled = AAudioExtensions::getInstance().isMMapEnabled();
  bool wasMMapTemporarilyDisabled = false;
  if (wasMMapOriginallyEnabled) {
    bool isMMapSafe = QuirksManager::getInstance().isMMapSafe(childBuilder);
    if (!isMMapSafe) {
      AAudioExtensions::getInstance().setMMapEnabled(false);
      wasMMapTemporarilyDisabled = true;
    }
  }
  result = streamP->open();
  if (wasMMapTemporarilyDisabled) {
    AAudioExtensions::getInstance().setMMapEnabled(wasMMapOriginallyEnabled); // restore original
  }
  if (result == Result::OK) {
    // AAudio supports setBufferSizeInFrames() so use it.
    if (dynamic_cast<AudioStreamAAudio*>(streamP) != nullptr) {
      int32_t optimalBufferSize = -1;
      // Use a reasonable default buffer size.
      if (streamP->getDirection() == Direction::Capture) {
        // For input, small size does not improve latency because the stream is usually
        // run close to empty. And a low size can result in XRuns so always use the maximum.
        optimalBufferSize = streamP->getBufferCapacityInFrames();
      } else if (streamP->getPerformanceMode() == PerformanceMode::LowLatency
          && streamP->getDirection() == Direction::Render)  { // Render check is redundant.
        optimalBufferSize = streamP->getFramesPerBurst() * knumBurstsForLowLatencyStreams;
      }
      if (optimalBufferSize >= 0) {
        auto setBufferResult = streamP->setBufferSizeInFrames(optimalBufferSize);
        if (!setBufferResult) {
          MINILOG_WARN("Failed to setBufferSizeInFrames({}). Error was ｛｝",
              std::to_string(optimalBufferSize),
              convertToText(setBufferResult.error()));
        }
      }
    }
    *streamPP = streamP;
  } else {
    delete streamP;
  }
  return result;
}

} // namespace coral
