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

#include "private/common/AudioThreadPriority.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <mutex>

// =============================================================================
//  Platform detection
//
//  utils/compiler.h defines exactly one CORAL_BUILD_FOR_* macro; Apple splits
//  into macOS and iOS, which share the Mach time-constraint policy.
// =============================================================================

#if defined(CORAL_BUILD_FOR_WIN)
#define CORAL_ATP_WINDOWS 1
#elif defined(CORAL_BUILD_FOR_MAC) || defined(CORAL_BUILD_FOR_IOS)
#define CORAL_ATP_APPLE 1
#elif defined(CORAL_BUILD_FOR_ANDROID)
#define CORAL_ATP_ANDROID 1
#define CORAL_ATP_POSIX 1
#elif defined(CORAL_BUILD_FOR_LINUX)
#define CORAL_ATP_LINUX 1
#define CORAL_ATP_POSIX 1
#else
#define CORAL_ATP_STUB 1
#endif

// =============================================================================
//  Platform includes
// =============================================================================

#if defined(CORAL_ATP_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

#elif defined(CORAL_ATP_APPLE)

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/resource.h>

#include <cstdint>

// The promotion payload below stores the previous time-constraint policy inline, so the alias has to
// be visible before it. It used to be declared with the Apple implementation further down, which
// made the file fail to compile on macOS.
using ApplePolicy = thread_time_constraint_policy_data_t;

#elif defined(CORAL_ATP_POSIX)

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(CORAL_ATP_ANDROID)
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 21
#include <sys/syscall.h>
#endif
#endif

#endif

namespace coral {

namespace {

// =============================================================================
//  Shared helpers
// =============================================================================

/**
 * The callback size used when a promotion is asked for with 0 frames.
 *
 * 10 ms of audio, which is 480 frames at 48 kHz: inside the range audio render
 * callbacks actually use (128-2048 frames, 3-15 ms at 48 kHz), so a derived Mach
 * time constraint describes a realistic period. A longer derived period would
 * hand the thread a deadline far looser than the callback it actually runs.
 */
uint32_t derivedBufferFramesFor(const uint32_t audioSampleRateHz) noexcept {
  const uint32_t rate = audioSampleRateHz > 0 ? audioSampleRateHz : 48000u;
  return std::max(1u, rate / 100u);
}

/** The callback size to use, honouring an explicit request when there is one. */
uint32_t deriveBufferFrames(const uint32_t audioBufferFrames, const uint32_t audioSampleRateHz) noexcept {
  if (audioBufferFrames > 0) {
    return audioBufferFrames;
  }
  return derivedBufferFramesFor(audioSampleRateHz);
}

uint32_t effectiveSampleRate(const uint32_t audioSampleRateHz) noexcept {
  return audioSampleRateHz > 0 ? audioSampleRateHz : 48000u;
}

/**
 * Reports the thread's priority on the documented scale: 0 means "not
 * real-time", higher means a stronger guarantee.
 *
 * The platform values are spread over the scale while leaving 0 reserved for the
 * non-real-time cases, so every platform answers the same question the same way.
 * Windows, for example, reports `THREAD_PRIORITY_NORMAL` (0) as "not real-time"
 * rather than as the bottom of the real-time range.
 */
#if defined(CORAL_ATP_WINDOWS)
int32_t scalePriority(const int32_t platformValue, const int32_t platformNotRealTime) noexcept {
  // Five platform steps map onto the ten-point scale, the first being 0.
  static constexpr int32_t kSteps = 5;
  if (platformValue <= platformNotRealTime) {
    return AudioThreadPriority::kMinRealTimePriority;
  }
  const int32_t step = std::min(kSteps - 1, platformValue - platformNotRealTime);
  return static_cast<int32_t>(static_cast<double>(step) / static_cast<double>(kSteps - 1) *
                                 static_cast<double>(AudioThreadPriority::kMaxRealTimePriority) +
                             0.5);
}
#endif

/**
 * Fills in an error without allocating, so it is safe to call on the failure
 * path of a `noexcept` function.
 *
 * This is the only place an error is built, so every platform renders a failure
 * the same way: the description, then a single code in the text when there is
 * one. A platform code is labelled as such, because it belongs to the platform
 * (an `errno`, a Win32 error, a `kern_return_t`); when the platform reported
 * nothing usable, the library's own `fallbackCode` is reported instead so a
 * caller still has something to switch on. Either way the text and the `code`
 * field agree.
 *
 * @param platformCode the value the platform reported, or 0 when it reported
 *                     nothing usable.
 * @param fallbackCode the library's own code, used when `platformCode` is 0.
 */
void setError(AudioThreadPriority::LastError* outError,
    const int64_t fallbackCode,
    const char* what,
    const int64_t platformCode = 0) {
  if (!outError) {
    return;
  }
  const bool fromPlatform = platformCode != 0;
  const int64_t code = fromPlatform ? platformCode : fallbackCode;
  const char* text = what != nullptr ? what : "";
  outError->code = code;
  outError->codeKind =
      fromPlatform ? AudioThreadPriority::LastError::kCodeFromPlatform
                   : AudioThreadPriority::LastError::kCodeFromLibrary;

  if (fromPlatform) {
    std::snprintf(outError->message, sizeof(outError->message), "%s (platform code %lld)", text,
        static_cast<long long>(platformCode));
  } else if (code != 0) {
    // The library's own code, so the text still carries something to look up.
    std::snprintf(outError->message, sizeof(outError->message), "%s (code %lld)", text,
        static_cast<long long>(code));
  } else {
    std::snprintf(outError->message, sizeof(outError->message), "%s", text);
  }
}

/**
 * The code to report for a failed POSIX call.
 *
 * `errno` is only meaningful when the call that failed actually set it; several
 * of the calls used here leave it untouched on failure. Reading it without
 * clearing it first would report a value left over from an unrelated call, so
 * callers zero `errno` immediately before the call and pass what they find.
 */
int64_t posixErrorCode(const int64_t fallbackCode) noexcept {
  return errno != 0 ? static_cast<int64_t>(errno) : fallbackCode;
}

/**
 * The platform-specific promotion state.
 *
 * It is stored inline in `AudioThreadPriority::Handle`, so promoting and
 * demoting a thread never allocates, and the platform's own types stay out of
 * the public header.
 */
struct HandlePayload final {
#if defined(CORAL_ATP_WINDOWS)
  /** MMCSS task handle, or null when only the thread priority was raised. */
  HANDLE task = nullptr;
  bool usedMmcss = false;
  /**
   * The thread priority in force before promotion.
   *
   * Recorded on both paths: MMCSS changes the thread priority itself, and the
   * fallback path changes nothing else, so demotion can only restore the exact
   * previous state if it was captured first.
   */
  int previousThreadPriority = THREAD_PRIORITY_NORMAL;
#elif defined(CORAL_ATP_APPLE)
  /** The time-constraint policy the thread had before promotion. */
  ApplePolicy previous{};
#elif defined(CORAL_ATP_POSIX)
  int previousPolicy = SCHED_OTHER;
  sched_param previousParam{};
  /** Whether the promotion ended up being a nice-value change instead. */
  bool niceFallback = false;
  /** The nice value in force before a nice-based promotion. */
  int previousNice = 0;
  /** The `errno` the failing call actually reported, or 0. */
  int lastErrno = 0;
#else
  int unused = 0;
#endif
};

static_assert(sizeof(HandlePayload) <= 32, "AudioThreadPriority::Handle::mReserved is too small");

/**
 * The payload lives in the handle's reserved storage, so promoting and demoting
 * a thread never allocates.
 */
HandlePayload& payloadOf(AudioThreadPriority::Handle& handle) noexcept {
  return *static_cast<HandlePayload*>(handle.reservedStorage());
}

const HandlePayload& payloadOf(const AudioThreadPriority::Handle& handle) noexcept {
  return *static_cast<const HandlePayload*>(handle.reservedStorage());
}

// =============================================================================
//  Windows: MMCSS
// =============================================================================

#if defined(CORAL_ATP_WINDOWS)

/**
 * MMCSS entry points, resolved from avrt.dll at run time.
 *
 * Resolving them dynamically keeps the library usable on a machine where the
 * Multimedia Class Scheduler Service is unavailable, in which case promotion
 * falls back to a plain thread priority instead of failing to load.
 */
struct MmcssApi {
  using SetCharacteristicsFn = HANDLE(WINAPI*)(LPCSTR, LPDWORD);
  using RevertCharacteristicsFn = BOOL(WINAPI*)(HANDLE);
  using SetMmPriorityFn = BOOL(WINAPI*)(HANDLE, int);

