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

#ifndef CORAL_THREAD_AUDIO_THREAD_PRIORITY_H
#define CORAL_THREAD_AUDIO_THREAD_PRIORITY_H

#include <utils/compiler.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace coral {

/**
 * Real-time priority management for audio rendering threads.
 *
 * Audio callbacks must run at a stable, low-latency priority: a render callback
 * that misses its deadline produces a dropout, and on some systems a thread that
 * blocks or is descheduled can stall other audio work in the same process. Every
 * supported platform expresses that requirement differently, and this class
 * hides the differences behind one interface.
 *
 * Per platform:
 *
 *  - **Windows**: the Multimedia Class Scheduler Service (MMCSS). The thread is
 *    registered with the "Pro Audio" task (falling back to "Audio"), which is a
 *    permanent MMCSS task, and then raised to the critical MMCSS priority. MMCSS
 *    guarantees the thread a share of CPU time and keeps the system timer
 *    resolution high. `avrt.dll` is loaded dynamically, so a machine without the
 *    service degrades to `SetThreadPriority` instead of failing to start.
 *  - **Linux**: `SCHED_FIFO` through `pthread_setschedparam`. That normally needs
 *    root, `CAP_SYS_NICE`, or a non-zero `RLIMIT_RTPRIO`; the soft limit is
 *    raised to the hard limit first when the process is allowed to. The default
 *    request is 6 (about priority 59 of the `SCHED_FIFO` band), not the top of
 *    the band: an audio I/O thread does not need to outrank hard real-time work.
 *    Callers that do need the top can pass 10 explicitly. When real-time
 *    scheduling is not permitted the caller keeps a plain thread, and the request
 *    is reported through LastError.
 *  - **Apple**: the Mach time-constraint policy (`THREAD_TIME_CONSTRAINT_POLICY`),
 *    which is what CoreAudio's own rendering threads use. The previous policy is
 *    saved and restored on demotion.
 *  - **Android**: `SCHED_FIFO` for the audio thread, with a `setpriority`
 *    fallback so the thread is at least scheduled ahead of normal work when
 *    real-time scheduling is refused.
 *  - **Other platforms**: a no-op that reports success, so callers need no
 *    platform conditionals.
 *
 * Threading rules, inherited from the platform APIs:
 *
 *  - Promotion and demotion are **thread affine**: demote from the same thread
 *    that was promoted. The one exception is `promoteCurrentThread()` on Android
 *    when a `ThreadInfo` captured by another thread is passed in.
 *  - A `ThreadInfo` stays valid while its owning thread is alive. Do not use one
 *    after the thread it describes has exited.
 *  - Promotion and demotion are cheap but not free; promote once per audio
 *    thread rather than once per callback.
 */
class AudioThreadPriority final {
public:
  /**
   * Requested real-time priority, 0 being the lowest usable real-time priority
   * and 10 the highest.
   *
   * This is a platform-independent scale. It is mapped onto whatever the
   * platform provides: the MMCSS priority on Windows, the `SCHED_FIFO` priority
   * range on Linux and Android, and the time-constraint arithmetic on Apple.
   */
  static constexpr int32_t kMinRealTimePriority = 0;
  static constexpr int32_t kMaxRealTimePriority = 10;

  /**
   * The priority used when the caller does not ask for a specific one.
   *
   * On Linux this is 6, which maps to roughly 59 of the `SCHED_FIFO` band on a
   * normal system. The top of the band (60-98) is left for work that genuinely
   * needs hard real time; an audio I/O thread that can block in ALSA does not.
   * Pass 10 explicitly when the top is actually required. Other platforms keep
   * 10 as their default.
   */
#if defined(CORAL_BUILD_FOR_LINUX)
  static constexpr int32_t kDefaultRealTimePriority = 6;
#else
  static constexpr int32_t kDefaultRealTimePriority = 10;
#endif

  /** Default audio callback size used when `audioBufferFrames` is 0. */
  static constexpr uint32_t kDefaultBufferFrames = 0;

