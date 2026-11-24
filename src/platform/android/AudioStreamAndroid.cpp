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

#include "AudioStreamAndroid.h"

#include "aaudio/AudioStreamAAudio.h"
#include "opensles/AudioStreamInputOpenSLES.h"
#include "opensles/AudioStreamOutputOpenSLES.h"

namespace coral {

std::shared_ptr<AudioStream> AudioStreamAndroid::Create(const AudioStreamBuilder& builder) {
  if (AudioStreamAAudio::isSupported()) {
    return std::make_shared<AudioStreamAAudio>(builder);
  }
  if (builder.getDirection() == Direction::Render) {
    return std::make_shared<AudioStreamOutputOpenSLES>(builder);
  }
}

} // namespace coral
