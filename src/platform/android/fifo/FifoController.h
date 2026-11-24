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

#ifndef CORAL_AUDIO_FIFO_CONTROLLER_H
#define CORAL_AUDIO_FIFO_CONTROLLER_H

#include "FifoControllerBase.h"

#include <atomic>
#include <cstdint>

namespace coral {

class FifoController final : public FifoControllerBase {
public:
  explicit FifoController(AudioFrameCount totalFrames) noexcept;

  ~FifoController() override = default;

  FifoController(const FifoController &) = delete;
  FifoController &operator=(const FifoController &) = delete;

  uint64_t getReadCounter() const override {
    return mReadCounter.load(std::memory_order_acquire);
  }

  void setReadCounter(const uint64_t n) override {
    mReadCounter.store(n, std::memory_order_release);
  }

  void incrementReadCounter(const uint64_t n) override {
    mReadCounter.fetch_add(n, std::memory_order_acq_rel);
  }

  uint64_t getWriteCounter() const override {
    return mWriteCounter.load(std::memory_order_acquire);
  }

  void setWriteCounter(const uint64_t n) override {
    mWriteCounter.store(n, std::memory_order_release);
  }

  void incrementWriteCounter(const uint64_t n) override {
    mWriteCounter.fetch_add(n, std::memory_order_acq_rel);
  }

private:
  std::atomic<uint64_t> mReadCounter{};
  std::atomic<uint64_t> mWriteCounter{};
};

} // namespace coral

#endif // CORAL_AUDIO_FIFO_CONTROLLER_H