  /**
   * A platform-specific error, with the value that produced it.
   *
   * `code` is an `errno` value, a Win32 error code, or a Mach `kern_return_t`
   * when the failure came from the platform, and one of this class's own codes
   * (`kNotSupported`, `kNotPermitted`) when the platform had nothing to report.
   * `codeFromPlatform()` tells the two apart, so a caller that switches on `code`
   * knows whether it is looking at an OS value or a library one.
   *
   * The description is stored in a fixed-size buffer rather than a
   * `std::string`, so that reporting a failure never allocates. That is what
   * lets the promotion and demotion calls stay `noexcept` while still being
   * usable from an audio thread, where an allocation on the failure path could
   * turn into `std::terminate` through a `noexcept` boundary. Only `toString()`,
   * which a caller uses to print a diagnostic after the fact, allocates.
   *
   * The code is part of the text (`what()`), so a single logged line carries
   * everything; `toString()` renders that same text and does not append the code
   * a second time.
   */
  struct LastError {
    /** The longest description this type stores, including the terminator. */
    static constexpr size_t kMessageCapacity = 256;

    /** `codeKind` when the failure came from the platform itself. */
    static constexpr int8_t kCodeFromPlatform = 0;

    /** `codeKind` when the platform reported nothing usable and this library supplied the code. */
    static constexpr int8_t kCodeFromLibrary = 1;

    /** The platform or library code, or 0 when the failure carries none. */
    int64_t code = 0;
    /** Which of the two `code` is; see `codeFromPlatform()`. */
    int8_t codeKind = kCodeFromPlatform;
    /** A human readable description of what failed, always terminated. */
    char message[kMessageCapacity]{};

    [[nodiscard]] bool empty() const noexcept {
      return code == 0 && message[0] == '\0';
    }

    /** Whether `code` is a value the platform reported. */
    [[nodiscard]] bool codeFromPlatform() const noexcept {
      return codeKind == kCodeFromPlatform;
    }

    /** The description, including the code when there is one, for logging. */
    [[nodiscard]] const char* what() const noexcept {
      return message;
    }

    /** The description as a `std::string`. The code is already inside it. Allocates. */
    [[nodiscard]] std::string toString() const;
  };

  /**
   * Identifies a thread that can later be promoted, possibly from another thread
   * or another process.
   *
   * Opaque on purpose: its contents are a `pthread_t` on POSIX, a Mach thread
   * port on Apple, and a Win32 thread id on Windows.
   */
  class ThreadInfo final {
  public:
    ThreadInfo() noexcept = default;

    /**
     * Captures the calling thread.
     *
     * @return the captured information, or `std::nullopt` when the platform
     *         could not describe the calling thread.
     */
    [[nodiscard]] static std::optional<ThreadInfo> current() noexcept;

    /** Whether this describes a thread. */
    [[nodiscard]] bool isValid() const noexcept {
      return mValid;
    }

    /**
     * Reports where the thread this instance describes sits on the same
     * platform-independent scale that `promoteCurrentThread()` accepts.
     *
     * The query targets the **described** thread, not necessarily the calling
     * one, and the contract is identical on every platform:
     *
     *  - the reported value is in [kMinRealTimePriority, kMaxRealTimePriority];
     *  - `kMinRealTimePriority` (0) means the thread is **not** at a real-time
     *    priority: ordinary scheduling, no time-constraint policy, no MMCSS
     *    task;
     *  - a reported value that is at least the requested one means the requested
     *    priority is in force, so a promotion can be verified by comparing the
     *    two;
     *  - a promotion and the matching demotion round-trip the value on platforms
     *    where the platform can express the requested level exactly.
     *
     * @param outPriority receives the priority. Unchanged when this returns
     *                    `false`.
     * @return `false` when the platform cannot report that thread's priority or
     *         the described thread is gone, in which case the caller learns
     *         nothing and must not assume a value.
     *
     * @discussion
     * The described thread must still be alive: querying a thread that has
     * exited is undefined on the platforms that report the real-time policy by
     * thread handle (POSIX and Apple). A `ThreadInfo` captured by the calling
     * thread itself is therefore always safe to query.
     */
    [[nodiscard]] bool getCurrentPriority(int32_t* outPriority) const noexcept;

    /** Platform handle, for FFI. Do not interpret. */
    [[nodiscard]] const void* nativeHandle() const noexcept {
      return mHandle;
    }

    /** Platform thread id, for FFI. Do not interpret. */
    [[nodiscard]] uint64_t nativeThreadId() const noexcept {
      return mThreadId;
    }