  SetCharacteristicsFn setCharacteristics = nullptr;
  RevertCharacteristicsFn revertCharacteristics = nullptr;
  SetMmPriorityFn setMmPriority = nullptr;

  [[nodiscard]] bool available() const noexcept {
    return setCharacteristics != nullptr && revertCharacteristics != nullptr;
  }
};

/** AVRT_PRIORITY_CRITICAL, from avrt.h. */
constexpr int kAvrtPriorityCritical = 3;

const MmcssApi& mmcss() noexcept {
  static const MmcssApi api = [] {
    MmcssApi resolved;
    if (HMODULE avrt = ::LoadLibraryA("avrt.dll")) {
      resolved.setCharacteristics = reinterpret_cast<MmcssApi::SetCharacteristicsFn>(
          reinterpret_cast<void*>(::GetProcAddress(avrt, "AvSetMmThreadCharacteristicsA")));
      resolved.revertCharacteristics = reinterpret_cast<MmcssApi::RevertCharacteristicsFn>(
          reinterpret_cast<void*>(::GetProcAddress(avrt, "AvRevertMmThreadCharacteristics")));
      resolved.setMmPriority = reinterpret_cast<MmcssApi::SetMmPriorityFn>(
          reinterpret_cast<void*>(::GetProcAddress(avrt, "AvSetMmThreadPriority")));
    }
    return resolved;
  }();
  return api;
}


/**
 * The requested priority on the Windows scale.
 *
 * MMCSS hands out its own share per registered task, so the requested priority
 * only selects the plain thread priority used when MMCSS is unavailable.
 */
int windowsPriorityOf(const int32_t priority) noexcept {
  const int32_t clamped = detail::clampPriority(priority);
  if (clamped >= 8) {
    return THREAD_PRIORITY_TIME_CRITICAL;
  }
  if (clamped >= 5) {
    return THREAD_PRIORITY_HIGHEST;
  }
  return THREAD_PRIORITY_ABOVE_NORMAL;
}

/**
 * Registers the calling thread with a permanent MMCSS task and raises it to the
 * critical MMCSS priority.
 *
 * A permanent task ("Pro Audio", falling back to "Audio") ignores the task
 * index, so only the handle is kept. The first registration in a process is
 * serialized because MMCSS has been observed to fail with
 * ERROR_PATH_NOT_FOUND when threads register concurrently while the service is
 * still starting up.
 */
bool promoteWindows(HandlePayload* out, const int32_t priority, DWORD* outLastError) noexcept {
  const MmcssApi& api = mmcss();

  // Captured before anything is changed, so demotion can restore it exactly.
  const int previousPriority = ::GetThreadPriority(::GetCurrentThread());
  out->previousThreadPriority =
      previousPriority == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : previousPriority;

  if (api.available()) {
    static std::atomic<bool> warmUpDone{ false };
    static std::mutex warmUpMutex;

    if (!warmUpDone.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(warmUpMutex);
      if (!warmUpDone.load(std::memory_order_relaxed)) {
        DWORD ignored = 0;
        if (HANDLE warmUp = api.setCharacteristics("Audio", &ignored)) {
          api.revertCharacteristics(warmUp);
        }
        warmUpDone.store(true, std::memory_order_release);
      }
    }

    DWORD taskIndex = 0;
    HANDLE task = api.setCharacteristics("Pro Audio", &taskIndex);
    if (!task) {
      task = api.setCharacteristics("Audio", &taskIndex);
    }
    if (task) {
      if (api.setMmPriority) {
        // "Pro Audio" accepts only the critical priority; "Audio" accepts
        // several, and critical is the right one for a rendering callback.
        api.setMmPriority(task, kAvrtPriorityCritical);
      }
      out->task = task;
      out->usedMmcss = true;
      return true;
    }
    *outLastError = ::GetLastError();
  }

  // No MMCSS: an elevated thread priority is the best option that remains.
  if (::SetThreadPriority(::GetCurrentThread(), windowsPriorityOf(priority))) {
    out->task = nullptr;
    out->usedMmcss = false;
    return true;
  }
  *outLastError = ::GetLastError();
  return false;
}

bool demoteWindows(const HandlePayload& payload) noexcept {
  // Restore the thread priority no matter what happens to the MMCSS task: a
  // failed revert would otherwise leave the thread at whatever priority MMCSS
  // had set, which can be far above what it had before.
  const bool priorityRestored =
      ::SetThreadPriority(::GetCurrentThread(), payload.previousThreadPriority) != 0;

  if (!payload.usedMmcss) {
    // The fallback path only changed the thread priority.
    return priorityRestored;
  }

  const MmcssApi& api = mmcss();
  if (!api.revertCharacteristics || !payload.task) {
    return false;
  }
  const bool taskReverted = api.revertCharacteristics(payload.task) != 0;

  // Both steps have to succeed for the promotion to be fully undone, but the
  // priority restore above is what keeps a partial failure from leaving the
  // thread elevated.
  return taskReverted && priorityRestored;
}

// =============================================================================
//  Apple: Mach time-constraint policy
// =============================================================================

#elif defined(CORAL_ATP_APPLE)

bool getApplePolicy(const mach_port_t thread, ApplePolicy* out) noexcept {
  ApplePolicy policy{};
  mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
  boolean_t getDefault = FALSE;
  const kern_return_t result = ::thread_policy_get(thread, THREAD_TIME_CONSTRAINT_POLICY,
      reinterpret_cast<thread_policy_t>(&policy), &count, &getDefault);
  if (result != KERN_SUCCESS) {
    return false;
  }
  *out = policy;
  return true;
}

bool setApplePolicy(const mach_port_t thread, const ApplePolicy& policy) noexcept {
  ApplePolicy copy = policy;
  const kern_return_t result = ::thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
      reinterpret_cast<thread_policy_t>(&copy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  return result == KERN_SUCCESS;
}

/**
 * Installs the Mach time-constraint policy, the one CoreAudio uses for its own
 * rendering threads.
 *
 * The period is one callback, the constraint matches the period, and the
 * computation budget is a fraction of it: macOS 12 and later expect computation
 * to be about half the constraint for an audio callback. A higher requested
 * priority tightens that budget.
 */
bool promoteApple(const uint32_t audioBufferFrames,
    const uint32_t audioSampleRateHz,
    const int32_t priority,
    HandlePayload* out) noexcept {
  const mach_port_t thread = ::pthread_mach_thread_np(::pthread_self());
  if (thread == MACH_PORT_NULL) {
    return false;
  }

  ApplePolicy previous{};
  if (!getApplePolicy(thread, &previous)) {
    return false;
  }
  out->previous = previous;

  const uint32_t frames = deriveBufferFrames(audioBufferFrames, audioSampleRateHz);
  const uint32_t rate = effectiveSampleRate(audioSampleRateHz);

  // The callback period, in milliseconds.
  const double periodMs = static_cast<double>(frames) / static_cast<double>(rate) * 1000.0;

  mach_timebase_info_data_t timebase{};
  if (::mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
    return false;
  }
  // Milliseconds to Mach absolute time units.
  const double msToAbsolute =
      (static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer)) * 1000000.0;

  // The policy fields are 32-bit, and `msToAbsolute` is about 1e6 on modern
  // hardware, so anything past roughly 4.29e9 units must be refused rather than
  // silently truncated into a nonsense period. That is a little over 1 hour of
  // audio at 48 kHz, so a realistic request never comes close.
  constexpr double kMaxAbsoluteUnits = 4294967295.0;
  const double periodUnits = periodMs * msToAbsolute;
  if (!(periodUnits > 0.0) || periodUnits > kMaxAbsoluteUnits) {
    return false;
  }

  const double budgetScale = AudioThreadPriority::computationBudgetFraction(priority);

  ApplePolicy policy{};
  policy.period = static_cast<uint32_t>(periodUnits);
  policy.computation = static_cast<uint32_t>(periodUnits * budgetScale);
  policy.constraint = static_cast<uint32_t>(periodUnits);
  policy.preemptible = 1;
  if (policy.period == 0 || policy.constraint == 0) {
    return false;
  }
  // A computation budget must stay inside the constraint, or the kernel rejects
  // the policy as unsatisfiable.
  if (policy.computation >= policy.constraint) {
    policy.computation = policy.constraint - 1;
  }

  return setApplePolicy(thread, policy);
}

bool demoteApple(const HandlePayload& payload) noexcept {
  const mach_port_t thread = ::pthread_mach_thread_np(::pthread_self());
  if (thread == MACH_PORT_NULL) {
    return false;
  }
  return setApplePolicy(thread, payload.previous);
}

// =============================================================================
//  Linux and Android: SCHED_FIFO
// =============================================================================

#elif defined(CORAL_ATP_POSIX)


bool getPosixSchedule(const pthread_t thread, int* outPolicy, sched_param* outParam) noexcept {
  int policy = SCHED_OTHER;
  sched_param param{};
  if (::pthread_getschedparam(thread, &policy, &param) != 0) {
    return false;
  }
  *outPolicy = policy;
  *outParam = param;
  return true;
}

bool setPosixSchedule(const pthread_t thread, const int policy, const int priority) noexcept {
  sched_param param{};
  param.sched_priority = priority;
  return ::pthread_setschedparam(thread, policy, &param) == 0;
}

/**
 * Raises the soft RLIMIT_RTPRIO to the hard limit.
 *
 * Without a non-zero limit an unprivileged process cannot use SCHED_FIFO at all,
 * so this is what makes real-time scheduling possible for a process that has
 * been granted a limit in its service definition or in limits.conf.
 *
 * @param maxPriority the priority the caller wants to be allowed to use.
 * @param outPrevious optional; receives the limit in force before this call.
 * @param outChanged optional; set to true when the call changed the soft limit.
 * @return `true` when the limit is at least `maxPriority` afterwards.
 */
bool raiseRtLimit(const int32_t maxPriority, rlimit* outPrevious = nullptr,
    bool* outChanged = nullptr) noexcept {
  if (outChanged) {
    *outChanged = false;
  }
  rlimit limit{};
  if (::getrlimit(RLIMIT_RTPRIO, &limit) != 0) {
    return false;
  }
  if (outPrevious) {
    *outPrevious = limit;
  }
  if (limit.rlim_cur >= static_cast<rlim_t>(maxPriority)) {
    return true;
  }
  if (limit.rlim_max == 0) {
    // No real-time priority is allowed for this process.
    return false;
  }
  limit.rlim_cur = limit.rlim_max;
  if (::setrlimit(RLIMIT_RTPRIO, &limit) != 0) {
    return false;
  }
  if (outChanged) {
    *outChanged = true;
  }
  return limit.rlim_cur >= static_cast<rlim_t>(maxPriority);
}

/**
 * Maps the platform-independent priority onto the SCHED_FIFO band.
 *
 * The very top of the band is left unused so a stream that asks for the maximum
 * still has room below a higher-priority stream in the same process.
 */
int posixPriorityOf(const int32_t priority) noexcept {
  const int lowest = std::max(1, ::sched_get_priority_min(SCHED_FIFO));
  const int highest = std::max(lowest, ::sched_get_priority_max(SCHED_FIFO));
  if (highest <= lowest) {
    return lowest;
  }
  const int top = std::max(lowest + 1, highest - 1);
  const auto clamped = static_cast<double>(detail::clampPriority(priority));
  const double fraction = clamped / static_cast<double>(AudioThreadPriority::kMaxRealTimePriority);
  return lowest + static_cast<int>(fraction * static_cast<double>(top - lowest) + 0.5);
}

/**
 * Clamps a real-time priority read back from the kernel into the band that
 * `SCHED_FIFO` accepts, so restoring a captured priority is always legal.
 */
int posixPriorityClamped(const int realTimePriority) noexcept {
  const int lowest = std::max(1, ::sched_get_priority_min(SCHED_FIFO));
  const int highest = std::max(lowest, ::sched_get_priority_max(SCHED_FIFO));
  if (realTimePriority < lowest) {
    return lowest;
  }
  return std::min(realTimePriority, highest);
}

/**
 * Promotes the calling thread to SCHED_FIFO.
 *
 * The real-time limit is raised only as a second resort, and only when the
 * process is actually going to be given SCHED_FIFO: `RLIMIT_RTPRIO` is
 * process-wide and never comes back down by itself, so a failed attempt must
 * not leave it permanently raised as a side effect. If raising the limit and
 * retrying the policy both fail, the previous soft limit is restored. Trying the
 * policy first also keeps a process that holds `CAP_SYS_NICE` working even with
 * a zero limit.
 *
 * On Android SCHED_FIFO is normally refused to an ordinary app, so a raised nice
 * value is used as a fallback instead: the thread then still runs ahead of normal
 * work, just without a deadline guarantee.
 */
bool promotePosix(const int32_t priority, HandlePayload& out) noexcept {
  const pthread_t self = ::pthread_self();

  // `errno` is cleared before each failing call below, because none of
  // pthread_getschedparam, pthread_setschedparam, getrlimit or setrlimit is
  // required to set it on failure. Without clearing it the reported code could
  // be a leftover from an unrelated call.
  errno = 0;
  if (!getPosixSchedule(self, &out.previousPolicy, &out.previousParam)) {
    out.lastErrno = errno;
    return false;
  }
  out.niceFallback = false;
  out.previousNice = 0;
  out.lastErrno = 0;

  const int band = posixPriorityOf(priority);

  errno = 0;
  if (setPosixSchedule(self, SCHED_FIFO, band)) {
    return true;
  }
  const int directErrno = errno;

  // Refused: a non-zero RLIMIT_RTPRIO is what an unprivileged process needs, so
  // raise it if the hard limit allows it and try once more. If that also fails,
  // put the soft limit back: a failed promotion must not leave a side effect
  // behind just because the retry was attempted.
  rlimit previousLimit{};
  bool limitChanged = false;
  errno = 0;
  const bool limitRaised = raiseRtLimit(band, &previousLimit, &limitChanged);
  const int limitErrno = errno;
  int retryErrno = 0;
  if (limitRaised) {
    errno = 0;
    if (setPosixSchedule(self, SCHED_FIFO, band)) {
      return true;
    }
    retryErrno = errno;
  }
  if (limitChanged) {
    // Best effort: if this fails there is nothing safer to fall back to, but a
    // successful restore keeps the documented "no side effect" contract.
    errno = 0;
    (void) ::setrlimit(RLIMIT_RTPRIO, &previousLimit);
  }
  out.lastErrno = directErrno != 0 ? directErrno : (limitRaised ? retryErrno : limitErrno);

#if defined(CORAL_ATP_ANDROID)
  // Capture the nice value first so demotion can put it back rather than
  // guessing at zero.
  errno = 0;
  const int currentNice = ::getpriority(PRIO_PROCESS, 0);
  out.previousNice = (currentNice == -1 && errno != 0) ? 0 : currentNice;

  errno = 0;
  if (::setpriority(PRIO_PROCESS, 0, -16) == 0) {
    out.niceFallback = true;
    return true;
  }
  out.lastErrno = errno;
#endif

  return false;
}

bool demotePosix(const HandlePayload& payload) noexcept {
  const pthread_t self = ::pthread_self();
  // Clear `errno` for the same reason the promotion path does: neither call is
  // required to set it on failure, so a leftover value must not be reported as
  // the reason this failed.
  errno = 0;
  const bool policyRestored =
      setPosixSchedule(self, payload.previousPolicy, payload.previousParam.sched_priority);

  if (!payload.niceFallback) {
    return policyRestored;
  }

  // Undo the nice value the fallback promotion applied, restoring what it was
  // instead of assuming a default.
  errno = 0;
  const bool niceRestored = ::setpriority(PRIO_PROCESS, 0, payload.previousNice) == 0;
  return policyRestored && niceRestored;
}

#if defined(CORAL_ATP_ANDROID)
/** The Linux thread id, which is what an out-of-thread promotion needs. */
pid_t currentThreadId() noexcept {
#if defined(__NR_gettid)
  return static_cast<pid_t>(::syscall(__NR_gettid));
#else
  return ::gettid();
#endif
}

/**
 * Maps a `/proc/<tid>/stat` field-41 scheduling policy value onto the
 * `SCHED_*` constants.
 *
 * The procfs value is the kernel's own enumeration, which is not required to
 * match the libc constants, so it is translated explicitly.
 */
int policyFromProcfs(const long value) noexcept {
  switch (value) {
    case 0: return SCHED_OTHER;
    case 1: return SCHED_FIFO;
    case 2: return SCHED_RR;
#if defined(SCHED_BATCH)
    case 3: return SCHED_BATCH;
#endif
    default: return SCHED_OTHER;
  }
}

/**
 * Reads another thread's scheduling state.
 *
 * `sched_getscheduler` only answers for the calling thread, so a thread id is
 * resolved through `/proc/<tid>/stat`, whose fields carry everything needed to
 * put the thread back exactly as it was:
 *
 *  - field 19: the nice value;
 *  - field 40: the real-time priority;
 *  - field 41: the scheduling policy.
 *
 * @param outNice optional; receives the thread's nice value, which a scheduler
 *                parameter does not carry.
 * @return `false` when the state cannot be determined, so the caller can refuse
 *         instead of overwriting something it cannot put back.
 */
bool readThreadSchedule(const pid_t tid, int* outPolicy, sched_param* outParam, int* outNice = nullptr) noexcept {
  if (!outPolicy || !outParam || tid <= 0) {
    return false;
  }

  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(tid));

