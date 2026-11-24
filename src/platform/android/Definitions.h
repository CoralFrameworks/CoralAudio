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

#ifndef CORAL_AUDIO_ANDROID_DEFINITIONS_H
#define CORAL_AUDIO_ANDROID_DEFINITIONS_H

#include "private/audio/AudioStreamTypes.h"

namespace coral {

/**
 * The underlying audio API used by the audio stream.
 */
enum class AudioApi : uint8_t {
  /**
   * Try to use AAudio. If not available then use OpenSL ES.
   */
  Unspecified = 0,

  /**
   * Use OpenSL ES.
   * Note that OpenSL ES is deprecated in Android 13, API 30 and above.
   */
  OpenSLES,

  /**
   * Try to use AAudio. Fail if unavailable.
   * AAudio was first supported in Android 8, API 26 and above.
   * It is only recommended for API 27 and above.
   */
  AAudio
};

/**
 * The Usage attribute expresses *why* you are playing a sound, what is this
 * sound used for. This information is used by certain platforms or routing
 * policies to make more refined volume or routing decisions.
 *
 * Note that these match the equivalent values in AudioAttributes in the Android
 * Java API.
 *
 * For Android platform, this attribute only has an effect on API 28+.
 */
enum class Usage : int32_t {
  /**
   * Use this for streaming media, music performance, video, podcasts, etcetera.
   */
  Media = 1, // AUDIO_USAGE_MEDIA

  /**
   * Use this for voice over IP, telephony, etcetera.
   */
  VoiceCommunication = 2, // AUDIO_USAGE_VOICE_COMMUNICATION

  /**
   * Use this for sounds associated with telephony such as busy tones, DTMF,
   * etcetera.
   */
  VoiceCommunicationSignalling =
  3, // AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING

  /**
   * Use this to demand the users attention.
   */
  Alarm = 4, // AUDIO_USAGE_ALARM

  /**
   * Use this for notifying the user when a message has arrived or some
   * other background event has occured.
   */
  Notification = 5, // AUDIO_USAGE_NOTIFICATION

  /**
   * Use this when the phone rings.
   */
  NotificationRingtone = 6, // AUDIO_USAGE_NOTIFICATION_RINGTONE

  /**
   * Use this to attract the users attention when, for example, the battery is
   * low.
   */
  NotificationEvent = 10, // AUDIO_USAGE_NOTIFICATION_EVENT

  /**
   * Use this for screen readers, etcetera.
   */
  AssistanceAccessibility = 11, // AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY

  /**
   * Use this for driving or navigation directions.
   */
  AssistanceNavigationGuidance =
  12, // AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE

  /**
   * Use this for user interface sounds, beeps, etcetera.
   */
  AssistanceSonification = 13, // AUDIO_USAGE_ASSISTANCE_SONIFICATION

  /**
   * Use this for game audio and sound effects.
   */
  Game = 14, // AUDIO_USAGE_GAME

  /**
   * Use this for audio responses to user queries, audio instructions or help
   * utterances.
   */
  Assistant = 16, // AUDIO_USAGE_ASSISTANT
};

/**
 * This attribute can be used to allocate a session ID to the audio stream.
 *
 * This attribute only has an effect on Android API 28+.
 */
enum SessionId {
  /**
   * Do not allocate a session ID.
   * Effects cannot be used with this stream.
   * Default.
   */
  None = -1, // AUDIO_SESSION_ID_NONE

