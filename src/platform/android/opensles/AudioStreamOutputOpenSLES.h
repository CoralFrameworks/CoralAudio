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

#ifndef CORAL_AUDIO_ANDROID_AUDIO_STREAM_OUTPUT_OPEN_SLES_H
#define CORAL_AUDIO_ANDROID_AUDIO_STREAM_OUTPUT_OPEN_SLES_H

#include "AudioStreamOpenSLES.h"

namespace coral {

/**
 * Internal use only
 */
class AudioStreamOutputOpenSLES final : public AudioStreamOpenSLES {
public:
  explicit AudioStreamOutputOpenSLES(const AudioStreamBuilder& builder) noexcept;

  ~AudioStreamOutputOpenSLES() override = default;

  Result open() override;

  Result close() override;

  Result requestStart() override;

  Result requestPause() override;

  Result requestFlush() override;

  Result requestStop() override;

  std::string getName() const override {
    return "AudioStreamOutputOpenSLES";
  }

protected:
  Result requestPauseLocked();

  void setFramesRead(int64_t framesRead);

  Result updateServiceFrameCounter() override;

  void updateFramesRead() override;

private:
  SLuint32 channelCountToChannelMask(int channelCount) const;

  Result onAfterDestroy() override;

  Result requestFlushLocked();

  Result requestStopLocked();

  /**
   * Set OpenSL ES PLAYSTATE.
   *
   * @param newState SL_PLAYSTATE_PAUSED, SL_PLAYSTATE_PLAYING,
   * SL_PLAYSTATE_STOPPED
   * @return
   */
  Result setPlayStateLocked(SLuint32 newState);

  SLPlayItf mPlayInterface = nullptr;
};

} // namespace coral

#endif // CORAL_AUDIO_ANDROID_AUDIO_STREAM_OUTPUT_OPEN_SLES_H
