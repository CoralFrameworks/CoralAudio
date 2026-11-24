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

#include "Utilities.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sstream>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "aaudio/AudioStreamAAudio.h"
#endif

namespace coral {

std::string getPropertyString(const char * name) {
  std::string result;
#ifdef __ANDROID__
  char valueText[PROP_VALUE_MAX] = {0};
  if (__system_property_get(name, valueText) != 0) {
    result = valueText;
  }
#else
  (void) name;
#endif
  return result;
}

int getPropertyInteger(const char * name, int defaultValue) {
  int result = defaultValue;
#ifdef __ANDROID__
  char valueText[PROP_VALUE_MAX] = {0};
  if (__system_property_get(name, valueText) != 0) {
    result = atoi(valueText);
  }
#else
  (void) name;
#endif
  return result;
}

int getSdkVersion() {
  static int sCachedSdkVersion = -1;
#ifdef __ANDROID__
  if (sCachedSdkVersion == -1) {
    sCachedSdkVersion = getPropertyInteger("ro.build.version.sdk", -1);
  }
#endif
  return sCachedSdkVersion;
}

bool isAtLeastPreReleaseCodename(const std::string& codename) {
  std::string buildCodename = getPropertyString("ro.build.version.codename");
  // Special case "REL", which means the build is not a pre-release build.
  if ("REL" == buildCodename) {
    return false;
  }

  // Otherwise lexically compare them. Return true if the build codename is equal to or
  // greater than the requested codename.
  return buildCodename.compare(codename) >= 0;
}

bool isAAudioSupported() {
#if defined(CORAL_SUPPORTS_AAUDIO)
  return AudioStreamAAudio::isSupported();
#endif
  return false;
}

bool isAAudioRecommended() {
  // AAudio may not be stable on Android O, depending on how it is used.
  // To be safe, use AAudio only on O_MR1 and above.
  return (getSdkVersion() >= __ANDROID_API_O_MR1__) && isAAudioSupported();
}

} // namespace coral
