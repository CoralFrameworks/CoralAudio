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

#ifndef CORAL_AUDIO_ANDROID_OPEN_SLES_UTILITIES_H
#define CORAL_AUDIO_ANDROID_OPEN_SLES_UTILITIES_H

#include <SLES/OpenSLES_Android.h>

#include <coral/audio/AudioTypes.h>

namespace coral::OpenSLES {

const char* SLErrToStr(SLresult code);

/**
 * Creates an extended PCM format from the supplied format and data
 * representation. This method should only be called on Android devices with API
 * level 21+. API 21 introduced the SLAndroidDataFormat_PCM_EX object which
 * allows audio samples to be represented using single precision floating-point.
 *
 * @param format
 * @param representation
 * @return the extended PCM format
 */
SLAndroidDataFormat_PCM_EX
CreateExtendedFormat(SLDataFormat_PCM format, SLuint32 representation);

SLuint32 ConvertFormatToRepresentation(AudioSampleFormat sampleFormat);

} // namespace coral::OpenSLES

#endif // CORAL_AUDIO_ANDROID_OPEN_SLES_UTILITIES_H