  FILE* file = ::fopen(path, "r");
  if (!file) {
    return false;
  }
  char line[1024]{};
  const size_t read = ::fread(line, 1, sizeof(line) - 1, file);
  ::fclose(file);
  if (read == 0) {
    return false;
  }
  line[read] = '\0';

  // The command name is parenthesised and may itself contain spaces, so start
  // counting after the closing parenthesis. The field right after it is the
  // process state (field 3), which is a single letter, not a number; consume it
  // before parsing the numeric fields that start with ppid (field 4).
  const char* cursor = std::strrchr(line, ')');
  if (!cursor) {
    return false;
  }
  ++cursor;
  while (*cursor == ' ' || *cursor == '\t') {
    ++cursor;
  }
  if (*cursor == '\0' || *cursor == '\n') {
    return false;
  }
  while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\n') {
    ++cursor;
  }

  int niceValue = 0;
  int realTimePriority = 0;
  long policyValue = 0;
  bool haveNice = false;
  bool haveRealTime = false;
  bool havePolicy = false;
  int field = 4;

  while (*cursor != '\0' && field <= 41) {
    while (*cursor == ' ' || *cursor == '\t') {
      ++cursor;
    }
    if (*cursor == '\0' || *cursor == '\n') {
      break;
    }

    // Parse one signed decimal field in place.
    char* end = nullptr;
    const long value = std::strtol(cursor, &end, 10);
    if (end == cursor) {
      break;
    }
    cursor = end;

    switch (field) {
      case 19:
        niceValue = static_cast<int>(value);
        haveNice = true;
        break;
      case 40:
        realTimePriority = static_cast<int>(value);
        haveRealTime = true;
        break;
      case 41:
        policyValue = value;
        havePolicy = true;
        break;
      default:
        break;
    }
    ++field;
  }

