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

// =============================================================
//  tests_AudioThreadPriority.cpp
//
//  Exercises coral::AudioThreadPriority against the real platform:
//    - the backend is selected and named
//    - a thread can report its own scheduling priority
//    - promotion either succeeds, or fails with a usable error
//    - a successful promotion really raises the priority and demotion restores
//      the exact previous policy and priority
//    - RealTimeScope demotes on scope exit, on early return and when moved
//    - invalid handles and unsupported operations are reported, not ignored
//    - the promoted thread keeps rendering correctly (the point of it all)
//
//  Promotion is expected to fail when the process is not allowed to raise its
//  priority (unprivileged desktop Linux without RLIMIT_RTPRIO is the usual
//  case), which is not a test failure: the library must stay usable.
// =============================================================

#include "private/common/AudioThreadPriority.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Keep windows.h from defining min/max, which would shadow std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace coral;

namespace {

int gChecks = 0;
int gFailures = 0;

void check(const bool condition, const std::string& what) {
  ++gChecks;
  if (condition) {
    std::printf("  ok   %s\n", what.c_str());
  } else {
    ++gFailures;
    std::printf("  FAIL %s\n", what.c_str());
  }
}

void section(const char* title) {
  std::printf("\n== %s ==\n", title);
}

/** A tiny render callback, used to prove the thread still does its work. */
float renderBlock(const uint32_t frames, const uint32_t rate, const int phase) {
  double sum = 0.0;
  for (uint32_t f = 0; f < frames; ++f) {
    const double t = static_cast<double>(f + phase) / static_cast<double>(rate);
    sum += std::sin(2.0 * 3.14159265358979323846 * 440.0 * t);
  }
  return static_cast<float>(sum / static_cast<double>(frames));
}

/**
 * Reads the calling thread's priority, reporting a value whatever happens.
 *
 * `ThreadInfo::current()` is documented to be able to return `std::nullopt`, so
 * every caller has to handle that case rather than dereferencing it.
 *
 * @param outPriority receives the priority when one could be read.
 * @return `false` when the platform cannot describe or report the thread, so the
 *         caller knows not to compare anything.
 */
bool currentPriority(int32_t* outPriority) {
  const auto info = AudioThreadPriority::ThreadInfo::current();
  if (!info || !info->isValid()) {
    return false;
  }
  return info->getCurrentPriority(outPriority);
}

/**
 * Counts how many times the rendered text spells out a code.
 *
 * Every spelling the library uses has to be counted, not just the one a
 * particular platform happens to produce: a check that only looked for "(code "
 * would miss a second rendering written as "[code ...]".
 */
size_t countCodeMarkers(const std::string& text) {
  static const char* const kMarkers[] = {"(platform code ", "(code ", "[code "};
  size_t total = 0;
  for (const char* marker : kMarkers) {
    const std::string needle = marker;
    size_t at = text.find(needle);
    while (at != std::string::npos) {
      ++total;
      at = text.find(needle, at + needle.size());
    }
  }
  return total;
}

} // namespace anonymous

