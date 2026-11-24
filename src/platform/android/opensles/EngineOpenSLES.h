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

#ifndef CORAL_AUDIO_ANDROID_ENGINE_OPEN_SLES_H
#define CORAL_AUDIO_ANDROID_ENGINE_OPEN_SLES_H

#include <core/task/Mutex.h>

#include <atomic>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

namespace coral {

/**
 * Internal use only
 */
class EngineOpenSLES {
public:
  static EngineOpenSLES& getInstance();

  SLresult open();

  void close();

  SLresult createOutputMix(SLObjectItf* objectItf);

  SLresult createAudioPlayer(SLObjectItf* objectItf, SLDataSource* source, SLDataSink* sink);

  SLresult createAudioRecorder(SLObjectItf* objectItf, SLDataSource* source, SLDataSink* sink);

private:
  EngineOpenSLES() = default;

  virtual ~EngineOpenSLES() = default;

  EngineOpenSLES(const EngineOpenSLES&) = delete;

  EngineOpenSLES& operator=(const EngineOpenSLES&) = delete;

  Mutex mLock;
  int32_t mOpenCount = 0;

  SLObjectItf mEngineObject = nullptr;
  SLEngineItf mEngineInterface = nullptr;
};

} // namespace coral

#endif // CORAL_AUDIO_ANDROID_ENGINE_OPEN_SLES_H