  if (!haveNice || !haveRealTime || !havePolicy) {
    return false;
  }

  // Restore the policy the thread actually had, and its real-time priority.
  // Real-time policies carry the priority in the scheduler parameter; the
  // ordinary ones leave it at zero and express their weighting through nice.
  sched_param param{};
  if (policyValue == 1 || policyValue == 2) {
    // Clamped into the band SCHED_FIFO/SCHED_RR accept, so a value read back
    // from the kernel is always a legal one to restore.
    param.sched_priority = posixPriorityClamped(realTimePriority);
  } else {
    param.sched_priority = 0;
  }
  *outPolicy = policyFromProcfs(policyValue);
  *outParam = param;
  if (outNice) {
    *outNice = niceValue;
  }
  return true;
}
#endif

#endif // platform families


} // namespace anonymous

// =============================================================================
//  LastError
// =============================================================================

std::string AudioThreadPriority::LastError::toString() const {
  // The code is already part of the message: setError renders it once, in one
  // format. Appending it here as well is what used to produce
  // "... (code -2) [code -2]", two spellings of the same value.
  //
  // This is the only place an error allocates, and it runs when a caller is
  // about to report a failure, never on the promotion path itself.
  if (message[0] != '\0') {
    return std::string(message);
  }
  return code != 0 ? std::string("failure with no description") : std::string("no error");
}

