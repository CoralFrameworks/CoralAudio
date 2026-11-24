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

#ifndef CORAL_PRIVATE_AUDIO_OP_RESULT_H
#define CORAL_PRIVATE_AUDIO_OP_RESULT_H

#include <coral/audio/AudioTypes.h>

#include <type_traits>

namespace coral {

/**
 * An OpResult can store both the result of an operation (either OK or an error) and a value.
 */
template<typename T>
class OpResult {
public:
  constexpr OpResult(const Result error) noexcept
    : mError(error),
      mValue{} {
  }

  explicit OpResult(T value) noexcept
    : mError(Result::OK),
      mValue(value) {
  }

  OpResult(T value, const T error) noexcept
    : mError(error),
      mValue(value) {
  }

  /**
   * Returns the error
   */
  [[nodiscard]] Result error() const noexcept {
    return mError;
  }

  /**
   * Returns the value
   */
  T value() const noexcept {
    return mValue;
  }

  /**
   * Whether is ok
   */
  explicit operator bool() const noexcept {
    return mError == Result::OK;
  }

  /**
   * Quick way to check for an error.
   */
  bool operator !() const noexcept {
    return mError != Result::OK;
  }


  static OpResult Create(T numericResult) {
    static_assert(std::is_arithmetic_v<T>, "Only be called for numeric types (int or float)");
    if (numericResult >= 0) {
      return OpResult(numericResult);
    }
    return OpResult(static_cast<Result>(numericResult));
  }

private:
  const Result mError;
  const T mValue;
};

/**
 * If the result is `Result::kOK` then return the value, otherwise return a human-readable error message.
 */
template<typename T>
std::ostream& operator<<(std::ostream& str, const OpResult<T>& result) {
  if (!result) {
    str << convertToText(result.error());
  } else {
    str << result.value();
  }
  return str;
}

} // namespace coral


#endif //CORAL_PRIVATE_AUDIO_OP_RESULT_H