  private:
    friend class AudioThreadPriority;
    friend class RealTimeScope;

    void* mHandle = nullptr;
    uint64_t mThreadId = 0;
    bool mValid = false;
  };

  /**
   * The state a promotion produced, which demotion needs to undo it.
   *
   * Opaque; only `AudioThreadPriority` and `RealTimeScope` use it. The
   * platform-specific payload is stored inline in `mReserved`, so promoting and
   * demoting a thread never allocates.
   */
  class Handle final {
  public:
    Handle() noexcept = default;

    [[nodiscard]] bool isValid() const noexcept {
      return mValid;
    }

    /** The priority the thread had before it was promoted. */
    [[nodiscard]] int32_t previousPriority() const noexcept {
      return mPreviousPriority;
    }

    /**
     * Reserved storage for the platform's promotion state.
     *
     * The implementation stores its payload here, so promoting and demoting a
     * thread never allocates. The size is checked against that payload by a
     * `static_assert` in the implementation. Treat the contents as opaque.
     */
    [[nodiscard]] void* reservedStorage() noexcept {
      return mReserved;
    }

    /** Reserved storage, read-only. */
    [[nodiscard]] const void* reservedStorage() const noexcept {
      return mReserved;
    }

  private:
    friend class AudioThreadPriority;
    friend class RealTimeScope;

    /** A platform handle, meaningful for the payload the platform stored. */
    void* mNativeHandle = nullptr;
    /** Platform payload; `static_assert`ed against the platform's size. */
    alignas(std::max_align_t) unsigned char mReserved[32]{};
    int32_t mPreviousPriority = 0;
    bool mValid = false;
  };

  AudioThreadPriority() = delete;

  /**
   * Promotes the calling thread to real-time priority for audio work.
   *
   * @param audioBufferFrames the exact (or an upper bound on the) number of
   *                          frames rendered per callback, or 0 to derive a
   *                          sensible value from the sample rate. Used by the
   *                          Apple time-constraint policy.
   * @param audioSampleRateHz the stream's sample rate in Hz, or 0 to use a
   *                          default. Used by the Apple policy and by Android to
   *                          pick a scheduling priority band.
   * @param priority          the requested priority, clamped to
   *                          [kMinRealTimePriority, kMaxRealTimePriority].
   * @param outError          optional. Filled in with the platform error when
   *                          promotion fails.
   *
   * @return a handle to pass to `demoteCurrentThread()`, or an invalid handle on
   *         failure.
   *
   * @discussion
   * Promotion is best effort: a process that is not allowed to raise its
   * priority still renders correctly, just less predictably, so callers are
   * expected to continue after a failure. Always check `isValid()`.
   */
  [[nodiscard]] static Handle promoteCurrentThread(uint32_t audioBufferFrames = kDefaultBufferFrames,
      uint32_t audioSampleRateHz = 0,
      int32_t priority = kDefaultRealTimePriority,
      LastError* outError = nullptr) noexcept;

  /**
   * Promotes a thread described by `info`, which may not be the calling thread.
   *
   * @param info     the thread to promote, from `ThreadInfo::current()`.
   * @param outError optional. Filled in with the platform error on failure.
   *
   * @return a handle to pass to `demoteThread()`, or an invalid handle.
   *
   * @discussion
   * Only Android currently supports promoting another thread; everywhere else
   * this fails with `kNotSupported` and the caller must promote from the audio
   * thread itself.
   *
   * On Android the promotion is **reversible**: the target thread's scheduling
   * policy, priority and nice value are captured first and restored by
   * `demoteThread()`. Capturing another thread's scheduling state is not possible
   * through the POSIX calls, so it is read from `/proc/<tid>/stat`; when that
   * read fails this refuses with an error rather than overwriting a state it
   * could not put back. As on every platform, the described thread must still be
   * alive.
   */
  [[nodiscard]] static Handle promoteThread(const ThreadInfo& info,
      uint32_t audioBufferFrames = kDefaultBufferFrames,
      uint32_t audioSampleRateHz = 0,
      int32_t priority = kDefaultRealTimePriority,
      LastError* outError = nullptr) noexcept;