// =============================================================================
//  ThreadInfo
// =============================================================================

std::optional<AudioThreadPriority::ThreadInfo> AudioThreadPriority::ThreadInfo::current() noexcept {
  ThreadInfo info;

#if defined(CORAL_ATP_WINDOWS)
  info.mThreadId = static_cast<uint64_t>(::GetCurrentThreadId());
  info.mHandle = reinterpret_cast<void*>(::GetCurrentThread());
  info.mValid = true;
#elif defined(CORAL_ATP_APPLE)
  // The handle stores the Mach thread port, because that is what
  // thread_policy_get/set operate on; the pthread_t is kept as the thread id.
  info.mHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(::pthread_mach_thread_np(::pthread_self())));
  info.mThreadId = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(::pthread_self()));
  info.mValid = info.mHandle != nullptr;
#elif defined(CORAL_ATP_ANDROID)
  info.mThreadId = static_cast<uint64_t>(currentThreadId());
  info.mHandle = reinterpret_cast<void*>(::pthread_self());
  info.mValid = true;
#elif defined(CORAL_ATP_POSIX)
  const pthread_t self = ::pthread_self();
  info.mHandle = reinterpret_cast<void*>(self);
  info.mThreadId = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(self));
  info.mValid = true;
#else
  info.mValid = false;
