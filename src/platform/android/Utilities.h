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

#ifndef CORAL_AUDIO_ANDROID_UTILITIES_H
#define CORAL_AUDIO_ANDROID_UTILITIES_H

#include <cstdint>
#include <string>

namespace coral {

/**
 * @param name
 * @return the value of a named system property in a string or empty string
 */
std::string getPropertyString(const char * name);

/**
 * @param name
 * @param defaultValue
 * @return integer value associated with a property or the default value
 */
int getPropertyInteger(const char * name, int defaultValue);

/**
 * Return the version of the SDK that is currently running.
 *
 * For example, on Android, this would return 27 for Oreo 8.1.
 * If the version number cannot be determined then this will return -1.
 *
 * @return version number or -1
 */
int getSdkVersion();

/**
 * Returns whether a device is on a pre-release SDK that is at least the specified codename
 * version.
 *
 * @param codename the code name to verify.
 * @return boolean of whether the device is on a pre-release SDK and is at least the specified
 * codename
 */
bool isAtLeastPreReleaseCodename(const std::string& codename);

/**
 * Indicates whether supported AAudio library.
 */
bool isAAudioSupported();

/**
 * Indicates whether recommended using AAudio.
 */
bool isAAudioRecommended();

} // namespace coral

#endif //CORAL_AUDIO_ANDROID_UTILITIES_H