  /**
   * Reverts the scheduling state a promotion replaced on the calling thread.
   *
   * @return `true` when the previous state was restored.
   *
   * @discussion
   * Must run on the thread that was promoted. A `Handle` is consumed by this
   * call: demoting twice is an error, and a `RealTimeScope` never does it.
   */
  static bool demoteCurrentThread(const Handle& handle, LastError* outError = nullptr) noexcept;

  /**
   * Reverts the scheduling state a promotion replaced on the thread it
   * describes.
   *
   * @param info     the thread to demote, from `ThreadInfo::current()`.
   * @param handle   the handle `promoteThread()` returned for that thread.
   * @param outError optional. Filled in with the platform error on failure.
   * @return `true` when the previous state was restored.
   *
   * @discussion
   * Android-only, like `promoteThread()`. There it restores the policy, priority
   * and nice value that `promoteThread()` captured, so the round trip leaves the
   * thread as it was found. Everywhere else this fails with `kNotSupported`.
   *
   * @see promoteThread
   */
  static bool demoteThread(const ThreadInfo& info,
      const Handle& handle,
      LastError* outError = nullptr) noexcept;

  /**
   * Asks the platform to allow more real-time priority, for example by raising
   * `RLIMIT_RTPRIO` on Linux or by requesting a scheduler band on Android.
   *
   * @param maxPriority the highest priority the process expects to need.
   * @return `true` when the limit is now at least `maxPriority`.
   *
   * @discussion
   * Optional: `promoteCurrentThread()` does what it can without this. Useful
   * when a caller wants to know early whether real-time scheduling is available.
   */
  static bool setRealTimeLimit(int32_t maxPriority, LastError* outError = nullptr) noexcept;

  /**
   * Returns the platform's name for the mechanism in use, for logging.
   */
  [[nodiscard]] static const char* backendName() noexcept;

  /**
   * The share of one audio callback period this platform lets the thread compute
   * for, as a fraction of the period, for the given requested priority.
   *
   * On Apple platforms this is exactly the `computation / period` ratio of the
   * Mach time-constraint policy the promotion installs, and it is at most 1/2,
   * the share macOS 12 and later expect from an audio callback. Elsewhere it
   * describes how much of the callback the platform's scheduling guarantees the
   * thread. It is 1/2 at the 10-point default used on Windows and Android, and
   * 0.4 at the 6-point default used on Linux.
   *
   * Useful for checking that a callback fits the budget it asked for: a callback
   * that needs more CPU time than this cannot meet its deadline.
   *
   * @param priority the requested priority, clamped to the supported range.
   * @return a fraction in (0, 1].
   */
  [[nodiscard]] static double computationBudgetFraction(int32_t priority = kDefaultRealTimePriority) noexcept;

  /**
   * The callback size used when a promotion is asked for with 0 frames.
   *
   * Roughly 10 ms of audio at `audioSampleRateHz`, which is 480 frames at
   * 48 kHz: inside the 128-2048 frame (3-15 ms at 48 kHz) range that audio
   * render callbacks actually use, so a derived Mach time constraint is a
   * realistic period rather than a very long one.
   *
   * @param audioSampleRateHz the stream's sample rate in Hz, or 0 for 48 kHz.
   * @return a frame count of at least 1.
   */
  [[nodiscard]] static uint32_t derivedBufferFrames(uint32_t audioSampleRateHz) noexcept;

  /** The errno value reported when an operation is unsupported here. */
  static constexpr int64_t kNotSupported = -1;

  /** The errno value reported when the platform refused the request. */
  static constexpr int64_t kNotPermitted = -2;
};

/**
 * RAII guard that keeps a thread at real-time priority for its lifetime.
 *
 * This is the interface most code should use: it cannot forget to demote, and it
 * demotes when the scope exits early or an exception unwinds.
 *
 * @code
 * void renderLoop() {
 *   coral::RealTimeScope rt(512, 48000);
 *   if (!rt.isActive()) {
 *     logWarning("running without real-time priority: %s", rt.lastError().what());
 *   }
 *   while (running) {
 *     renderOneBuffer();
 *   }
 * }  // priority restored here
 * @endcode
 *
 * Thread affinity: a promotion must be undone on the thread that was promoted,
 * so a scope belongs to the thread that constructed it. Do not move it across
 * threads; moving it between scopes in the same thread is fine.
 *
 * Handling a failed demotion: the destructor demotes, but it cannot report
 * anything, because the object is already going away. **A failure inside the
 * destructor is therefore unrecoverable** — the thread stays at the promoted
 * priority and the handle that could have retried it is destroyed with the
 * scope. A caller that cares about that must call `demote()` explicitly while
 * the scope is still alive, check the result, and retry from `lastError()`:
 *
 * @code
 * {
 *   coral::RealTimeScope rt(512, 48000);
 *   render();
 *   if (!rt.demote()) {
 *     logError("could not restore priority: %s", rt.lastError().what());
 *     // The promotion is still in place; rt.isActive() is true and the
 *     // handle is still valid, so a retry is possible right here.
 *   }
 * }  // Nothing left to undo: the explicit demote() already succeeded.
 * @endcode
 */