#endif

  if (!info.mValid) {
    return std::nullopt;
  }
  return info;
}

bool AudioThreadPriority::ThreadInfo::getCurrentPriority(int32_t* outPriority) const noexcept {
  if (!outPriority || !mValid) {
    return false;
  }

#if defined(CORAL_ATP_WINDOWS)
  const int priority = ::GetThreadPriority(reinterpret_cast<HANDLE>(mHandle));
  if (priority == THREAD_PRIORITY_ERROR_RETURN) {
    return false;
  }
  // THREAD_PRIORITY_NORMAL means ordinary scheduling, which the scale calls 0.
  // The five Win32 levels at or above ABOVE_NORMAL spread over the rest.
  *outPriority = scalePriority(priority, THREAD_PRIORITY_NORMAL);
  return true;
#elif defined(CORAL_ATP_APPLE)
  // Query the described thread through its Mach port, not the calling thread.
  const auto thread = static_cast<mach_port_t>(reinterpret_cast<uintptr_t>(mHandle));
  ApplePolicy policy{};
  if (thread == MACH_PORT_NULL || !getApplePolicy(thread, &policy)) {
    return false;
  }
  if (policy.period == 0 || policy.computation == 0) {
    // No time-constraint policy is installed, so the thread is not real-time.
    *outPriority = kMinRealTimePriority;
    return true;
  }
  // Invert the budget the promotion installed, so a promoted thread reports the
  // priority that was requested for it.
  const double ratio = static_cast<double>(policy.computation) / static_cast<double>(policy.period);
  constexpr double kLowestFraction = 0.25;
  constexpr double kHighestFraction = 0.5;
  const double fraction = (ratio - kLowestFraction) / (kHighestFraction - kLowestFraction);
  const auto scaled = static_cast<int32_t>(fraction * static_cast<double>(kMaxRealTimePriority) + 0.5);
  *outPriority = detail::clampPriority(scaled);
  return true;
#elif defined(CORAL_ATP_POSIX)
  // Query the described thread rather than the calling thread.
  const auto thread = static_cast<pthread_t>(reinterpret_cast<uintptr_t>(mHandle));
  int policy = SCHED_OTHER;
  sched_param param{};
  if (::pthread_getschedparam(thread, &policy, &param) != 0) {
    return false;
  }
  if (policy != SCHED_FIFO && policy != SCHED_RR) {
    // SCHED_OTHER means ordinary scheduling, which the scale calls 0, rather
    // than the bottom of the real-time range.
    *outPriority = kMinRealTimePriority;
    return true;
  }
  const int lowest = std::max(1, ::sched_get_priority_min(SCHED_FIFO));
  const int highest = std::max(lowest, ::sched_get_priority_max(SCHED_FIFO));
  if (highest <= lowest) {
    *outPriority = kMaxRealTimePriority;
    return true;
  }
  // The promotion leaves the very top of the band unused, so the scale runs to
  // the same ceiling it was mapped from.
  const int top = std::max(lowest + 1, highest - 1);
  const double fraction = static_cast<double>(param.sched_priority - lowest) /
                          static_cast<double>(top - lowest);
  *outPriority = detail::clampPriority(
      static_cast<int32_t>(fraction * static_cast<double>(kMaxRealTimePriority) + 0.5));
  return true;
#else
  *outPriority = kMinRealTimePriority;
  return true;
#endif
}

// =============================================================================
//  Promotion and demotion
// =============================================================================

AudioThreadPriority::Handle AudioThreadPriority::promoteCurrentThread(const uint32_t audioBufferFrames,
    const uint32_t audioSampleRateHz,
    const int32_t priority,
    LastError* outError) noexcept {
  if (outError) {
    *outError = LastError{};
  }

  Handle handle;
  const int32_t clamped = detail::clampPriority(priority);

  // Record where the thread is starting from *before* touching it, so
  // previousPriority() describes the state the promotion replaced.
  if (const auto info = ThreadInfo::current()) {
    int32_t previous = 0;
    if (info->getCurrentPriority(&previous)) {
      handle.mPreviousPriority = previous;
    }
  }

#if defined(CORAL_ATP_WINDOWS)
  DWORD lastError = 0;
  if (!promoteWindows(&payloadOf(handle), clamped, &lastError)) {
    setError(outError, kNotPermitted, "MMCSS promotion failed", lastError);
    return handle;
  }
  handle.mNativeHandle = payloadOf(handle).task;
  handle.mValid = true;

#elif defined(CORAL_ATP_APPLE)
  if (!promoteApple(audioBufferFrames, audioSampleRateHz, clamped, &payloadOf(handle))) {
    setError(outError, kNotPermitted, "Mach time-constraint promotion failed");
    return handle;
  }
  handle.mNativeHandle = &payloadOf(handle);
  handle.mValid = true;

#elif defined(CORAL_ATP_POSIX)
  (void) audioBufferFrames;
  if (!promotePosix(clamped, payloadOf(handle))) {
    // The payload carries the `errno` the failing call actually set, so a stale
    // value from an unrelated call is never reported.
    const int64_t platformCode = payloadOf(handle).lastErrno;
    setError(outError, kNotPermitted,
        "SCHED_FIFO promotion failed; the process needs root, CAP_SYS_NICE, or a non-zero RLIMIT_RTPRIO",
        platformCode);
    return handle;
  }
  handle.mNativeHandle = &payloadOf(handle);
  handle.mValid = true;

#else
  (void) audioBufferFrames;
  (void) audioSampleRateHz;
  (void) clamped;
  handle.mValid = true;
#endif

  return handle;
}

