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

#include "private/audio/AudioStreamTypes.h"

#include <coral/audio/AudioTypes.h>
#include <media/Definitions.h>

using namespace coral;

template<>
const char* convertToText<Result>(const Result code) {
  switch (code) {
    case Result::OK:
      return "OK";
    case Result::ErrorDisconnected:
      return "ErrorDisconnected";
    case Result::ErrorIllegalArgument:
      return "ErrorIllegalArgument";
    case Result::ErrorInternal:
      return "ErrorInternal";
    case Result::ErrorInvalidState:
      return "ErrorInvalidState";
    case Result::ErrorInvalidHandle:
      return "ErrorInvalidHandle";
    case Result::ErrorUnimplemented:
      return "ErrorUnimplemented";
    case Result::ErrorUnavailable:
      return "ErrorUnavailable";
    case Result::ErrorNoFreeHandles:
      return "ErrorNoFreeHandles";
    case Result::ErrorNoMemory:
      return "ErrorNoMemory";
    case Result::ErrorNull:
      return "ErrorNull";
    case Result::ErrorTimeout:
      return "ErrorTimeout";
    case Result::ErrorWouldBlock:
      return "ErrorWouldBlock";
    case Result::ErrorInvalidFormat:
      return "ErrorInvalidFormat";
    case Result::ErrorOutOfRange:
      return "ErrorOutOfRange";
    case Result::ErrorNoService:
      return "ErrorNoService";
    case Result::ErrorInvalidRate:
      return "ErrorInvalidRate";
    case Result::ErrorClosed:
      return "ErrorClosed";
    default:
      return "Unrecognized result";
  }
}

template<>
const char* convertToText<PerformanceMode>(const PerformanceMode mode) {
  switch (mode) {
    case PerformanceMode::None:
      return "None";
    case PerformanceMode::LowLatency:
      return "LowLatency";
    case PerformanceMode::PowerSaving:
      return "PowerSaving";
    default:
      return "Unrecognized PerformanceMode";
  }
}

template<>
const char* convertToText<Direction>(const Direction direction) {
  switch (direction) {
    case Direction::Render:
      return "Render";
    case Direction::Capture:
      return "Capture";
  }
}

template<>
const char* convertToText<DataCallbackResult>(const DataCallbackResult result) {
  switch (result) {
    case DataCallbackResult::Stop:
      return "Stop";
    case DataCallbackResult::Continue:
      return "Continue";
    default:
      return "Unrecognized DataCallbackResult";
  }
}

template <>
const char *convertToText<StreamState>(StreamState state) {
  switch (state) {
    case StreamState::Closed:
      return "Closed";
    case StreamState::Closing:
      return "Closing";
    case StreamState::Disconnected:
      return "Disconnected";
    case StreamState::Flushed:
      return "Flushed";
    case StreamState::Flushing:
      return "Flushing";
    case StreamState::Open:
      return "Open";
    case StreamState::Paused:
      return "Paused";
    case StreamState::Pausing:
      return "Pausing";
    case StreamState::Started:
      return "Started";
    case StreamState::Starting:
      return "Starting";
    case StreamState::Stopped:
      return "Stopped";
    case StreamState::Stopping:
      return "Stopping";
    case StreamState::Uninitialized:
      return "Uninitialized";
    case StreamState::Unknown:
      return "Unknown";
    default:
      return "Unrecognized stream state";
  }
}

template<>
const char *convertToText<ContentType>(ContentType contentType) {

  switch (contentType) {
    case ContentType::Speech:
      return "Speech";
    case ContentType::Music:
      return "Music";
    case ContentType::Movie:
      return "Movie";
    case ContentType::Sonification:
      return "Sonification";
    default:
      return "Unrecognized content type";
  }
}

template<>
const char *convertToText<InputPreset>(InputPreset inputPreset) {
  switch (inputPreset) {
    case InputPreset::Generic:
      return "Generic";
    case InputPreset::Camcorder:
      return "Camcorder";
    case InputPreset::VoiceRecognition:
      return "VoiceRecognition";
    case InputPreset::VoiceCommunication:
      return "VoiceCommunication";
    case InputPreset::Unprocessed:
      return "Unprocessed";
    case InputPreset::VoicePerformance:
      return "VoicePerformance";
    default:
      return "Unrecognized input preset";
  }
}
