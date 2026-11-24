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

#ifndef CORAL_PRIVATE_AUDIO_DEFINITIONS_H
#define CORAL_PRIVATE_AUDIO_DEFINITIONS_H

#include <cstdint>

namespace coral {

/**
 * The direction of the AudioStream.
 */
enum class Direction : uint8_t {
  //!< Used for playback to system
  Render = 0,
  //!< Used for recording from system
  Capture = 1,
};

enum class DataCallbackResult : uint8_t {
  //!< Indicates to the caller that the callbacks should continue.
  Continue = 0,
  //!< Indicates to the caller that the callbacks should stop immediately.
  Stop = 1,
};

/**
 * The state of the AudioStream
 */
enum class StreamState : uint32_t {
  // clang-format off
  Uninitialized = 0, // STREAM_STATE_UNINITIALIZED,
  Unknown = 1,       // STREAM_STATE_UNKNOWN,
  Open = 2,          // STREAM_STATE_OPEN,
  Starting = 3,      // STREAM_STATE_STARTING,
  Started = 4,       // STREAM_STATE_STARTED,
  Pausing = 5,       // STREAM_STATE_PAUSING,
  Paused = 6,        // STREAM_STATE_PAUSED,
  Flushing = 7,      // STREAM_STATE_FLUSHING,
  Flushed = 8,       // STREAM_STATE_FLUSHED,
  Stopping = 9,      // STREAM_STATE_STOPPING,
  Stopped = 10,      // STREAM_STATE_STOPPED,
  Closing = 11,      // STREAM_STATE_CLOSING,
  Closed = 12,       // STREAM_STATE_CLOSED,
  Disconnected = 13, // STREAM_STATE_DISCONNECTED,
  // clang-format on
};

enum class SharingMode : uint8_t {
  /**
   * This will be the only stream using a particular source or sink.
   * This mode will provide the lowest possible latency.
   * You should close EXCLUSIVE streams immediately when you are not using them.
   *
   * If you do not need the lowest possible latency then we recommend using
   * Shared, which is the default.
   */
  Exclusive = 0,
  /**
   * Multiple applications can share the same device.
   * The data from output streams will be mixed by the audio service.
   * The data for input streams will be distributed by the audio service.
   *
   * This will have higher latency than the EXCLUSIVE mode.
   */
  Shared = 1,
};

/**
 * The performance mode of the audio stream.
 */
enum class PerformanceMode : uint8_t {
  /**
   * No particular performance needs. Default.
   */
  None = 10, // PERFORMANCE_MODE_NONE,

  /**
   * Extending battery life is most important.
   */
  PowerSaving = 11, // PERFORMANCE_MODE_POWER_SAVING,

  /**
   * Reducing latency is most important.
   */
  LowLatency = 12, // PERFORMANCE_MODE_LOW_LATENCY
};

/**
 * The ContentType attribute describes *what* you are playing.
 * It expresses the general category of the content. This information is
 * optional. But in case it is known (for instance {@link Movie} for a movie
 * streaming service or {@link Speech} for an audio book application) this
 * information might be used by the audio framework to enforce audio focus.
 *
 * Note that these match the equivalent values in AudioAttributes in the Android
 * Java API.
 *
 * This attribute only has an effect on Android API 28+.
 *
 * For iOS, it would convert to AVAudioSessionCategory.
 */
enum class ContentType : int32_t {
  /**
   * Use this for spoken voice, audio books, etcetera.
   */
  Speech = 1, // CONTENT_TYPE_SPEECH

  /**
   * Use this for pre-recorded or live music.
   */
  Music = 2, // CONTENT_TYPE_MUSIC

  /**
   * Use this for a movie or video soundtrack.
   */
  Movie = 3, // CONTENT_TYPE_MOVIE

  /**
   * Use this for sound is designed to accompany a user action,
   * such as a click or beep sound made when the user presses a button.
   */
  Sonification = 4, // CONTENT_TYPE_SONIFICATION
};

/**
 * Defines the audio source.
 * An audio source defines both a default physical source of audio signal, and a
 * recording configuration.
 *
 * Note that these match the equivalent values in MediaRecorder.AudioSource in
 * the Android Java API.
 *
 * This attribute only has an effect on Android API 28+.
 *
 * For iOS, it would convert to AVAudioSession settings.
 */
enum class InputPreset : int32_t {
  /**
   * Use this preset when other presets do not apply.
   */
  Generic = 1, // AUDIO_INPUT_PRESET_GENERIC

  /**
   * Use this preset when recording video.
   */
  Camcorder = 5, // AUDIO_INPUT_PRESET_CAMCORDER

  /**
   * Use this preset when doing speech recognition.
   */
  VoiceRecognition = 6, // AUDIO_INPUT_PRESET_VOICE_RECOGNITION

  /**
   * Use this preset when doing telephony or voice messaging.
   */
  VoiceCommunication = 7, // AUDIO_INPUT_PRESET_VOICE_COMMUNICATION

  /**
   * Use this preset to obtain an input with no effects.
   * Note that this input will not have automatic gain control
   * so the recorded volume may be very low.
   */
  Unprocessed = 9, // AUDIO_INPUT_PRESET_UNPROCESSED

  /**
   * Use this preset for capturing audio meant to be processed in real time
   * and played back for live performance (e.g karaoke).
   * The capture path will minimize latency and coupling with playback path.
   */
  VoicePerformance = 10, // AUDIO_INPUT_PRESET_VOICE_PERFORMANCE
};

} // namespace coral

#endif //CORAL_PRIVATE_AUDIO_DEFINITIONS_H