AudioThreadPriority::Handle AudioThreadPriority::promoteThread(const ThreadInfo& info,
    const uint32_t audioBufferFrames,
    const uint32_t audioSampleRateHz,
    const int32_t priority,
    LastError* outError) noexcept {
  if (outError) {
    *outError = LastError{};
  }

  Handle handle;
  if (!info.isValid()) {
    setError(outError, kNotSupported, "promoteThread: the thread info is not valid");
    return handle;
  }

#if defined(CORAL_ATP_ANDROID)
  const auto tid = static_cast<pid_t>(info.nativeThreadId());

  // Capture what the target thread is doing now, so demoteThread can put it
  // back. Refusing beats overwriting a state that cannot be restored.
  HandlePayload& payload = payloadOf(handle);
  if (!readThreadSchedule(tid, &payload.previousPolicy, &payload.previousParam, &payload.previousNice)) {
    setError(outError, kNotPermitted,
        "promoteThread: the target thread's scheduling state could not be read, so it could not be restored");
    return handle;
  }

  const int band = posixPriorityOf(detail::clampPriority(priority));
  rlimit previousLimit{};
  bool limitChanged = false;
  errno = 0;
  (void) raiseRtLimit(band, &previousLimit, &limitChanged);

  sched_param param{};
  param.sched_priority = band;
  errno = 0;
  if (::sched_setscheduler(tid, SCHED_FIFO, &param) != 0) {
    const int schedErrno = errno;
    if (limitChanged) {
      // The promotion did not happen, so the limit raised for it must not stay
      // behind. Keep the scheduler error, not the error from this rollback.
      errno = 0;
      (void) ::setrlimit(RLIMIT_RTPRIO, &previousLimit);
    }
    setError(outError, kNotPermitted,
        "promoteThread: sched_setscheduler failed for the target thread",
        schedErrno != 0 ? schedErrno : kNotPermitted);
    return handle;
  }
  handle.mValid = true;
  (void) audioBufferFrames;
  (void) audioSampleRateHz;
#else
  (void) audioBufferFrames;
  (void) audioSampleRateHz;
  (void) priority;
  setError(outError, kNotSupported,
      "promoteThread: promoting another thread is only supported on Android; promote from the audio thread itself");
#endif

  return handle;
}

bool AudioThreadPriority::demoteCurrentThread(const Handle& handle, LastError* outError) noexcept {
  if (outError) {
    *outError = LastError{};
  }
  if (!handle.isValid()) {
    setError(outError, kNotSupported, "demoteCurrentThread: the handle is not valid");
    return false;
  }

#if defined(CORAL_ATP_WINDOWS)
  if (!demoteWindows(payloadOf(handle))) {
    setError(outError, kNotPermitted, "MMCSS demotion failed", ::GetLastError());
    return false;
  }
  return true;

#elif defined(CORAL_ATP_APPLE)
  if (!demoteApple(payloadOf(handle))) {
    setError(outError, kNotPermitted, "Mach time-constraint demotion failed");
    return false;
  }
  return true;

#elif defined(CORAL_ATP_POSIX)
  if (!demotePosix(payloadOf(handle))) {
    // `errno` is cleared by demotePosix before it touches the scheduler, so only
    // a value the failing call actually set is reported.
    setError(outError, kNotPermitted, "restoring the scheduler settings failed",
        posixErrorCode(kNotPermitted));
    return false;
  }
  return true;

#else
  return true;
#endif
}

bool AudioThreadPriority::demoteThread(const ThreadInfo& info, const Handle& handle, LastError* outError) noexcept {
  if (outError) {
    *outError = LastError{};
  }
  if (!info.isValid() || !handle.isValid()) {
    setError(outError, kNotSupported, "demoteThread: the thread info or handle is not valid");
    return false;
  }

#if defined(CORAL_ATP_ANDROID)
  // Restore what the target thread had before it was promoted, rather than
  // assuming SCHED_OTHER. Two independent steps, reported separately so a
  // failure names the call that failed: the policy and priority were captured by
  // promoteThread, and the nice value is restored on its own because a scheduler
  // parameter does not carry it.
  const HandlePayload& payload = payloadOf(handle);
  const auto tid = static_cast<pid_t>(info.nativeThreadId());

  sched_param param = payload.previousParam;
  errno = 0;
  const bool policyRestored = ::sched_setscheduler(tid, payload.previousPolicy, &param) == 0;
  const int policyErrno = errno;

  errno = 0;
  const bool niceRestored = ::setpriority(PRIO_PROCESS, static_cast<id_t>(tid), payload.previousNice) == 0;
  const int niceErrno = errno;

  if (!policyRestored || !niceRestored) {
    // Name the step (or steps) that failed, with that call's own code, so a
    // partial restore is not reported as a generic failure.
    const char* what = nullptr;
    int64_t code = 0;
    if (!policyRestored && !niceRestored) {
      what = "demoteThread: restoring the target thread's scheduling policy failed"
             " and restoring its nice value also failed";
      code = policyErrno != 0 ? policyErrno : niceErrno;
    } else if (!policyRestored) {
      what = "demoteThread: restoring the target thread's scheduling policy failed"
             " (the nice value was restored)";
      code = policyErrno;
    } else {
      what = "demoteThread: the target thread's scheduling policy was restored"
             " but restoring its nice value failed";
      code = niceErrno;
    }
    setError(outError, kNotPermitted, what, code != 0 ? code : kNotPermitted);
    return false;
  }
  return true;
#else
  setError(outError, kNotSupported,
      "demoteThread: demoting another thread is only supported on Android; demote from the audio thread itself");
  return false;
#endif
}

