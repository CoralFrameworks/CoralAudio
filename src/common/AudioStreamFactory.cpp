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

#include "private/audio/AudioStreamFactory.h"

#if defined(ANDROID) || defined(__ANDROID__)
#include "platform/android/AudioStreamAndroid.h"
#elif defined(APPLE) || defined(__APPLE__)
#include "platform/darwin/AudioStreamCoreAudio.h"
#elif defined(LINUX) || defined(__linux__)
#include "platform/linux/AudioStreamAlsa.h"
#elif defined(WIN32) || defined(_WIN32)
#include "platform/win32/AudioStreamWasapi.h"
#endif

namespace coral {
namespace {
class BuiltinStreamFactory final : public AudioStreamFactory {
public:
  std::shared_ptr<AudioStream> create(const AudioStreamBuilder& builder) override {
#if defined(ANDROID) || defined(__ANDROID__)
    return AudioStreamAndroid::Create(builder);
#elif defined(APPLE) || defined(__APPLE__)
    return AudioStreamCoreAudio::Create(builder);
#elif defined(LINUX) || defined(__linux__)
    return AudioStreamAlsa::Create(builder);
#elif defined(WIN32) || defined(_WIN32)
    return AudioStreamWasapi::Create(builder);
#endif
    return nullptr;
  }
};
} // namespace anonymous

std::unique_ptr<AudioStreamFactory> CreateBuiltinAudioStreamFactory() {
  return std::make_unique<BuiltinStreamFactory>();
}

} // namespace coral