  /**
   * Allocate a session ID that can be used to attach and control
   * effects using the Java AudioEffects API.
   * Note that the use of this flag may result in higher latency.
   *
   * Note that this matches the value of AudioManager.AUDIO_SESSION_ID_GENERATE.
   */
  Allocate = 0, // AUDIO_SESSION_ID_ALLOCATE
};

/**
 * The channel mask of the audio stream. The underlying type is `uint32_t`.
 * Use of this enum is convenient.
 *
 * ChannelMask::Unspecified means this is not specified.
 * The rest of the enums are channel position masks.
 * Use the combinations of the channel position masks defined below instead of
 * using those values directly.
 */
enum class ChannelMask : uint32_t { // aaudio_channel_mask_t
  Unspecified = 0,
  FrontLeft = 1 << 0,
  FrontRight = 1 << 1,
  FrontCenter = 1 << 2,
  LowFrequency = 1 << 3,
  BackLeft = 1 << 4,
  BackRight = 1 << 5,
  FrontLeftOfCenter = 1 << 6,
  FrontRightOfCenter = 1 << 7,
  BackCenter = 1 << 8,
  SideLeft = 1 << 9,
  SideRight = 1 << 10,
  TopCenter = 1 << 11,
  TopFrontLeft = 1 << 12,
  TopFrontCenter = 1 << 13,
  TopFrontRight = 1 << 14,
  TopBackLeft = 1 << 15,
  TopBackCenter = 1 << 16,
  TopBackRight = 1 << 17,
  TopSideLeft = 1 << 18,
  TopSideRight = 1 << 19,
  BottomFrontLeft = 1 << 20,
  BottomFrontCenter = 1 << 21,
  BottomFrontRight = 1 << 22,
  LowFrequency2 = 1 << 23,
  FrontWideLeft = 1 << 24,
  FrontWideRight = 1 << 25,

  Mono = FrontLeft,

  Stereo = FrontLeft | FrontRight,

  CM2Point1 = FrontLeft | FrontRight | LowFrequency,

  Tri = FrontLeft | FrontRight | FrontCenter,

  TriBack = FrontLeft | FrontRight | BackCenter,

  CM3Point1 = FrontLeft | FrontRight | FrontCenter | LowFrequency,

  CM2Point0Point2 = FrontLeft | FrontRight | TopSideLeft | TopSideRight,

  CM2Point1Point2 = CM2Point0Point2 | LowFrequency,

  CM3Point0Point2 = FrontLeft | FrontRight | FrontCenter | TopSideLeft | TopSideRight,

  CM3Point1Point2 = CM3Point0Point2 | LowFrequency,

  Quad = FrontLeft | FrontRight | BackLeft | BackRight,

  QuadSide = FrontLeft | FrontRight | SideLeft | SideRight,

  Surround = FrontLeft | FrontRight | FrontCenter | BackCenter,

  Penta = Quad | FrontCenter,

  // aka 5Point1Back
  CM5Point1 = FrontLeft | FrontRight | FrontCenter | LowFrequency | BackLeft | BackRight,

  CM5Point1Side = FrontLeft | FrontRight | FrontCenter | LowFrequency | SideLeft | SideRight,

  CM6Point1 = FrontLeft | FrontRight | FrontCenter | LowFrequency | BackLeft | BackRight | BackCenter,

  CM7Point1 = CM5Point1 | SideLeft | SideRight,

  CM5Point1Point2 = CM5Point1 | TopSideLeft | TopSideRight,

  CM5Point1Point4 = CM5Point1 | TopFrontLeft | TopFrontRight | TopBackLeft | TopBackRight,

  CM7Point1Point2 = CM7Point1 | TopSideLeft | TopSideRight,

  CM7Point1Point4 = CM7Point1 | TopFrontLeft | TopFrontRight | TopBackLeft | TopBackRight,

  CM9Point1Point4 = CM7Point1Point4 | FrontWideLeft | FrontWideRight,

  CM9Point1Point6 = CM9Point1Point4 | TopSideLeft | TopSideRight,

  FrontBack = FrontCenter | BackCenter,
};

/**
 * The PrivacySensitiveMode attribute determines whether an input stream can be
 * shared with another privileged app, for example the Assistant.
 *
 * This allows to override the default behavior tied to the audio source (e.g
 * InputPreset::VoiceCommunication is private by default but
 * InputPreset::Unprocessed is not).
 *
 * Only available in Android.
 */
enum class PrivacySensitiveMode : int32_t {

  /**
   * When not explicitly requested, set privacy sensitive mode according to
   * input preset: communication and camcorder captures are considered privacy
   * sensitive by default.
   */
  Unspecified = 0,

  /**
   * Privacy sensitive mode disabled.
   */
  Disabled = 1,

  /**
   * Privacy sensitive mode enabled.
   */
  Enabled = 2,
};

} // namespace coral

#endif //CORAL_AUDIO_ANDROID_DEFINITIONS_H
