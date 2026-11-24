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

#ifndef CORAL_AUDIO_ANDROID_AUDIO_STREAM_INPUT_OPEN_SLES_H
#define CORAL_AUDIO_ANDROID_AUDIO_STREAM_INPUT_OPEN_SLES_H

#include "AudioStreamOpenSLES.h"

namespace coral {

/**
 * Internal use only
 */
class AudioStreamInputOpenSLES final : public AudioStreamOpenSLES {
public:
  explicit AudioStreamInputOpenSLES(const AudioStreamBuilder& builder) noexcept;

  ~AudioStreamInputOpenSLES() override = default;

  Result open() override;

  Result close() override;

  Result requestStart() override;

  Result requestPause() override;

  Result requestFlush() override;

  Result requestStop() override;

  [[nodiscard]] std::string getName() const override {
    return "AudioStreamInputOpenSLES";
  }

protected:
  Result requestStopLocked();

  Result updateServiceFrameCounter() override;

  void updateFramesWritten() override;

private:
  SLuint32 channelCountToChannelMask(int channelCount) const;

  Result setRecordStateLocked(SLuint32 newState);

  SLRecordItf mRecordInterface = nullptr;
};

} // namespace coral

#endif // CORAL_AUDIO_ANDROID_AUDIO_STREAM_INPUT_OPEN_SLES_H