class RealTimeScope final {
public:
  RealTimeScope() noexcept = default;

  /**
   * Promotes the calling thread for as long as this object lives.
   *
   * @param audioBufferFrames frames per callback, or 0 for a derived default.
   * @param audioSampleRateHz the stream's sample rate in Hz, or 0 for a default.
   * @param priority          requested priority, clamped to the supported range.
   */
  explicit RealTimeScope(uint32_t audioBufferFrames,
      uint32_t audioSampleRateHz,
      int32_t priority = AudioThreadPriority::kDefaultRealTimePriority) noexcept;

  /**
   * Reverts the priority, when it can.
   *
   * The destructor cannot report a failure anywhere, so it does not: a refused
   * restore leaves the thread at the promoted priority and the handle is
   * destroyed with the scope, so that failure is unrecoverable. Call `demote()`
   * explicitly before the scope ends when the failure has to be handled.
   */
  ~RealTimeScope();

  RealTimeScope(const RealTimeScope&) = delete;
  RealTimeScope& operator=(const RealTimeScope&) = delete;
  RealTimeScope(RealTimeScope&& other) noexcept;
  RealTimeScope& operator=(RealTimeScope&& other) noexcept;

  /**
   * Whether the thread is currently at real-time priority.
   *
   * `false` means the platform refused, which is not fatal: keep rendering.
   */
  [[nodiscard]] bool isActive() const noexcept {
    return mHandle.isValid();
  }

  /** The reason promotion failed, when `isActive()` is false. */
  [[nodiscard]] const AudioThreadPriority::LastError& lastError() const noexcept {
    return mError;
  }

  /**
   * The priority the thread had before promotion.
   *
   * Only meaningful while `isActive()` is true: once the promotion has been
   * undone there is no previous state left to describe, and this reports 0.
   */
  [[nodiscard]] int32_t previousPriority() const noexcept {
    return mPreviousPriority;
  }

  /**
   * Reverts the priority now, instead of at the end of the scope.
   *
   * @return `true` when the previous state was restored.
   *
   * @discussion
   * On failure the promotion is **kept**, not discarded: the handle stays valid,
   * `isActive()` stays true and `lastError()` describes what the platform
   * refused. That way a caller that sees the failure can retry, or call
   * `AudioThreadPriority::demoteCurrentThread()` itself, instead of being left
   * with a thread at a priority it never asked for and no way to find out.
   * A successful demotion is the only thing that makes `isActive()` false.
   */
  bool demote() noexcept;

private:
  AudioThreadPriority::Handle mHandle;
  AudioThreadPriority::LastError mError;
  int32_t mPreviousPriority = 0;
};

/**
 * Convenience overload that derives the callback size from the sample rate.
 *
 * @param audioSampleRateHz the stream's sample rate in Hz.
 */
[[nodiscard]] inline RealTimeScope makeRealTimeScope(uint32_t audioSampleRateHz,
    int32_t priority = AudioThreadPriority::kDefaultRealTimePriority) noexcept {
  return RealTimeScope(AudioThreadPriority::kDefaultBufferFrames, audioSampleRateHz, priority);
}

namespace detail {

/** Clamps a requested priority into the platform-independent range. */
[[nodiscard]] constexpr int32_t clampPriority(const int32_t priority) noexcept {
  return std::clamp(priority, AudioThreadPriority::kMinRealTimePriority,
      AudioThreadPriority::kMaxRealTimePriority);
}

} // namespace detail

} // namespace coral

#endif //CORAL_THREAD_AUDIO_THREAD_PRIORITY_H
