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

#ifndef CORAL_AUDIO_ANDROID_OUTPUT_MIXER_OPEN_SLES_H
#define CORAL_AUDIO_ANDROID_OUTPUT_MIXER_OPEN_SLES_H

#include <atomic>
#include <mutex>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

namespace coral {

/**
 * Internal use only
 */
class OutputMixerOpenSLES {
public:
  static OutputMixerOpenSLES& getInstance();

  SLresult open();

  void close();

  SLresult createAudioPlayer(SLObjectItf* objectItf, SLDataSource* audioSource);

private:
  OutputMixerOpenSLES() = default;

  ~OutputMixerOpenSLES() = default;

  OutputMixerOpenSLES(const OutputMixerOpenSLES&) = delete;

  OutputMixerOpenSLES& operator=(const OutputMixerOpenSLES&) = delete;

  std::mutex mLock;
  int32_t mOpenCount = 0;

  SLObjectItf mOutputMixObject = nullptr;
};

} // namespace coral

#endif // CORAL_AUDIO_ANDROID_OUTPUT_MIXER_OPEN_SLES_H