bool AudioThreadPriority::setRealTimeLimit(const int32_t maxPriority, LastError* outError) noexcept {
  if (outError) {
    *outError = LastError{};
  }
  const int32_t clamped = detail::clampPriority(maxPriority);

#if defined(CORAL_ATP_POSIX)
  const int band = posixPriorityOf(clamped);
  errno = 0;
  if (!raiseRtLimit(band)) {
    setError(outError, kNotPermitted,
        "setRealTimeLimit: raising RLIMIT_RTPRIO failed; the process is not allowed to raise its real-time limit",
        posixErrorCode(kNotPermitted));
    return false;
  }
  return band <= ::sched_get_priority_max(SCHED_FIFO);
#elif defined(CORAL_ATP_WINDOWS)
  (void) clamped;
  // MMCSS hands out its share per registered task, not through a process limit.
  return true;
#elif defined(CORAL_ATP_APPLE)
  (void) clamped;
  // The time-constraint policy is not subject to a per-process limit.
  return true;
#else
  (void) clamped;
  return true;
#endif
}

const char* AudioThreadPriority::backendName() noexcept {
#if defined(CORAL_ATP_WINDOWS)
  return mmcss().available() ? "Windows MMCSS" : "Windows thread priority";
#elif defined(CORAL_ATP_APPLE)
  return "Mach time-constraint policy";
#elif defined(CORAL_ATP_ANDROID)
  return "Android SCHED_FIFO";
#elif defined(CORAL_ATP_POSIX)
  return "Linux SCHED_FIFO";
#else
  return "no-op";
#endif
}

uint32_t AudioThreadPriority::derivedBufferFrames(const uint32_t audioSampleRateHz) noexcept {
  return derivedBufferFramesFor(audioSampleRateHz);
}

double AudioThreadPriority::computationBudgetFraction(const int32_t priority) noexcept {  // The same curve every backend uses, so the reported budget matches what a
  // promotion will actually ask the platform for.
  constexpr double kLowestFraction = 0.25;
  constexpr double kHighestFraction = 0.5;
  const double clamped = static_cast<double>(detail::clampPriority(priority));
  const double fraction = clamped / static_cast<double>(kMaxRealTimePriority);
  const double budget = kLowestFraction + fraction * (kHighestFraction - kLowestFraction);
  // A thread can never be promised more than the whole period.
  return std::clamp(budget, kLowestFraction, kHighestFraction);
}

// =============================================================================
//  RealTimeScope
// =============================================================================

RealTimeScope::RealTimeScope(const uint32_t audioBufferFrames,
    const uint32_t audioSampleRateHz,
    const int32_t priority) noexcept {
  mHandle = AudioThreadPriority::promoteCurrentThread(audioBufferFrames, audioSampleRateHz, priority, &mError);
  mPreviousPriority = mHandle.isValid() ? mHandle.previousPriority() : 0;
}

RealTimeScope::~RealTimeScope() {
  // By the time this runs the object is going away, so a refused restore is
  // beyond recovery: there is no one left to retry with and no way to read
  // mError afterwards. A caller that needs to handle a failed demotion must
  // call demote() itself while the scope is still alive. See the class
  // documentation for that contract.
  (void) demote();
}

RealTimeScope::RealTimeScope(RealTimeScope&& other) noexcept
  : mHandle(other.mHandle),
    mError(other.mError),
    mPreviousPriority(other.mPreviousPriority) {
  other.mHandle = AudioThreadPriority::Handle{};
  other.mPreviousPriority = 0;
  other.mError = AudioThreadPriority::LastError{};
}

RealTimeScope& RealTimeScope::operator=(RealTimeScope&& other) noexcept {
  if (this != &other) {
    // Give up whatever this scope was holding. Unlike a plain demote() call,
    // a refusal here cannot be reported to anyone: the assignment has to
    // overwrite both the handle and the error, and the temporary being assigned
    // from is what the caller is holding. A failed restore of the old state is
    // therefore lost, exactly as it would be in the destructor. Callers that need
    // to handle it must call demote() and check the result before assigning.
    (void) demote();

    mHandle = other.mHandle;
    mError = other.mError;
    mPreviousPriority = other.mPreviousPriority;

    other.mHandle = AudioThreadPriority::Handle{};
    other.mPreviousPriority = 0;
    other.mError = AudioThreadPriority::LastError{};
  }
  return *this;
}

bool RealTimeScope::demote() noexcept {
  if (!mHandle.isValid()) {
    return false;
  }
  const bool restored = AudioThreadPriority::demoteCurrentThread(mHandle, &mError);
  if (restored) {
    // Only a completed demotion invalidates the handle, and it clears the
    // remembered priority with it: there is no "previous" state to describe once
    // the thread is back to where it started.
    mHandle = AudioThreadPriority::Handle{};
    mPreviousPriority = 0;
    return true;
  }
  // The platform refused to restore the previous state. Keeping the handle means
  // the caller can retry, or hand it to AudioThreadPriority::demoteCurrentThread
  // explicitly, instead of being left with a thread stuck at a priority it never
  // asked for and no way to say so. mError explains what failed.
  return false;
}

} // namespace coral