int main() {
  std::printf("AudioThreadPriority tests\n");
  std::printf("backend: %s\n", AudioThreadPriority::backendName());

  section("Backend and thread description");
  {
    check(std::strlen(AudioThreadPriority::backendName()) > 0, "the backend has a name");

    const auto info = AudioThreadPriority::ThreadInfo::current();
    check(info.has_value(), "the calling thread can be described");
    check(info && info->isValid(), "the description is valid");
    check(!AudioThreadPriority::ThreadInfo{}.isValid(), "a default ThreadInfo is invalid");

    int32_t priority = -1;
    const bool reported = currentPriority(&priority);
    if (reported) {
      std::printf("       current priority = %d\n", priority);
    }
    check(reported, "the calling thread reports its scheduling priority");
    check(!reported || (priority >= AudioThreadPriority::kMinRealTimePriority &&
                           priority <= AudioThreadPriority::kMaxRealTimePriority),
        "the reported priority is inside the documented range");

    int32_t unused = 0;
    check(!AudioThreadPriority::ThreadInfo{}.getCurrentPriority(&unused),
        "an invalid ThreadInfo reports no priority");
    // A null out-parameter is rejected on a valid description too, but only when
    // the platform was able to describe the thread at all.
    const auto valid = AudioThreadPriority::ThreadInfo::current();
    if (valid) {
      check(valid->isValid() && !valid->getCurrentPriority(nullptr),
          "a null out-parameter is rejected");
    } else {
      check(true, "the platform cannot describe the thread, so the null check does not apply");
    }
  }

  section("Real-time limit");
  {
    AudioThreadPriority::LastError error;
    const bool raised = AudioThreadPriority::setRealTimeLimit(AudioThreadPriority::kMaxRealTimePriority, &error);
    if (!raised) {
      std::printf("       not permitted: %s\n", error.toString().c_str());
    }
    // Either answer is legitimate; what matters is that it is reported cleanly.
    check(raised || !error.empty(), "a refusal comes with an error description");
  }

  section("Promotion, priority change and demotion");
  {
    const auto before = AudioThreadPriority::ThreadInfo::current();
    int32_t priorityBefore = 0;
    const bool hadPriority = before && before->getCurrentPriority(&priorityBefore);

    AudioThreadPriority::LastError error;
    const auto handle = AudioThreadPriority::promoteCurrentThread(512, 48000,
        AudioThreadPriority::kMaxRealTimePriority, &error);

    if (!handle.isValid()) {
      std::printf("       promotion refused (expected when unprivileged): %s\n", error.toString().c_str());
      check(!error.empty(), "a refused promotion carries an error description");
    } else {
      check(handle.isValid(), "promotion returned a usable handle");

      int32_t priorityAfter = 0;
      const bool readAfter = currentPriority(&priorityAfter);
      std::printf("       priority %d -> %d\n", priorityBefore, priorityAfter);
      check(readAfter, "the priority can be read back after promotion");
      if (hadPriority && readAfter) {
        check(priorityAfter >= priorityBefore, "promotion did not lower the priority");
        check(priorityAfter > priorityBefore, "promotion raised the priority");
      }

      AudioThreadPriority::LastError demoteError;
      const bool demoted = AudioThreadPriority::demoteCurrentThread(handle, &demoteError);
      check(demoted, std::string("demotion succeeded") +
                         (demoted ? "" : (": " + demoteError.toString())));
      if (demoted && hadPriority) {
        int32_t priorityRestored = 0;
        const bool readRestored = currentPriority(&priorityRestored);
        std::printf("       restored priority = %d (was %d)\n", priorityRestored, priorityBefore);
        check(readRestored && priorityRestored == priorityBefore,
            "demotion restored the exact previous priority");
      }
    }
  }

  section("Operations on an invalid handle are reported");
  {
    AudioThreadPriority::LastError error;
    check(!AudioThreadPriority::demoteCurrentThread(AudioThreadPriority::Handle{}, &error),
        "demoting an invalid handle fails");
    check(!error.empty(), "the failure carries an error description");

    error = AudioThreadPriority::LastError{};
    check(!AudioThreadPriority::demoteThread(AudioThreadPriority::ThreadInfo{},
              AudioThreadPriority::Handle{}, &error),
        "demoting an invalid thread info fails");
    check(!error.empty(), "the failure carries an error description");
  }

  section("RealTimeScope demotes on scope exit");
  {
    const auto before = AudioThreadPriority::ThreadInfo::current();
    int32_t priorityBefore = 0;
    const bool hadPriority = before && before->getCurrentPriority(&priorityBefore);

    bool activeInside = false;
    {
      RealTimeScope scope(256, 48000, AudioThreadPriority::kMaxRealTimePriority);
      activeInside = scope.isActive();
      check(scope.lastError().empty() || !scope.isActive(),
          "an inactive scope explains why it is inactive");
      std::printf("       scope active = %d, previous priority = %d\n", static_cast<int>(activeInside),
          scope.previousPriority());
    }

    int32_t priorityAfter = 0;
    const bool readAfter = currentPriority(&priorityAfter);
    check(readAfter, "the priority can be read after the scope ended");
    if (hadPriority && readAfter) {
      std::printf("       priority after scope = %d (was %d)\n", priorityAfter, priorityBefore);
      check(priorityAfter == priorityBefore, "the scope restored the priority on exit");
    }
    (void) activeInside;
  }

  section("RealTimeScope demotes when the scope exits early");
  {
    const auto before = AudioThreadPriority::ThreadInfo::current();
    int32_t priorityBefore = 0;
    const bool hadPriority = before && before->getCurrentPriority(&priorityBefore);

    // The exit paths are reached through a function pointer so the compiler
    // cannot fold them away, which keeps this a real early exit: the scope's
    // destructor runs in a function the caller never sees return, with the
    // scope still live.
    using PromotePath = bool (*)(volatile int*);
    volatile PromotePath path = [](volatile int* sideEffect) -> bool {
      RealTimeScope scope(512, 44100, AudioThreadPriority::kDefaultRealTimePriority);
      *sideEffect = 1; // The scope is alive and doing work here.
      if (!scope.isActive()) {
        return false; // Early exit with the scope alive but inactive.
      }
      return true; // Normal exit at the end of the function body.
    };

    volatile int sideEffect = 0;
    const bool active = path(&sideEffect);
    std::printf("       early-exit scope active = %d (destructor ran inside the callee)\n",
        static_cast<int>(active));
    check(sideEffect == 1, "the scope was live when the early exit was taken");

    int32_t priorityAfter = 0;
    const bool readAfter = currentPriority(&priorityAfter);
    if (hadPriority && readAfter) {
      std::printf("       priority after early exit = %d (was %d)\n", priorityAfter, priorityBefore);
      check(priorityAfter == priorityBefore, "the destructor restored the priority inside the callee");
    } else {
      check(true, "priority comparison unavailable on this platform");
    }
  }

  section("RealTimeScope demotes when an exception unwinds through it");
  {
    const auto before = AudioThreadPriority::ThreadInfo::current();
    int32_t priorityBefore = 0;
    const bool hadPriority = before && before->getCurrentPriority(&priorityBefore);

    bool caught = false;
    try {
      RealTimeScope scope(256, 48000, AudioThreadPriority::kDefaultRealTimePriority);
      // Throwing out of a scope the caller never asked to leave is the other
      // case where a guard earns its keep.
      throw std::runtime_error("unwind through the real-time scope");
    } catch (const std::runtime_error&) {
      caught = true;
    }
    check(caught, "the exception propagated out of the scope");

    int32_t priorityAfter = 0;
    const bool readAfter = currentPriority(&priorityAfter);
    if (hadPriority && readAfter) {
      std::printf("       priority after unwind = %d (was %d)\n", priorityAfter, priorityBefore);
      check(priorityAfter == priorityBefore, "the destructor restored the priority while unwinding");
    } else {
      check(true, "priority comparison unavailable on this platform");
    }
  }

  section("RealTimeScope transfers ownership when moved");
  {
    const auto before = AudioThreadPriority::ThreadInfo::current();
    int32_t priorityBefore = 0;
    const bool hadPriority = before && before->getCurrentPriority(&priorityBefore);

    {
      RealTimeScope outer(512, 48000);
      RealTimeScope inner(std::move(outer));
      check(!outer.isActive(), "the moved-from scope is inactive");
      check(inner.isActive() || !inner.lastError().empty(),
          "the moved-to scope owns the promotion or reports why not");
    }

    int32_t priorityAfter = 0;
    const bool readAfter = currentPriority(&priorityAfter);
    if (hadPriority && readAfter) {
      check(priorityAfter == priorityBefore, "the moved scope restored the priority once");
    } else {
      check(true, "priority comparison unavailable on this platform");
    }
  }

  section("A promoted thread keeps rendering");
  {
    std::atomic<bool> started{ false };
    std::atomic<int> blocks{ 0 };
    std::atomic<float> peak{ 0.0f };
    std::atomic<bool> promoted{ false };

    std::thread worker([&] {
      const auto handle = AudioThreadPriority::promoteCurrentThread(512, 48000);
      promoted.store(handle.isValid(), std::memory_order_relaxed);
      started.store(true, std::memory_order_release);

      for (int i = 0; i < 200; ++i) {
        const float value = renderBlock(512, 48000, i * 512);
        peak.store(std::max(peak.load(std::memory_order_relaxed), std::fabs(value)),
            std::memory_order_relaxed);
        blocks.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }

      if (handle.isValid()) {
        AudioThreadPriority::LastError error;
        const bool demoted = AudioThreadPriority::demoteCurrentThread(handle, &error);
        if (!demoted) {
          std::printf("       worker demotion failed: %s\n", error.toString().c_str());
        }
      }
    });

    while (!started.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    worker.join();

    std::printf("       worker: promoted=%d blocks=%d peak=%.4f\n", static_cast<int>(promoted.load()),
        blocks.load(), peak.load());
    check(blocks.load() == 200, "the promoted thread completed every render block");
    check(peak.load() > 0.001f, "the promoted thread produced audio");
  }

  section("Default and priority range");
  {
#if defined(CORAL_BUILD_FOR_LINUX)
    // Linux audio I/O uses the middle of the SCHED_FIFO band, leaving the top
    // for hard real-time work.
    check(AudioThreadPriority::kDefaultRealTimePriority == 6,
        "the Linux default leaves the top of the real-time band free");
#else
    check(AudioThreadPriority::kDefaultRealTimePriority == 10,
        "the default is the top of the supported range off Linux");
#endif
    check(AudioThreadPriority::kDefaultRealTimePriority >= AudioThreadPriority::kMinRealTimePriority &&
              AudioThreadPriority::kDefaultRealTimePriority <= AudioThreadPriority::kMaxRealTimePriority,
        "the default priority is inside the supported range");
    std::printf("       default priority: %d of [%d, %d]\n",
        AudioThreadPriority::kDefaultRealTimePriority,
        AudioThreadPriority::kMinRealTimePriority,
        AudioThreadPriority::kMaxRealTimePriority);
  }

  section("Buffer size derivation");
  {
    // The derived period must land in the range render callbacks actually use:
    // 3-15 ms, that is 128-2048 frames at 48 kHz.
    const uint32_t derived48 = AudioThreadPriority::derivedBufferFrames(48000);
    const uint32_t derived44 = AudioThreadPriority::derivedBufferFrames(44100);
    const uint32_t derived96 = AudioThreadPriority::derivedBufferFrames(96000);
    const uint32_t derivedFallback = AudioThreadPriority::derivedBufferFrames(0);
    const double ms48 = static_cast<double>(derived48) / 48000.0 * 1000.0;
    std::printf("       derived frames: 48k=%u (%.2f ms) 44.1k=%u 96k=%u default=%u\n", derived48, ms48,
        derived44, derived96, derivedFallback);

    check(derived48 >= 128 && derived48 <= 2048, "the derived 48 kHz callback is a realistic size");
    check(ms48 >= 3.0 && ms48 <= 15.0, "the derived 48 kHz period is between 3 and 15 ms");
    check(derived44 > 0 && derived96 > 0, "other sample rates derive usable sizes");
    check(derivedFallback == derived48, "a missing sample rate falls back to 48 kHz");
    check(derived48 > 0, "the derived size is never zero");

    // A zero frame count must still produce a usable promotion request.
    AudioThreadPriority::LastError error;
    const auto handle = AudioThreadPriority::promoteCurrentThread(0, 48000, 0, &error);
    if (handle.isValid()) {
      check(AudioThreadPriority::demoteCurrentThread(handle), "a derived buffer size still promotes and demotes");
    } else {
      check(!error.empty(), "a derived buffer size reports why promotion failed");
    }

    // Out-of-range priorities are clamped rather than rejected.
    AudioThreadPriority::LastError highError;
    const auto high = AudioThreadPriority::promoteCurrentThread(512, 48000, 999, &highError);
    if (high.isValid()) {
      check(AudioThreadPriority::demoteCurrentThread(high), "an out-of-range priority is clamped, not rejected");
    } else {
      check(!highError.empty(), "an out-of-range priority reports why promotion failed");
    }

    AudioThreadPriority::LastError lowError;
    const auto low = AudioThreadPriority::promoteCurrentThread(512, 48000, -50, &lowError);
    if (low.isValid()) {
      check(AudioThreadPriority::demoteCurrentThread(low), "a negative priority is clamped, not rejected");
    } else {
      check(!lowError.empty(), "a negative priority reports why promotion failed");
    }
  }

  section("Promoting another thread");
  {
    const auto info = AudioThreadPriority::ThreadInfo::current();
    check(info.has_value(), "the calling thread can be described");

    if (info) {
      AudioThreadPriority::LastError error;
      const auto handle = AudioThreadPriority::promoteThread(*info, 512, 48000, 10, &error);
#if defined(__ANDROID__)
      // Android promotes by Linux thread id, so this is expected to work there.
      if (handle.isValid()) {
        check(AudioThreadPriority::demoteThread(*info, handle), "the promoted thread was demoted");
      } else {
        check(!error.empty(), "a refused out-of-thread promotion explains itself");
      }
#else
      // Everywhere else this is documented as unsupported, and must say so.
      check(!handle.isValid(), "promoting another thread is refused off Android");
      check(!error.empty(), "the refusal carries an error description");
      std::printf("       %s\n", error.what());

      AudioThreadPriority::LastError demoteError;
      check(!AudioThreadPriority::demoteThread(*info, AudioThreadPriority::Handle{}, &demoteError),
          "demoting another thread is refused off Android");
      check(!demoteError.empty(), "the refusal carries an error description");
#endif
    } else {
      // Without a description there is nothing to promote, so the out-of-thread
      // path cannot be exercised on this platform.
      check(true, "the platform cannot describe a thread, so out-of-thread promotion is untestable");
    }
  }

  section("makeRealTimeScope derives the buffer size");
  {
    const auto before = AudioThreadPriority::ThreadInfo::current();
    int32_t priorityBefore = 0;
    const bool hadPriority = before && before->getCurrentPriority(&priorityBefore);

    {
      // No explicit frame count: the scope derives one from the sample rate.
      RealTimeScope scope = makeRealTimeScope(48000, AudioThreadPriority::kDefaultRealTimePriority);
      check(scope.isActive() || !scope.lastError().empty(),
          "the convenience scope is either active or explains why not");
    }

    int32_t priorityAfter = 0;
    const bool readAfter = currentPriority(&priorityAfter);
    if (hadPriority && readAfter) {
      check(priorityAfter == priorityBefore, "the convenience scope restored the priority");
    } else {
      check(true, "priority comparison unavailable on this platform");
    }
  }

  section("Computation budget stays within the recommended share");
  {
    const double atDefault = AudioThreadPriority::computationBudgetFraction();
    const double atLowest = AudioThreadPriority::computationBudgetFraction(
        AudioThreadPriority::kMinRealTimePriority);
    const double atHighest = AudioThreadPriority::computationBudgetFraction(
        AudioThreadPriority::kMaxRealTimePriority);
    std::printf("       budget: lowest=%.3f default=%.3f highest=%.3f\n", atLowest, atDefault, atHighest);

    check(atLowest > 0.0 && atLowest <= 0.5, "the lowest budget is a positive share of the period");
    check(atDefault <= 0.5, "the default priority asks for no more than half the callback period");
    check(atHighest <= 0.5, "the highest priority asks for no more than half the callback period");
    check(atLowest <= atDefault && atDefault <= atHighest, "a higher priority never asks for less budget");
    check(AudioThreadPriority::computationBudgetFraction(5) < atHighest,
        "an intermediate priority asks for strictly less than the highest");
    // macOS 12 and later expect an audio callback to ask for about half its
    // period, so the top of the range has to land exactly there.
    check(std::fabs(atHighest - 0.5) < 1e-9, "the top of the range is exactly one half");
    // The clamp has to hold even when a caller asks for nonsense.
    check(AudioThreadPriority::computationBudgetFraction(999) <= 0.5,
        "an out-of-range priority does not inflate the budget");
    check(AudioThreadPriority::computationBudgetFraction(-50) > 0.0,
        "a negative priority still yields a usable budget");
  }

  section("previousPriority reports the state the promotion replaced");
  {
    const auto before = AudioThreadPriority::ThreadInfo::current();
    int32_t expectedPrevious = 0;
    const bool hadPriority = before && before->getCurrentPriority(&expectedPrevious);

    const RealTimeScope scope(512, 48000, AudioThreadPriority::kMaxRealTimePriority);
    if (scope.isActive()) {
      std::printf("       previous=%d expected=%d\n", scope.previousPriority(), expectedPrevious);
      if (hadPriority) {
        check(scope.previousPriority() == expectedPrevious,
            "the scope reports the priority that was in force before promotion");
      } else {
        check(true, "priority comparison unavailable on this platform");
      }
    } else {
      check(!scope.lastError().empty(), "an inactive scope explains why it is inactive");
    }
  }

#if defined(_WIN32)
  section("Windows restores the priority the thread actually had");
  {
    // A thread that was not at the default priority must come back to the value
    // it had, not to THREAD_PRIORITY_NORMAL.
    const int original = ::GetThreadPriority(::GetCurrentThread());
    check(original != THREAD_PRIORITY_ERROR_RETURN, "the thread priority can be read");
    check(::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0,
        "the thread priority was lowered for the test");

    {
      const RealTimeScope scope(512, 48000, AudioThreadPriority::kMaxRealTimePriority);
      check(scope.isActive() || !scope.lastError().empty(),
          "the scope is either active or explains why not");
    }

    const int restored = ::GetThreadPriority(::GetCurrentThread());
    std::printf("       priority: original=%d lowered=%d restored=%d\n", original,
        THREAD_PRIORITY_BELOW_NORMAL, restored);
    check(restored == THREAD_PRIORITY_BELOW_NORMAL,
        "demotion restored the lowered priority, not the default");

    // Leave the thread as it was found.
    check(::SetThreadPriority(::GetCurrentThread(), original) != 0, "the original priority was put back");
  }
#endif

  section("Error reporting is bounded, allocated only on demand, and usable from noexcept code");
  {
    // Every failure path runs inside a noexcept function, so it must fill the
    // message into fixed storage instead of building a std::string. The buffer is
    // 256 bytes, so a description can never be longer than that and is always
    // terminated.
    AudioThreadPriority::LastError error;

    // A refusal from a promotion attempt exercises a real message path.
    const auto doomed = AudioThreadPriority::promoteThread(AudioThreadPriority::ThreadInfo{}, 512, 48000, 10, &error);
    check(!doomed.isValid(), "an invalid thread info cannot be promoted");
    check(!error.empty(), "the failure is described");

    const size_t length = std::strlen(error.what());
    std::printf("       message (%zu chars): %s\n", length, error.what());
    check(length > 0, "the description is a usable C string");
    check(length < AudioThreadPriority::LastError::kMessageCapacity,
        "the description always fits the fixed buffer and stays terminated");
    check(error.what()[length] == '\0', "the description is terminated right after its length");

    // The allocating accessor is only used when a caller reports the failure.
    const std::string rendered = error.toString();
    check(!rendered.empty(), "toString() renders the description for a human");

    // toString() must not append the code a second time in a second format: the
    // text is what setError produced, so the code appears exactly once, in one
    // spelling, and toString() adds nothing to it.
    check(rendered == std::string(error.what()),
        "toString() renders exactly the message, adding no second copy of the code");
    const std::string marker = error.codeFromPlatform() ? "(platform code " : "(code ";
    check(rendered.find(marker) != std::string::npos || error.code == 0,
        "the code is spelled the way its origin says it should be");

    const size_t markers = countCodeMarkers(rendered);
    std::printf("       code=%lld fromPlatform=%d markers=%zu: %s\n",
        static_cast<long long>(error.code), static_cast<int>(error.codeFromPlatform()), markers,
        rendered.c_str());
    if (error.code != 0) {
      check(markers == 1, "a non-zero code is spelled out exactly once");
    } else {
      check(markers == 0, "no code is mentioned when there is none");
    }

    // A successful promotion must not leave a stale description behind.
    AudioThreadPriority::LastError cleared;
    const auto handle = AudioThreadPriority::promoteCurrentThread(512, 48000,
        AudioThreadPriority::kDefaultRealTimePriority, &cleared);
    if (handle.isValid()) {
      check(cleared.empty(), "a successful promotion clears the error");
      check(AudioThreadPriority::demoteCurrentThread(handle), "the promotion was undone");
    } else {
      check(!cleared.empty(), "a failed promotion explains itself");
    }
  }

  section("A refused demotion keeps the promotion, so it can be retried");
  {
    // A demotion the platform accepts must make the scope inactive. The failure
    // case cannot be forced portably, so this pins the success side of the
    // contract that the failure side is defined against.
    RealTimeScope scope(512, 48000, AudioThreadPriority::kMaxRealTimePriority);
    if (scope.isActive()) {
      const int32_t previous = scope.previousPriority();
      const bool demoted = scope.demote();
      check(demoted, "the demotion was accepted");
      check(!scope.isActive(), "a completed demotion makes the scope inactive");
      check(scope.lastError().empty(), "a completed demotion leaves no error behind");
      // A cleared scope must not keep describing a previous state it no longer
      // has: previousPriority() is only meaningful while isActive() is true.
      check(scope.previousPriority() == 0, "a completed demotion clears the remembered priority");
      std::printf("       previousPriority: %d before demote, %d after\n", previous,
          scope.previousPriority());

      // Demoting again is refused, and must not report a bogus restore.
      check(!scope.demote(), "demoting an already demoted scope reports nothing to do");
    } else {
      check(!scope.lastError().empty(), "an inactive scope explains why it is inactive");
    }
  }

  std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
  std::printf("%s\n", gFailures == 0 ? "tests_AudioThreadPriority: PASS" : "tests_AudioThreadPriority: FAIL");
  return gFailures == 0 ? 0 : 1;
}
