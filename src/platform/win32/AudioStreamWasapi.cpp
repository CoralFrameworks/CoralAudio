/*
 * Copyright (C) 2026 The Coral Project
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

#include "AudioStreamWasapi.h"

#include <coral/audio/AudioUtils.h>

#include <core/event/EventLooper.h>
#include <media/Utilities.h>
#include <minilog/log.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace coral {

namespace {

/** How many device buffers the ring buffer holds ahead of the device in blocking mode. */
constexpr uint32_t kRingBufferBursts = 4;

/** Default buffer, in frames, used when the endpoint does not report a device period. */
constexpr uint32_t kDefaultBufferFrames = 480;

/** How many device periods a shared mode buffer is asked for. */
constexpr int32_t kSharedBufferPeriods = 2;

/**
 * How many device periods an exclusive mode buffer is asked for.
 *
 * An exclusive-mode stream that is event driven is a ping-pong of two buffers whose duration is
 * the periodicity, so this is both the cadence of the callbacks and half of the latency of the
 * stream. It is a small multiple of the device period rather than the capacity the application
 * asked for: that capacity belongs to the ring buffer a blocking caller writes through, and a
 * device buffer that grew with it would stretch every callback and the latency with it.
 */
constexpr int32_t kExclusiveBufferPeriods = 2;

/** Bounds of the stream thread's wait for the device, in milliseconds. */
constexpr uint32_t kMinPumpTimeoutMillis = 10;
constexpr uint32_t kMaxPumpTimeoutMillis = 100;

/**
 * How long the stream thread waits for the device before it looks at the state machine
 * again, in milliseconds.
 *
 * `request*()` publishes the state it wants before it posts the message that applies it, and
 * the stream thread only runs one message at a time: a command posted while this thread is
 * waiting for the endpoint is picked up after a slice rather than after the whole device
 * timeout, which is what `stop()` and `pause()` are measured by.
 */
constexpr uint32_t kPumpWaitSliceMillis = 10;

/** One second expressed in the 100-nanosecond units WASAPI uses. */
constexpr REFERENCE_TIME kHnsPerSecond = 10000000;

/**
 * `KSDATAFORMAT_SUBTYPE_PCM` and `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`.
 *
 * They are spelled out here rather than taken from `<ksmedia.h>` because the header
 * only declares them: using its macros would mean linking the library that defines
 * them, for two constants that never change.
 */
const GUID kSubFormatPcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
const GUID kSubFormatIeeeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

/**
 * The speaker masks from `<ksmedia.h>`, for the channel counts an endpoint can mix.
 *
 * A mask of 0 leaves the speaker assignment to the endpoint, which is the right
 * answer for a layout that has no standard mask.
 */
constexpr DWORD kChannelMaskMono = 0x4; // SPEAKER_FRONT_CENTER
constexpr DWORD kChannelMaskStereo = 0x3; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
constexpr DWORD kChannelMaskQuad = 0x33;
constexpr DWORD kChannelMask5Point1 = 0x3F;
constexpr DWORD kChannelMask7Point1 = 0x63F;

/** The standard speaker mask of a channel count, or 0 when there is none. */
DWORD defaultChannelMask(const uint32_t channels) {
  switch (channels) {
    case 1:
      return kChannelMaskMono;
    case 2:
      return kChannelMaskStereo;
    case 4:
      return kChannelMaskQuad;
    case 6:
      return kChannelMask5Point1;
    case 8:
      return kChannelMask7Point1;
    default:
      return 0;
  }
}

/** Whether a mix format describes 32-bit floating point samples. */
bool isFloatFormat(const WAVEFORMATEX& format) {
  if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return true;
  }
  if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
    return false;
  }
  const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
  return IsEqualGUID(extensible->SubFormat, kSubFormatIeeeFloat) != FALSE;
}

/** Whether a mix format is the same layout this stream reports. */
bool isSameFormat(const WAVEFORMATEX& format,
  const uint32_t sampleRate,
  const uint32_t channels,
  const AudioSampleFormat sampleFormat) {
  if (isFloatFormat(format) != (sampleFormat == AudioSampleFormat::kFloat)) {
    return false;
  }
  return format.nSamplesPerSec == sampleRate && format.nChannels == channels &&
         format.wBitsPerSample == MediaUtils::getBytesPerSample(sampleFormat) * 8;
}

/** Whether the calling thread is the one running `thread`'s looper. */
bool isLooperThread(const std::unique_ptr<EventThread>& thread) {
  if (thread == nullptr) {
    return false;
  }
  const auto& looper = thread->getLooper();
  return looper != nullptr && looper->isCurrentThread();
}

/**
 * Scales a device clock position into frames at the stream's sample rate.
 *
 * `IAudioClock::GetPosition()` reports ticks of the device clock, whose frequency is its own
 * and not necessarily the stream's: endpoints commonly run the clock at 192 kHz while the
 * stream runs at 48 kHz, and a clock that counted its ticks as frames would advance four
 * times faster than the audio. The whole seconds of the position are split from the
 * remainder, so that a position that has been running for a long time cannot overflow the
 * multiplication.
 */
uint64_t framesFromClockPosition(const uint64_t position,
  const uint64_t clockFrequency,
  const uint64_t sampleRate) {
  if (clockFrequency == 0 || sampleRate == 0) {
    return 0;
  }
  const uint64_t seconds = position / clockFrequency;
  const uint64_t remainder = position % clockFrequency;
  return seconds * sampleRate + remainder * sampleRate / clockFrequency;
}

/**
 * Quits an event thread and disposes of it.
 *
 * A thread that quits from its own looper thread can only be detached, and the detached
 * thread goes on touching its `EventThread` object while it unwinds. Destroying that
 * object here would be a use after free, so in that one case ownership is deliberately
 * released and the object is left behind for the exiting thread. It is a few hundred
 * bytes, and the case only arises when a stream is closed from inside one of its own
 * callbacks, which is the documented "stop and close on another thread" restriction.
 */
void disposeEventThread(std::unique_ptr<EventThread>& thread) {
  if (thread == nullptr) {
    return;
  }
  const bool self = isLooperThread(thread);
  thread->quit();
  if (self) {
    (void) thread.release();
  } else {
    thread.reset();
  }
}

} // namespace anonymous

// =============================================================================
//  Construction
// =============================================================================

std::shared_ptr<AudioStream> AudioStreamWasapi::Create(const AudioStreamBuilder& builder) {
  return std::make_shared<AudioStreamWasapi>(builder);
}

AudioStreamWasapi::AudioStreamWasapi(const AudioStreamBuilder& builder) noexcept
  : AudioStream(builder),
    mEstablishedSharingMode(builder.getSharingMode()),
    // The base class reports the device buffer once the client is initialized, so the
    // capacity the application asked for is remembered here, before it is overwritten.
    mRequestedCapacityInFrames(builder.getBufferCapacityInFrames()),
    // The callback size is asked for in the same way, and it is published as the device burst
    // when the application left it unspecified, which is also remembered here.
    mRequestedFramesPerCallback(builder.getFramesPerDataCallback()),
    // The format and the sharing mode are reported by the device once it was consulted, and a
    // released stream reports none of them, so what was asked for is kept for the next open().
    mRequestedSampleRate(builder.getSampleRate()),
    mRequestedChannelCount(builder.getChannelCount()),
    mRequestedFormat(builder.getFormat()),
    mRequestedSharingMode(builder.getSharingMode()) {
}

AudioStreamWasapi::~AudioStreamWasapi() {
  // The destructor has to leave nothing behind: the caller is free to drop a running
  // stream without closing it first.
  close();
}

void AudioStreamWasapi::ensureComInitialized() {
  // COM must be initialized on every thread that touches a WASAPI interface, and the
  // stream thread is not the thread that called open(). The apartment is deliberately
  // left initialized for the life of the thread: the interfaces outlive the call that
  // created them, so uninitializing in between would tear down the very apartment
  // their proxies live in. A thread's apartment is released when the thread exits.
  static thread_local const bool initialized = []() {
    // Multithreaded, because the same interfaces are used from the caller's thread and
    // from the stream thread. RPC_E_CHANGED_MODE is not a failure here: it only means
    // the thread already has an apartment, which is all WASAPI needs.
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return true;
  }();
  (void) initialized;
}

// =============================================================================
//  Opening
// =============================================================================

Result AudioStreamWasapi::open() {
  std::lock_guard<std::mutex> lock(mLock);
  if (mState.load() != StreamState::Uninitialized) {
    MINILOG_WARN("AudioStreamWasapi::open() called on a stream that is not uninitialized");
    return Result::ErrorInvalidState;
  }

  const Result config = isValidConfig();
  if (config != Result::OK) {
    MINILOG_WARN("Invalid audio configuration: {}", convertToText(config));
    return config;
  }

  ensureComInitialized();

  Result result = openDevice();
  if (result == Result::OK) {
    result = negotiateFormat();
  }
  if (result == Result::OK) {
    result = initializeClient();
  }
  if (result == Result::OK) {
    result = createStreamResources();
  }
  if (result != Result::OK) {
    releaseDevice();
    return result;
  }

  result = startEventHandler();
  if (result != Result::OK) {
    quitEventHandler();
    releaseDevice();
    return result;
  }

  setState_l(StreamState::Open);
  mStateChanged.notify_all();

  MINILOG_DEBUG("AudioStreamWasapi opened: direction={}, sampleRate={}, channels={}, format={}, "
    "deviceBufferFrames={}, framesPerBurst={}, sharing={}",
    convertToText(getDirection()),
    static_cast<int>(getSampleRate()),
    static_cast<int>(getChannelCount()),
    convertToText(getFormat()),
    static_cast<int>(mDeviceBufferFrames),
    static_cast<int>(mFramesPerBurst),
    mEstablishedSharingMode == SharingMode::Exclusive ? "exclusive" : "shared");
  return Result::OK;
}

Result AudioStreamWasapi::openDevice() {
  const HRESULT createResult = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(mDeviceEnumerator.GetAddressOf()));
  if (FAILED(createResult)) {
    MINILOG_ERROR("Failed to create the WASAPI device enumerator: {}", static_cast<int32_t>(createResult));
    return resultFromHresult(createResult);
  }

  const EDataFlow flow = isRender() ? eRender : eCapture;
  const HRESULT endpointResult = mDeviceEnumerator->GetDefaultAudioEndpoint(
    flow, eConsole, mDevice.GetAddressOf());
  if (FAILED(endpointResult)) {
    MINILOG_ERROR("No default {} endpoint is available: {}",
      isRender() ? "playback" : "recording", static_cast<int32_t>(endpointResult));
    return resultFromHresult(endpointResult);
  }

  const HRESULT activateResult = mDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
    reinterpret_cast<void**>(mAudioClient.GetAddressOf()));
  if (FAILED(activateResult)) {
    MINILOG_ERROR("Failed to activate the WASAPI audio client: {}", static_cast<int32_t>(activateResult));
    return resultFromHresult(activateResult);
  }
  return Result::OK;
}

void AudioStreamWasapi::fillFormat(WAVEFORMATEXTENSIBLE* out,
  const uint32_t sampleRate,
  const uint32_t channels,
  const AudioSampleFormat format,
  const DWORD channelMask) {
  ZeroMemory(out, sizeof(*out));

  uint16_t bitsPerSample = 32;
  const GUID* subFormat = &kSubFormatIeeeFloat;
  switch (format) {
    case AudioSampleFormat::kInt16:
      bitsPerSample = 16;
      subFormat = &kSubFormatPcm;
      break;
    case AudioSampleFormat::kInt24:
      bitsPerSample = 24;
      subFormat = &kSubFormatPcm;
      break;
    case AudioSampleFormat::kInt32:
      bitsPerSample = 32;
      subFormat = &kSubFormatPcm;
      break;
    case AudioSampleFormat::kFloat:
    default:
      bitsPerSample = 32;
      subFormat = &kSubFormatIeeeFloat;
      break;
  }

  out->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  out->Format.nChannels = static_cast<WORD>(channels);
  out->Format.nSamplesPerSec = sampleRate;
  out->Format.wBitsPerSample = bitsPerSample;
  out->Format.nBlockAlign = static_cast<WORD>(channels * bitsPerSample / 8);
  out->Format.nAvgBytesPerSec = out->Format.nBlockAlign * sampleRate;
  out->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  out->Samples.wValidBitsPerSample = bitsPerSample;
  out->dwChannelMask = channelMask;
  out->SubFormat = *subFormat;
}

AudioSampleFormat AudioStreamWasapi::formatFromWaveFormat(const WAVEFORMATEX& format) {
  // The format is described by the container, because that is what the device and the
  // ring buffer move around: 24 valid bits inside a 32-bit container are reported as
  // 32-bit samples, which is what the caller has to hand over.
  //
  // The container is also all the scaling needs. A PCM container is left-aligned, so the
  // valid bits of a `wValidBitsPerSample` smaller than `wBitsPerSample` are the most
  // significant ones: a full scale 24-bit sample in a 32-bit container is a full scale
  // int32, and the 1/2^31 scaling of `SampleTraits<int32_t>` reads it correctly.
  if (isFloatFormat(format)) {
    return format.wBitsPerSample == 32 ? AudioSampleFormat::kFloat : AudioSampleFormat::kInvalid;
  }
  switch (format.wBitsPerSample) {
    case 16:
      return AudioSampleFormat::kInt16;
    case 24:
      return AudioSampleFormat::kInt24;
    case 32:
      return AudioSampleFormat::kInt32;
    default:
      return AudioSampleFormat::kInvalid;
  }
}

void AudioStreamWasapi::adoptDeviceFormat(const WAVEFORMATEX& format) {
  mSampleRate = format.nSamplesPerSec;
  mChannelCount = format.nChannels;
  mFormat = formatFromWaveFormat(format);
}

Result AudioStreamWasapi::negotiateFormat() {
  WAVEFORMATEX* mixFormat = nullptr;
  const HRESULT mixResult = mAudioClient->GetMixFormat(&mixFormat);
  if (FAILED(mixResult) || mixFormat == nullptr) {
    MINILOG_ERROR("Failed to query the endpoint mix format: {}", static_cast<int32_t>(mixResult));
    return resultFromHresult(mixResult);
  }

  // Every field the application left unspecified is taken from what the endpoint is
  // already mixing, so an unconfigured stream plays without any conversion. What was asked for
  // is read from the request rather than from what the stream reports: a released stream
  // reports the format of the device that is gone, and the stream being opened here is the one
  // the application asked for.
  const uint32_t sampleRate = mRequestedSampleRate != kUnspecified
                                ? static_cast<uint32_t>(mRequestedSampleRate)
                                : mixFormat->nSamplesPerSec;
  const uint32_t channels =
    mRequestedChannelCount != kUnspecified ? mRequestedChannelCount : mixFormat->nChannels;
  AudioSampleFormat format = mRequestedFormat;
  if (format == AudioSampleFormat::kDefault) {
    format = formatFromWaveFormat(*mixFormat);
  }

  if (sampleRate == 0 || channels == 0 || format == AudioSampleFormat::kInvalid ||
      format == AudioSampleFormat::kDefault) {
    MINILOG_ERROR("Unsupported audio format request: sampleRate={}, channels={}, format={}",
      static_cast<int>(sampleRate), static_cast<int>(channels), convertToText(format));
    CoTaskMemFree(mixFormat);
    return Result::ErrorInvalidFormat;
  }

  // Reuse the endpoint's own channel mask when the channel count matches, so a
  // surround request keeps the speaker assignment the endpoint already uses.
  DWORD channelMask = defaultChannelMask(channels);
  if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mixFormat->nChannels == channels) {
    channelMask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat)->dwChannelMask;
  }

  mEngineConverts = !isSameFormat(*mixFormat, sampleRate, channels, format);
  fillFormat(&mDeviceFormat, sampleRate, channels, format, channelMask);
  adoptDeviceFormat(mDeviceFormat.Format);
  CoTaskMemFree(mixFormat);
  return Result::OK;
}

Result AudioStreamWasapi::initializeClient() {
  REFERENCE_TIME defaultPeriod = 0;
  REFERENCE_TIME minPeriod = 0;
  if (FAILED(mAudioClient->GetDevicePeriod(&defaultPeriod, &minPeriod))) {
    defaultPeriod = 0;
    minPeriod = 0;
  }

  // The rate the client is initialized at, which is the requested one unless the endpoint
  // refuses to convert and the stream falls back to what the endpoint mixes.
  auto sampleRate = static_cast<uint32_t>(getSampleRate());

  // The device period the stream thread is woken on, which the endpoint reports as the minimum
  // one in the low latency mode and as its default otherwise. It is read here because the
  // exclusive mode buffer below is expressed in periods.
  const REFERENCE_TIME period =
    (getPerformanceMode() == PerformanceMode::LowLatency && minPeriod > 0) ? minPeriod : defaultPeriod;

  // A buffer duration is a time, and the frames it covers depend on the rate the stream is
  // opened at: a duration that covers the requested capacity at one rate covers fewer frames
  // at a higher one. It is therefore recomputed whenever the rate changes below.
  const auto durationForRate = [this, defaultPeriod](const uint32_t rate) {
    REFERENCE_TIME duration = defaultPeriod > 0
                                ? defaultPeriod * kSharedBufferPeriods
                                : hnsFromFrames(kDefaultBufferFrames, rate);
    if (mRequestedCapacityInFrames != kUnspecified) {
      duration = std::max(duration, hnsFromFrames(mRequestedCapacityInFrames, rate));
    }
    return duration;
  };

  // An exclusive-mode stream that is event driven is a ping-pong of two buffers that WASAPI
  // requires to be as long as the periodicity, so its buffer is the period: the application is
  // served one of them per wake, and the latency of the stream is the two of them. It is
  // deliberately not the capacity the application asked for, which is there so that a blocking
  // caller has somewhere to put its audio, and which would otherwise become the cadence of the
  // callbacks and the latency of the stream.
  const auto exclusiveDurationForRate = [period](const uint32_t rate) {
    return period > 0 ? period * kExclusiveBufferPeriods : hnsFromFrames(kDefaultBufferFrames, rate);
  };

  REFERENCE_TIME duration = durationForRate(sampleRate);
  REFERENCE_TIME exclusiveDuration = exclusiveDurationForRate(sampleRate);

  // The event callback flag is what makes the endpoint signal `mAudioEvent` instead of
  // expecting the application to poll, and a stream never persists its settings.
  const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;

  HRESULT hr = E_FAIL;
  bool established = false;

  if (mRequestedSharingMode == SharingMode::Exclusive) {
    WAVEFORMATEX* closest = nullptr;
    const HRESULT supported =
      mAudioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &mDeviceFormat.Format, &closest);
    if (closest != nullptr) {
      CoTaskMemFree(closest);
    }
    if (supported == S_OK) {
      hr = mAudioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, exclusiveDuration,
        exclusiveDuration, &mDeviceFormat.Format, nullptr);
      if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Exclusive mode only accepts a buffer that is an exact multiple of the device
        // period. The endpoint reports the period it settled on, and the client has to
        // be activated again before it can be initialized with it.
        UINT32 alignedFrames = 0;
        if (SUCCEEDED(mAudioClient->GetBufferSize(&alignedFrames)) && alignedFrames > 0) {
          exclusiveDuration = hnsFromFrames(alignedFrames, sampleRate);
          mAudioClient.Reset();
          const HRESULT activateResult = mDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(mAudioClient.GetAddressOf()));
          if (SUCCEEDED(activateResult)) {
            hr = mAudioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, exclusiveDuration,
              exclusiveDuration, &mDeviceFormat.Format, nullptr);
          } else {
            hr = activateResult;
          }
        }
      }
      established = SUCCEEDED(hr);
    } else {
      // Exclusive mode only accepts a format the endpoint supports natively, and there
      // is no way to enumerate those, so a refusal is expected rather than exceptional:
      // the stream degrades to shared mode, which is what most machines can do.
      MINILOG_DEBUG("The endpoint refused exclusive mode ({}); using shared mode",
        static_cast<int32_t>(supported));
    }
  }

  if (!established) {
    // Shared mode with engine side conversion keeps the format the application asked
    // for even when the endpoint mixes something else.
    const DWORD convertFlags = flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const bool wantsConversion = mEngineConverts;
    hr = mAudioClient->Initialize(
      AUDCLNT_SHAREMODE_SHARED, convertFlags, duration, 0, &mDeviceFormat.Format, nullptr);
    if (SUCCEEDED(hr)) {
      mEngineConverts = wantsConversion;
    } else {
      // No engine conversion available: open the endpoint with exactly what it mixes
      // and report that format to the application.
      WAVEFORMATEX* mixFormat = nullptr;
      if (FAILED(mAudioClient->GetMixFormat(&mixFormat)) || mixFormat == nullptr) {
        MINILOG_ERROR("Failed to open the WASAPI client: {}", static_cast<int32_t>(hr));
        return resultFromHresult(hr);
      }
      const AudioSampleFormat mixSampleFormat = formatFromWaveFormat(*mixFormat);
      if (mixSampleFormat == AudioSampleFormat::kInvalid) {
        MINILOG_ERROR("The endpoint mixes a format this stream cannot represent");
        CoTaskMemFree(mixFormat);
        return Result::ErrorInvalidFormat;
      }
      fillFormat(&mDeviceFormat, mixFormat->nSamplesPerSec, mixFormat->nChannels, mixSampleFormat,
        mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
          ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat)->dwChannelMask
          : defaultChannelMask(mixFormat->nChannels));
      adoptDeviceFormat(*mixFormat);
      CoTaskMemFree(mixFormat);
      mEngineConverts = false;
      // The stream is opened at the rate the endpoint mixes, so the buffer duration and the
      // capacity the application asked for are recomputed for that rate: the duration that
      // was prepared for the requested rate would cover fewer frames than were asked for.
      sampleRate = static_cast<uint32_t>(getSampleRate());
      if (sampleRate == 0) {
        // Everything below is expressed in frames of that rate, so a rate of zero is not a
        // stream that can be opened.
        MINILOG_ERROR("The endpoint mixes at a sample rate of zero");
        return Result::ErrorInvalidFormat;
      }
      duration = durationForRate(sampleRate);
      hr = mAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, &mDeviceFormat.Format, nullptr);
      if (FAILED(hr)) {
        MINILOG_ERROR("Failed to open the WASAPI client in shared mode: {}", static_cast<int32_t>(hr));
        return resultFromHresult(hr);
      }
    }
  }

  mEstablishedSharingMode = established ? SharingMode::Exclusive : SharingMode::Shared;
  mSharingMode = mEstablishedSharingMode;

  UINT32 bufferFrames = 0;
  if (FAILED(mAudioClient->GetBufferSize(&bufferFrames)) || bufferFrames == 0) {
    MINILOG_ERROR("The WASAPI client reported no buffer size");
    return Result::ErrorInternal;
  }
  mDeviceBufferFrames = bufferFrames;
  mBufferCapacityInFrames = bufferFrames;
  mBufferSizeInFrames = bufferFrames;

  // One device period is the burst the endpoint consumes at a time, and the shortest
  // wait the stream thread can be woken up for.
  mFramesPerBurst = framesFromHns(period, sampleRate);
  if (mFramesPerBurst == 0 || mFramesPerBurst > mDeviceBufferFrames) {
    mFramesPerBurst = mDeviceBufferFrames;
  }

  // The size one data callback is given at most: what the application asked for, or the burst of
  // a stream that did not ask for one. It is published as the stream's callback size, and the
  // pump holds itself to it, so a device buffer that is larger than this is served by several
  // callbacks rather than by one buffer the application did not budget for.
  mFramesPerCallback = mRequestedFramesPerCallback != kUnspecified
                         ? mRequestedFramesPerCallback
                         : mFramesPerBurst;

  const uint32_t bufferMillis = mDeviceBufferFrames * 1000 / sampleRate;
  mPumpTimeoutMillis =
    std::clamp(bufferMillis * 2, kMinPumpTimeoutMillis, kMaxPumpTimeoutMillis);
  calculateDefaultDelayBeforeCloseMillis();
  return Result::OK;
}

Result AudioStreamWasapi::createStreamResources() {
  mAudioEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto reset
  if (mAudioEvent == nullptr) {
    MINILOG_ERROR("Failed to create the WASAPI buffer event");
    return Result::ErrorNoMemory;
  }

  HRESULT hr = mAudioClient->SetEventHandle(mAudioEvent);
  if (FAILED(hr)) {
    MINILOG_ERROR("Failed to register the WASAPI buffer event: {}", static_cast<int32_t>(hr));
    return resultFromHresult(hr);
  }

  if (isRender()) {
    hr = mAudioClient->GetService(__uuidof(IAudioRenderClient),
      reinterpret_cast<void**>(mRenderClient.GetAddressOf()));
  } else {
    hr = mAudioClient->GetService(__uuidof(IAudioCaptureClient),
      reinterpret_cast<void**>(mCaptureClient.GetAddressOf()));
  }
  if (FAILED(hr)) {
    MINILOG_ERROR("Failed to get the WASAPI {} client: {}",
      isRender() ? "render" : "capture", static_cast<int32_t>(hr));
    return resultFromHresult(hr);
  }

  // The device clock is optional: it only refines what the render frame counters
  // report, and a stream is perfectly usable without it.
  if (FAILED(mAudioClient->GetService(
    __uuidof(IAudioClock), reinterpret_cast<void**>(mAudioClock.GetAddressOf())))) {
    mAudioClock.Reset();
  }

  // The ring buffer decouples the caller from the device in blocking mode. In callback
  // mode it stays empty and untouched, but it is sized anyway so that a stream can be
  // used either way without being reopened.
  const uint32_t deviceFrames = std::max(mDeviceBufferFrames, kDefaultBufferFrames);
  const uint32_t capacity = std::max(deviceFrames * kRingBufferBursts,
    mRequestedCapacityInFrames != kUnspecified ? mRequestedCapacityInFrames : 0);
  mRingBuffer.init(capacity, getChannelCount());

  // Silent packets and underruns need zeroed frames in the stream's own format. The buffer
  // covers twice the device buffer, which is beyond any packet an endpoint hands over in
  // practice, so that the stream thread never has to allocate for one: a larger packet is
  // truncated by `pumpCapture` and reported once.
  mSilenceBuffer.assign(static_cast<size_t>(deviceFrames) * 2 * getBytesPerFrame(), 0);
  mReportedOversizedSilencePacket = false;
  return Result::OK;
}

Result AudioStreamWasapi::startEventHandler() {
  mThread = std::make_unique<EventThread>("CoralWasapiStream");
  mThread->start();
  if (mThread->getLooper() == nullptr) {
    return Result::ErrorInternal;
  }

  // A handler holds its callback weakly, so the stream has to keep a reference alive
  // for as long as a handler may dispatch to it. The reference is non-owning: the
  // stream belongs to its caller, and a deleter here would destroy it a second time.
  mCallbackRef = std::shared_ptr<EventHandler::Callback>(this, [](EventHandler::Callback*) {
  });
  mHandler = EventHandler::create(mThread->getLooper(), mCallbackRef);
  if (mHandler == nullptr) {
    return Result::ErrorInternal;
  }

  if (isErrorCallbackSpecified()) {
    // Errors are delivered on their own thread because the documented reaction to one is
    // to stop and close the stream, and closing joins the stream thread.
    mErrorThread = std::make_unique<EventThread>("CoralWasapiError");
    mErrorThread->start();
    if (mErrorThread->getLooper() == nullptr) {
      return Result::ErrorInternal;
    }
    mErrorHandler = EventHandler::create(mErrorThread->getLooper(), mCallbackRef);
    if (mErrorHandler == nullptr) {
      return Result::ErrorInternal;
    }
  }
  return Result::OK;
}

// =============================================================================
//  Teardown
// =============================================================================

Result AudioStreamWasapi::close() {
  {
    std::lock_guard<std::mutex> lock(mLock);
    switch (mState.load()) {
      case StreamState::Closed:
        return Result::ErrorClosed;
      case StreamState::Uninitialized:
        // Nothing was ever opened, so there is nothing to release.
        return Result::ErrorInvalidState;
      default:
        // Refuse any request*() that races with this call, and stop the pump from
        // rescheduling itself.
        setState_l(StreamState::Closing);
        break;
    }
  }
  mStateChanged.notify_all();

  // Refresh the frame counters while the device is still reachable: the render counter
  // is read from the device clock.
  AudioStream::close();
  teardown();

  setState(StreamState::Closed);
  return Result::OK;
}

Result AudioStreamWasapi::release() {
  {
    std::lock_guard<std::mutex> lock(mLock);
    if (mState.load() == StreamState::Uninitialized || mState.load() == StreamState::Closed) {
      return Result::ErrorClosed;
    }
    setState_l(StreamState::Closing);
  }
  mStateChanged.notify_all();

  teardown();

  // Released, not closed: the object can be opened again.
  setState(StreamState::Uninitialized);
  return Result::OK;
}

void AudioStreamWasapi::teardown() {
  // Joining the threads first is what makes the rest safe: once no handler can run, no
  // device call and no application callback is in flight, so the interfaces below can
  // be released without racing anything.
  quitEventHandler();
  releaseDevice();

  mFatalErrorReported.store(false);
  mPumpScheduled.store(false);
  mXRunCount.store(0);
  mLastClockPosition.store(0);
  mFramesPlayedBase.store(0);
  mDeviceBufferFrames = 0;

  // A handle for a stream thread that is already gone is simply dropped: demoting from
  // this thread would change *this* thread's priority, and the thread MMCSS promoted
  // has exited, which releases its own task.
  mThreadPriority = AudioThreadPriority::Handle{};

  resetRingBuffer();
}

void AudioStreamWasapi::quitEventHandler() {
  std::shared_ptr<EventHandler> handler;
  std::shared_ptr<EventHandler> errorHandler;
  {
    std::lock_guard<std::mutex> lock(mLock);
    // Clearing the handlers under the lock is what guarantees that no request*() that
    // runs from now on can post to a handler that is being torn down.
    handler = std::move(mHandler);
    mHandler = nullptr;
    errorHandler = std::move(mErrorHandler);
    mErrorHandler = nullptr;
  }

  if (handler != nullptr) {
    handler->removeAllCallbacksAndMessages();
  }
  if (errorHandler != nullptr) {
    errorHandler->removeAllCallbacksAndMessages();
  }

  // Both quits join the thread, except when called from the thread itself: an
  // application that closes its stream from inside its own data or error callback can
  // only be detached from.
  disposeEventThread(mThread);
  disposeEventThread(mErrorThread);
  // Dropping the last reference to the callback object does not delete the stream: the
  // reference was built with a no-op deleter.
  mCallbackRef.reset();
}

void AudioStreamWasapi::releaseDevice() {
  ensureComInitialized();
  std::unique_lock<std::shared_mutex> lock(mClientLock);
  if (mAudioClient != nullptr) {
    // Stop before releasing so the endpoint is not left running on a dead session, and
    // reset so its buffer does not hold audio for the next client.
    mAudioClient->Stop();
    mAudioClient->Reset();
  }
  mAudioClock.Reset();
  mRenderClient.Reset();
  mCaptureClient.Reset();
  mAudioClient.Reset();
  mDevice.Reset();
  mDeviceEnumerator.Reset();
  if (mAudioEvent != nullptr) {
    ::CloseHandle(mAudioEvent);
    mAudioEvent = nullptr;
  }
  mSilenceBuffer.clear();
  mEngineConverts = false;
  // A pump that is inside a data callback has to notice that the interfaces it holds are
  // gone, even when the stream was opened again in the meantime and they are not null.
  mClientGeneration.fetch_add(1, std::memory_order_release);

  // The stream reports an unspecified format again: a released stream has no device behind
  // it, and reading these before the next open() would otherwise report the values of the
  // device that is gone.
  mDeviceFormat = WAVEFORMATEXTENSIBLE{};
  mSampleRate = kUnspecified;
  mChannelCount = kUnspecified;
  mFormat = AudioSampleFormat::kDefault;
  mFramesPerBurst = kUnspecified;
  mBufferCapacityInFrames = kUnspecified;
  mBufferSizeInFrames = kUnspecified;
  // The callback size falls back to what the application asked for, without the burst the
  // device that is gone imposed on it.
  mFramesPerCallback = mRequestedFramesPerCallback;
  // The mode the stream reports falls back to the one that was asked for, for the same reason:
  // the mode that was established belonged to the device that is gone, and the next open()
  // tries the request again instead of the mode it fell back to.
  mSharingMode = mRequestedSharingMode;
}

// =============================================================================
//  State machine
// =============================================================================

StreamState AudioStreamWasapi::getState() {
  // A plain read: the state is published atomically, and `AudioStream::waitForStateTransition`
  // calls this while it already holds `mLock`.
  return mState.load();
}

void AudioStreamWasapi::setState(const StreamState state) {
  {
    std::lock_guard<std::mutex> lock(mLock);
    setState_l(state);
  }
  mStateChanged.notify_all();
}

void AudioStreamWasapi::setState_l(const StreamState state) {
  mState.store(state);
}

bool AudioStreamWasapi::isCommandExpected(const StreamState expected) {
  std::lock_guard<std::mutex> lock(mLock);
  return mState.load() == expected;
}

Result AudioStreamWasapi::waitForStateChange(const StreamState inputState,
  StreamState* nextState,
  const int64_t timeoutNanoseconds) {
  std::unique_lock<std::mutex> lock(mLock);
  StreamState state = mState.load();

  if (state == StreamState::Closed) {
    if (nextState != nullptr) {
      *nextState = state;
    }
    return Result::ErrorClosed;
  }

  if (state == inputState && timeoutNanoseconds > 0) {
    mStateChanged.wait_for(lock, std::chrono::nanoseconds(timeoutNanoseconds),
      [this, inputState] { return mState.load() != inputState; });
    state = mState.load();
  }

  if (nextState != nullptr) {
    *nextState = state;
  }
  if (state == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  return state != inputState ? Result::OK : Result::ErrorTimeout;
}

Result AudioStreamWasapi::requestStart() {
  std::shared_ptr<EventHandler> handler;
  {
    std::lock_guard<std::mutex> lock(mLock);
    switch (mState.load()) {
      case StreamState::Open:
      case StreamState::Paused:
      case StreamState::Stopped:
      case StreamState::Flushed:
        break;
      case StreamState::Starting:
      case StreamState::Started:
        // Already starting or running: nothing to do.
        return Result::OK;
      default:
        return Result::ErrorInvalidState;
    }
    if (mHandler == nullptr) {
      return Result::ErrorClosed;
    }
    if (isDataCallbackSpecified()) {
      setDataCallbackEnabled(true);
    }
    setState_l(StreamState::Starting);
    handler = mHandler;
  }
  mStateChanged.notify_all();
  return handler->sendEmptyMessage(MSG_START) ? Result::OK : Result::ErrorInternal;
}

Result AudioStreamWasapi::requestPause() {
  std::shared_ptr<EventHandler> handler;
  {
    std::lock_guard<std::mutex> lock(mLock);
    switch (mState.load()) {
      case StreamState::Started:
      case StreamState::Starting:
        break;
      case StreamState::Pausing:
      case StreamState::Paused:
        return Result::OK;
      default:
        return Result::ErrorInvalidState;
    }
    if (mHandler == nullptr) {
      return Result::ErrorClosed;
    }
    setState_l(StreamState::Pausing);
    handler = mHandler;
  }
  mStateChanged.notify_all();
  return handler->sendEmptyMessage(MSG_PAUSE) ? Result::OK : Result::ErrorInternal;
}

Result AudioStreamWasapi::requestFlush() {
  std::shared_ptr<EventHandler> handler;
  {
    std::lock_guard<std::mutex> lock(mLock);
    switch (mState.load()) {
      case StreamState::Starting:
      case StreamState::Started:
      case StreamState::Pausing:
      case StreamState::Paused:
        break;
      case StreamState::Flushing:
      case StreamState::Flushed:
        return Result::OK;
      default:
        return Result::ErrorInvalidState;
    }
    if (mHandler == nullptr) {
      return Result::ErrorClosed;
    }
    setState_l(StreamState::Flushing);
    handler = mHandler;
  }
  mStateChanged.notify_all();
  return handler->sendEmptyMessage(MSG_FLUSH) ? Result::OK : Result::ErrorInternal;
}

Result AudioStreamWasapi::requestStop() {
  std::shared_ptr<EventHandler> handler;
  {
    std::lock_guard<std::mutex> lock(mLock);
    switch (mState.load()) {
      case StreamState::Open:
      case StreamState::Starting:
      case StreamState::Started:
      case StreamState::Pausing:
      case StreamState::Paused:
      case StreamState::Flushing:
      case StreamState::Flushed:
      case StreamState::Disconnected:
        break;
      case StreamState::Stopping:
      case StreamState::Stopped:
        return Result::OK;
      default:
        return Result::ErrorInvalidState;
    }
    if (mHandler == nullptr) {
      return Result::ErrorClosed;
    }
    setState_l(StreamState::Stopping);
    handler = mHandler;
  }
  mStateChanged.notify_all();
  return handler->sendEmptyMessage(MSG_STOP) ? Result::OK : Result::ErrorInternal;
}

bool AudioStreamWasapi::handleMessage(Message* msg) {
  switch (msg->what) {
    case MSG_PUMP:
      pump();
      break;
    case MSG_START:
      onStart();
      break;
    case MSG_PAUSE:
      onPause();
      break;
    case MSG_FLUSH:
      onFlush();
      break;
    case MSG_STOP:
      onStop();
      break;
    case MSG_NOTIFY_ERROR:
      onNotifyError();
      break;
    default:
      // Not a message of this stream: report it as unhandled rather than swallowing it. Both
      // handlers share this callback object, so an unknown code is a mistake somewhere, and
      // the handler that owns it is the one that can say so.
      return false;
  }
  // The message was consumed here.
  return true;
}

// =============================================================================
//  Device commands
// =============================================================================

void AudioStreamWasapi::onStart() {
  if (!isCommandExpected(StreamState::Starting)) {
    return;
  }

  Result result = Result::OK;
  {
    std::shared_lock<std::shared_mutex> lock(mClientLock);
    if (mAudioClient == nullptr) {
      result = Result::ErrorClosed;
    } else {
      if (mDeviceBufferStale) {
        // A reset that failed while stopping or flushing can leave audio queued in the
        // client, which this start would play. The client is stopped here, so retrying the
        // reset is legal, and this is the last chance to do it before the device runs again.
        if (SUCCEEDED(mAudioClient->Reset())) {
          mDeviceBufferStale = false;
        } else {
          MINILOG_WARN("The WASAPI client still refuses to reset; the next start may play what it holds");
        }
      }
      if (isRender() && mRenderClient != nullptr) {
        // Fill the buffer before the device is started: the engine starts consuming as
        // soon as Start() returns, and whatever the buffer held would be heard as a
        // click. Silence is what a stream that has not been written to should play.
        //
        // An exclusive-mode stream is handed a whole buffer per event, and the one the client
        // holds is what has to be filled before the device runs, so silence fills all of it.
        // A shared-mode stream keeps the frames that are already queued instead, and only what
        // the engine has made room for is filled again.
        UINT32 silent = mDeviceBufferFrames;
        if (mEstablishedSharingMode != SharingMode::Exclusive) {
          UINT32 padding = 0;
          silent = SUCCEEDED(mAudioClient->GetCurrentPadding(&padding)) && mDeviceBufferFrames > padding
                     ? mDeviceBufferFrames - padding
                     : 0;
        }
        if (silent > 0) {
          BYTE* data = nullptr;
          if (SUCCEEDED(mRenderClient->GetBuffer(silent, &data))) {
            mRenderClient->ReleaseBuffer(silent, AUDCLNT_BUFFERFLAGS_SILENT);
          }
        }
      }
      result = resultFromHresult(mAudioClient->Start());
    }
  }

  if (result != Result::OK) {
    mErrorCallbackResult = result;
    MINILOG_ERROR("Failed to start the WASAPI stream: {}", convertToText(result));
    if (isCallbackMode()) {
      setDataCallbackEnabled(false);
    }
    if (result == Result::ErrorClosed) {
      // The client was released from under this command, which only a concurrent close or
      // release does: that path owns the state machine from here on.
      return;
    }
    // `start()` returned long before this ran, so the application can only learn about the
    // failure through its error callback. Reporting it is what keeps an endpoint that
    // refuses to run from looking like a stream that simply did not start.
    reportDeviceError(result, "IAudioClient::Start");
    return;
  }

  setState(StreamState::Started);
  schedulePump();
}

void AudioStreamWasapi::onPause() {
  if (!isCommandExpected(StreamState::Pausing)) {
    return;
  }
  {
    std::shared_lock<std::shared_mutex> lock(mClientLock);
    if (mAudioClient != nullptr) {
      // WASAPI pauses by stopping the client: the buffer is kept, so a later start
      // resumes exactly where this left off.
      mAudioClient->Stop();
    }
  }
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  setState(StreamState::Paused);
}

void AudioStreamWasapi::onFlush() {
  if (!isCommandExpected(StreamState::Flushing)) {
    return;
  }
  {
    std::shared_lock<std::shared_mutex> lock(mClientLock);
    if (mAudioClient != nullptr) {
      // Reset is only legal while the client is stopped, and a flushed stream has
      // nothing left to play.
      mAudioClient->Stop();
      mDeviceBufferStale = FAILED(mAudioClient->Reset());
      if (mDeviceBufferStale) {
        MINILOG_DEBUG("The WASAPI client refused to reset while flushing");
      }
    }
  }
  resetRingBuffer();
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  setState(StreamState::Flushed);
}

void AudioStreamWasapi::onStop() {
  if (!isCommandExpected(StreamState::Stopping)) {
    return;
  }
  {
    std::shared_lock<std::shared_mutex> lock(mClientLock);
    if (mAudioClient != nullptr) {
      mAudioClient->Stop();
      // A reset that fails leaves whatever the client queued in place, so the next start
      // remembers to try again instead of playing it.
      mDeviceBufferStale = FAILED(mAudioClient->Reset());
      if (mDeviceBufferStale) {
        MINILOG_DEBUG("The WASAPI client refused to reset while stopping");
      }
    }
  }
  resetRingBuffer();
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  setState(StreamState::Stopped);
}

// =============================================================================
//  Device pump
// =============================================================================

void AudioStreamWasapi::schedulePump() {
  // Exactly one MSG_PUMP is allowed to be in flight: the pump reschedules itself at the
  // end of every iteration, so a state change that stops it cannot leave a queue that
  // grows or a loop that spins.
  if (mPumpScheduled.exchange(true)) {
    return;
  }

  std::shared_ptr<EventHandler> handler;
  {
    std::lock_guard<std::mutex> lock(mLock);
    handler = mHandler;
  }
  if (handler == nullptr || !handler->sendEmptyMessage(MSG_PUMP)) {
    mPumpScheduled.store(false);
  }
}

void AudioStreamWasapi::pump() {
  mPumpScheduled.store(false);

  if (mState.load() != StreamState::Started) {
    demotePumpThread();
    return;
  }
  promotePumpThread();

  // The endpoint signals the event once per period. The wait is bounded so that an
  // endpoint that stops signalling cannot hold this thread forever, and it is taken in
  // slices, because the commands that `request*()` posts are only handled once this
  // iteration returns: a command waits for the current slice rather than for the whole
  // device timeout, which is what bounds the latency of `pause()`, `flush()` and `stop()`.
  if (mAudioEvent != nullptr) {
    uint32_t waited = 0;
    while (waited < mPumpTimeoutMillis) {
      const uint32_t slice = std::min(kPumpWaitSliceMillis, mPumpTimeoutMillis - waited);
      if (::WaitForSingleObject(mAudioEvent, slice) == WAIT_OBJECT_0) {
        break;
      }
      waited += slice;
      // A request published its state before it posted the message that applies it, so a
      // state that is no longer Started is a command waiting to be handled.
      if (mState.load() != StreamState::Started) {
        demotePumpThread();
        return;
      }
    }
  }

  if (mState.load() != StreamState::Started) {
    demotePumpThread();
    return;
  }

  ensureComInitialized();
  if (isRender()) {
    pumpRender();
  } else {
    pumpCapture();
  }

  if (mState.load() == StreamState::Started) {
    schedulePump();
  } else {
    demotePumpThread();
  }
}

uint32_t AudioStreamWasapi::callbackChunkFrames(const uint32_t bufferFrames) const {
  // mFramesPerCallback is what the stream published: the size the application asked for, or the
  // burst it was given instead. A buffer that already fits in one callback is served as one, and
  // the last chunk of a buffer that is not a multiple of the callback size is shorter rather
  // than longer.
  const uint32_t published = mFramesPerCallback != kUnspecified ? mFramesPerCallback : mFramesPerBurst;
  if (published == 0 || published >= bufferFrames) {
    return bufferFrames;
  }
  return published;
}

void AudioStreamWasapi::pumpRender() {
  std::shared_lock<std::shared_mutex> lock(mClientLock);
  if (mAudioClient == nullptr || mRenderClient == nullptr) {
    return;
  }

  // The frames the endpoint is waiting for. A shared-mode stream frees its buffer a period at a
  // time, so what it is waiting for is what the engine played since the last pass. An
  // exclusive-mode stream that is event driven is a ping-pong of two buffers instead: the
  // endpoint hands one of them over per event and the whole of it is filled, so
  // `GetCurrentPadding` is not consulted there. It describes the frames that are already queued,
  // not the buffer being served, and reading it as though it did can leave a pass with nothing
  // to do and the endpoint waiting for a buffer that is never handed back.
  UINT32 frames = mDeviceBufferFrames;
  if (mEstablishedSharingMode != SharingMode::Exclusive) {
    UINT32 padding = 0;
    const HRESULT paddingResult = mAudioClient->GetCurrentPadding(&padding);
    if (FAILED(paddingResult)) {
      lock.unlock();
      reportDeviceError(resultFromHresult(paddingResult), "GetCurrentPadding");
      return;
    }
    frames = mDeviceBufferFrames > padding ? mDeviceBufferFrames - padding : 0;
  }
  if (frames == 0) {
    return;
  }

  BYTE* data = nullptr;
  HRESULT hr = mRenderClient->GetBuffer(frames, &data);
  if (FAILED(hr)) {
    lock.unlock();
    reportDeviceError(resultFromHresult(hr), "IAudioRenderClient::GetBuffer");
    return;
  }

  DWORD releaseFlags = 0;
  bool stopRequested = false;

  if (isCallbackMode()) {
    // The application owns the buffer for the duration of the callback, so the client
    // lock is dropped: it may call back into the stream, and every other entry point
    // takes the lock. The callback size is read here, while the device is still the one
    // this buffer belongs to.
    //
    // The device buffer is as large as the endpoint's buffer, and the application only
    // promised to handle the callback size it asked for: it is served in chunks of that size
    // rather than in one buffer that may be many times larger. `chunkFrames` is never zero,
    // because the buffer it describes is not empty.
    const uint32_t chunkFrames = callbackChunkFrames(frames);
    const size_t bytesPerFrame = getBytesPerFrame();
    const auto generation = mClientGeneration.load(std::memory_order_acquire);
    lock.unlock();
    if (isDataCallbackEnabled()) {
      for (uint32_t offset = 0; offset < frames; offset += chunkFrames) {
        const uint32_t chunk = std::min(chunkFrames, frames - offset);
        const DataCallbackResult callbackResult =
          fireDataCallback(data + static_cast<size_t>(offset) * bytesPerFrame, chunk);
        if (callbackResult == DataCallbackResult::Stop) {
          // Hand the buffer back as silence: releasing stale audio would repeat the last
          // period of whatever the callback had produced before it stopped.
          releaseFlags = AUDCLNT_BUFFERFLAGS_SILENT;
          stopRequested = true;
          break;
        }
      }
      mFramesWritten.fetch_add(frames);
    } else {
      releaseFlags = AUDCLNT_BUFFERFLAGS_SILENT;
    }
    lock.lock();
    if (mRenderClient == nullptr ||
        mClientGeneration.load(std::memory_order_acquire) != generation) {
      // The stream was closed or released from inside the callback. `frames` describes the
      // buffer of the client that is gone, so there is nothing left to release it to: a
      // stream that was opened again in the meantime has a buffer of its own.
      return;
    }
  } else {
    size_t filled = 0;
    {
      std::lock_guard<std::mutex> bufferLock(mBufferLock);
      filled = popFromRingBuffer(data, frames);
    }
    if (filled < frames) {
      // Nothing left to play: the tail is silence, and a buffer the device had to be
      // given without data is an underrun.
      std::memset(data + filled * getBytesPerFrame(), 0, (frames - filled) * getBytesPerFrame());
      if (filled == 0) {
        releaseFlags = AUDCLNT_BUFFERFLAGS_SILENT;
      }
      mXRunCount.fetch_add(1);
    }
    mBufferSpaceAvailable.notify_all();
  }

  hr = mRenderClient->ReleaseBuffer(frames, releaseFlags);
  if (FAILED(hr)) {
    lock.unlock();
    reportDeviceError(resultFromHresult(hr), "IAudioRenderClient::ReleaseBuffer");
    return;
  }

  if (!isCallbackMode()) {
    // In blocking mode the frames the device took are counted here; in callback mode the
    // callback produced them and the device clock reports what was actually played.
    mFramesRead.fetch_add(static_cast<int64_t>(frames));
  }

  lock.unlock();

  if (stopRequested) {
    // Ask for the stop from this thread. The pump has already been told not to
    // reschedule itself, because the state is no longer Started.
    setState(StreamState::Stopping);
    std::shared_ptr<EventHandler> handler;
    {
      std::lock_guard<std::mutex> stateLock(mLock);
      handler = mHandler;
    }
    if (handler != nullptr && !handler->sendEmptyMessage(MSG_STOP)) {
      // The queue is gone, so nothing will move the state machine: settle it here.
      setState(StreamState::Stopped);
    }
  }
}

void AudioStreamWasapi::pumpCapture() {
  std::shared_lock<std::shared_mutex> lock(mClientLock);
  if (mAudioClient == nullptr || mCaptureClient == nullptr) {
    return;
  }

  const size_t bytesPerFrame = getBytesPerFrame();
  // The zeroed frames a silent packet is served from were sized when the stream was
  // prepared. A packet larger than that is truncated rather than grown, because growing it
  // here would allocate on the thread the device is waiting for.
  const auto maxSilenceFrames = bytesPerFrame > 0
                                  ? static_cast<UINT32>(mSilenceBuffer.size() / bytesPerFrame)
                                  : 0;
  UINT32 packetFrames = 0;
  HRESULT hr = mCaptureClient->GetNextPacketSize(&packetFrames);
  while (SUCCEEDED(hr) && packetFrames > 0) {
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    hr = mCaptureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
    if (FAILED(hr)) {
      lock.unlock();
      reportDeviceError(resultFromHresult(hr), "IAudioCaptureClient::GetBuffer");
      return;
    }

    if (frames == 0) {
      mCaptureClient->ReleaseBuffer(0);
      break;
    }
    // The client is told what it handed over, whether or not the packet is passed on in
    // full: a truncated silent packet still has to be released with its own frame count.
    const UINT32 releasedFrames = frames;

    // A silent packet has undefined contents, so it is replaced by zeroed frames before
    // the application can see it.
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    if (silent) {
      if (frames > maxSilenceFrames) {
        if (!mReportedOversizedSilencePacket) {
          mReportedOversizedSilencePacket = true;
          MINILOG_WARN("A silent capture packet of {} frames exceeds the {} frames the stream prepared; "
                       "it is truncated rather than grown on the stream thread",
            frames, maxSilenceFrames);
        }
        frames = maxSilenceFrames;
        if (frames == 0) {
          mCaptureClient->ReleaseBuffer(releasedFrames);
          break;
        }
      }
      data = mSilenceBuffer.data();
    }

    bool stopRequested = false;
    // A packet can cover more frames than the application asked to handle per callback, so it
    // is delivered in chunks of the published callback size, exactly as a render buffer is.
    // The size is read here, while the lock still describes the client that owns the packet.
    const uint32_t chunkFrames = callbackChunkFrames(frames);
    if (isCallbackMode()) {
      bool consumed = false;
      const auto generation = mClientGeneration.load(std::memory_order_acquire);
      lock.unlock();
      if (isDataCallbackEnabled()) {
        for (uint32_t offset = 0; offset < frames; offset += chunkFrames) {
          const uint32_t chunk = std::min(chunkFrames, frames - offset);
          if (fireDataCallback(data + static_cast<size_t>(offset) * bytesPerFrame, chunk) ==
              DataCallbackResult::Stop) {
            stopRequested = true;
            break;
          }
        }
        consumed = true;
      }
      lock.lock();
      if (mCaptureClient == nullptr ||
          mClientGeneration.load(std::memory_order_acquire) != generation) {
        // The stream was closed or released from inside the callback, so the packet belongs
        // to a client that is gone: a stream that was opened again has packets of its own.
        return;
      }
      if (consumed) {
        // The callback took the frames as they arrived, so nothing is left waiting: for a
        // capture stream in this mode the frames received and the frames consumed are the
        // same, and both counters report them.
        mFramesWritten.fetch_add(frames);
        mFramesRead.fetch_add(frames);
      }
    } else {
      size_t stored = 0;
      {
        std::lock_guard<std::mutex> bufferLock(mBufferLock);
        stored = pushToRingBuffer(data, frames);
      }
      if (stored < frames) {
        // The application did not read in time: what does not fit is discarded.
        mXRunCount.fetch_add(1);
      }
      mFramesWritten.fetch_add(frames);
      mBufferDataAvailable.notify_all();
    }

    mCaptureClient->ReleaseBuffer(releasedFrames);
    if (stopRequested) {
      setState(StreamState::Stopping);
      std::shared_ptr<EventHandler> handler;
      {
        std::lock_guard<std::mutex> stateLock(mLock);
        handler = mHandler;
      }
      if (handler != nullptr && !handler->sendEmptyMessage(MSG_STOP)) {
        setState(StreamState::Stopped);
      }
      return;
    }

    hr = mCaptureClient->GetNextPacketSize(&packetFrames);
  }

  if (FAILED(hr)) {
    lock.unlock();
    reportDeviceError(resultFromHresult(hr), "IAudioCaptureClient::GetNextPacketSize");
  }
}

void AudioStreamWasapi::promotePumpThread() {
  if (mThreadPriority.isValid()) {
    return;
  }
  mThreadPriority = AudioThreadPriority::promoteCurrentThread(mFramesPerBurst,
    static_cast<uint32_t>(getSampleRate()));
  if (!mThreadPriority.isValid()) {
    MINILOG_DEBUG("The stream thread could not be raised to real-time priority");
  }
}

void AudioStreamWasapi::demotePumpThread() {
  if (!mThreadPriority.isValid()) {
    return;
  }
  // Both the promotion and the demotion are thread affine: this runs on the stream
  // thread, which is the thread that was promoted.
  if (!AudioThreadPriority::demoteCurrentThread(mThreadPriority)) {
    MINILOG_DEBUG("The stream thread priority could not be restored");
  }
  mThreadPriority = AudioThreadPriority::Handle{};
}

// =============================================================================
//  Errors
// =============================================================================

void AudioStreamWasapi::reportDeviceError(const Result error, const char* reason) {
  // Only the first failure is delivered: the ones that follow are the same device
  // failing again, and the application is told once.
  if (mFatalErrorReported.exchange(true)) {
    return;
  }

  mErrorCallbackResult = error;
  mPendingError = error;
  MINILOG_ERROR("WASAPI {} failed: {}", reason, convertToText(error));

  std::shared_ptr<EventHandler> handler;
  {
    std::lock_guard<std::mutex> lock(mLock);
    handler = mErrorHandler;
    // Publish the failure so that a caller waiting for a transition learns about it
    // instead of waiting for a state change that will never come.
    setState_l(StreamState::Disconnected);
  }
  mStateChanged.notify_all();

  if (handler != nullptr) {
    if (!handler->sendEmptyMessage(MSG_NOTIFY_ERROR)) {
      MINILOG_WARN("The error callback could not be scheduled");
    }
  } else {
    MINILOG_DEBUG("No error callback was set, so the failure was only logged");
  }
}

void AudioStreamWasapi::onNotifyError() {
  const auto errorCallback = getErrorCallback();
  if (errorCallback == nullptr) {
    return;
  }

  const Result error = mPendingError;
  if (errorCallback->onError(this, error)) {
    // The application took over; it will stop and close the stream itself.
    return;
  }

  errorCallback->onErrorBeforeClose(this, error);
  close();
  // Warning: the application is allowed to drop the stream from `onErrorBeforeClose()`,
  // which would leave `this` dangling from here on. This is the same restriction the
  // other platform implementations document.
  errorCallback->onErrorAfterClose(this, error);
}

// =============================================================================
//  Data path
// =============================================================================

size_t AudioStreamWasapi::pushToRingBuffer(const uint8_t* source, const size_t frames) {
  if (source == nullptr || frames == 0) {
    return 0;
  }
  // `AudioRingBuffer::write()` is all or nothing, so it is only asked for what fits.
  const size_t fit = std::min(frames, mRingBuffer.space());
  if (fit == 0) {
    return 0;
  }

  bool stored = false;
  switch (getFormat()) {
    case AudioSampleFormat::kFloat: {
      stored = mRingBuffer.write(reinterpret_cast<const float*>(source), fit);
      break;
    }
    case AudioSampleFormat::kInt16: {
      stored = mRingBuffer.writeInt16(reinterpret_cast<const int16_t*>(source), fit);
      break;
    }
    case AudioSampleFormat::kInt24: {
      stored = mRingBuffer.writeInt24(source, fit);
      break;
    }
    case AudioSampleFormat::kInt32: {
      stored = mRingBuffer.writeInt32(reinterpret_cast<const int32_t*>(source), fit);
      break;
    }
    default: {
      break;
    }
  }
  return stored ? fit : 0;
}

size_t AudioStreamWasapi::popFromRingBuffer(uint8_t* destination, const size_t frames) {
  if (destination == nullptr || frames == 0) {
    return 0;
  }
  switch (getFormat()) {
    case AudioSampleFormat::kFloat:
      return mRingBuffer.read(reinterpret_cast<float*>(destination), frames);
    case AudioSampleFormat::kInt16:
      return mRingBuffer.readInt16(reinterpret_cast<int16_t*>(destination), frames);
    case AudioSampleFormat::kInt24:
      return mRingBuffer.readInt24(destination, frames);
    case AudioSampleFormat::kInt32:
      return mRingBuffer.readInt32(reinterpret_cast<int32_t*>(destination), frames);
    default:
      return 0;
  }
}

void AudioStreamWasapi::resetRingBuffer() {
  {
    std::lock_guard<std::mutex> lock(mBufferLock);
    mRingBuffer.reset();
  }
  // A caller blocked in read() or write() has to be woken up: the data or the space it
  // is waiting for is never going to arrive.
  mBufferSpaceAvailable.notify_all();
  mBufferDataAvailable.notify_all();
}

OpResult<uint32_t> AudioStreamWasapi::write(const void* buffer,
  const uint32_t numFrames,
  const int64_t timeoutNanoseconds) {
  if (buffer == nullptr) {
    return Result::ErrorNull;
  }
  if (!isRender() || isCallbackMode()) {
    return Result::ErrorInvalidState;
  }
  if (mState.load() != StreamState::Started) {
    return Result::ErrorInvalidState;
  }
  if (numFrames == 0) {
    return OpResult(0u);
  }

  const size_t bytesPerFrame = getBytesPerFrame();
  const auto* source = static_cast<const uint8_t*>(buffer);
  const int64_t startedAt = AudioUtils::getNanoseconds();
  const int64_t deadline = startedAt + std::max<int64_t>(timeoutNanoseconds, 0);

  uint32_t written = 0;
  std::unique_lock<std::mutex> lock(mBufferLock);
  while (written < numFrames) {
    const size_t accepted =
      pushToRingBuffer(source + static_cast<size_t>(written) * bytesPerFrame, numFrames - written);
    if (accepted > 0) {
      written += static_cast<uint32_t>(accepted);
      continue;
    }

    if (timeoutNanoseconds <= 0 || mState.load() != StreamState::Started) {
      break;
    }
    const int64_t remaining = deadline - AudioUtils::getNanoseconds();
    if (remaining <= 0) {
      break;
    }
    // The device drains the buffer on its own thread and signals this. The predicate is what
    // keeps a signal that arrives between the attempt above and the wait from being missed,
    // which would otherwise cost the caller the rest of its timeout.
    mBufferSpaceAvailable.wait_for(lock, std::chrono::nanoseconds(remaining),
      [this] { return mRingBuffer.space() > 0; });
  }
  lock.unlock();

  if (written > 0) {
    // The frames the application handed over are what `getFramesWritten()` reports on a
    // render stream; the frames the endpoint played are counted by the stream thread.
    mFramesWritten.fetch_add(written);
    mBufferSpaceAvailable.notify_all();
    return OpResult(written);
  }
  return Result::ErrorTimeout;
}

OpResult<uint32_t> AudioStreamWasapi::read(void* buffer,
  const uint32_t numFrames,
  const int64_t timeoutNanoseconds) {
  if (buffer == nullptr) {
    return Result::ErrorNull;
  }
  if (isRender() || isCallbackMode()) {
    return Result::ErrorInvalidState;
  }
  if (mState.load() != StreamState::Started) {
    return Result::ErrorInvalidState;
  }
  if (numFrames == 0) {
    return OpResult(0u);
  }

  const size_t bytesPerFrame = getBytesPerFrame();
  auto* destination = static_cast<uint8_t*>(buffer);
  const int64_t startedAt = AudioUtils::getNanoseconds();
  const int64_t deadline = startedAt + std::max<int64_t>(timeoutNanoseconds, 0);

  uint32_t framesRead = 0;
  std::unique_lock<std::mutex> lock(mBufferLock);
  while (framesRead < numFrames) {
    const size_t got =
      popFromRingBuffer(destination + static_cast<size_t>(framesRead) * bytesPerFrame, numFrames - framesRead);
    if (got > 0) {
      framesRead += static_cast<uint32_t>(got);
      continue;
    }

    if (timeoutNanoseconds <= 0 || mState.load() != StreamState::Started) {
      break;
    }
    const int64_t remaining = deadline - AudioUtils::getNanoseconds();
    if (remaining <= 0) {
      break;
    }
    // The device fills the buffer on its own thread and signals this. The predicate is what
    // keeps a signal that arrives between the attempt above and the wait from being missed,
    // which would otherwise cost the caller the rest of its timeout.
    mBufferDataAvailable.wait_for(lock, std::chrono::nanoseconds(remaining),
      [this] { return mRingBuffer.available() > 0; });
  }
  lock.unlock();

  if (framesRead > 0) {
    mFramesRead.fetch_add(framesRead);
    mBufferDataAvailable.notify_all();
    return OpResult(framesRead);
  }
  return Result::ErrorTimeout;
}

// =============================================================================
//  Queries
// =============================================================================

uint32_t AudioStreamWasapi::getBufferSizeInFrames() {
  return mBufferSizeInFrames;
}

OpResult<uint32_t> AudioStreamWasapi::setBufferSizeInFrames(const uint32_t requestedFrames) {
  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  // WASAPI sizes the buffer at initialization, so the only honest answer is the size
  // that is in use. It is not a failure: the caller learns the real value either way.
  if (requestedFrames > mBufferCapacityInFrames) {
    MINILOG_DEBUG("setBufferSizeInFrames({}) exceeds the capacity of {}", requestedFrames,
      mBufferCapacityInFrames);
  }
  return OpResult(mBufferSizeInFrames);
}

OpResult<uint32_t> AudioStreamWasapi::getXRunCount() {
  if (!isXRunCountSupported()) {
    return Result::ErrorUnimplemented;
  }
  return OpResult(mXRunCount.load());
}

bool AudioStreamWasapi::isXRunCountSupported() const {
  // Only blocking mode can see a missed deadline: in callback mode the engine owns the
  // buffer, and it neither reports a late callback nor a partial one.
  return !isDataCallbackSpecified();
}

void AudioStreamWasapi::updateFramesWritten() {
  // Nothing to refresh: every frame counted here is counted by the thread that moved it,
  // which is the stream thread, and it is counted as it moves.
}

Result AudioStreamWasapi::readDeviceClock(int64_t& framePosition, uint64_t& qpc) {
  // The caller holds `mClientLock`.
  if (mAudioClock == nullptr) {
    return Result::ErrorUnimplemented;
  }

  UINT64 position = 0;
  UINT64 clockQpc = 0;
  const HRESULT hr = mAudioClock->GetPosition(&position, &clockQpc);
  if (FAILED(hr)) {
    return resultFromHresult(hr);
  }

  UINT64 clockFrequency = 0;
  const HRESULT frequencyResult = mAudioClock->GetFrequency(&clockFrequency);
  if (FAILED(frequencyResult)) {
    return resultFromHresult(frequencyResult);
  }

  // `IAudioClock::GetPosition` reports the position in device clock ticks, not in frames: the
  // frequency is the device's own (192 kHz on many endpoints, not the 48 kHz the stream runs at).
  // Counting the ticks as frames made the counters that read this advance several times faster
  // than the frames the stream was actually fed, which the sink reported as a playback position
  // that ran ahead of the media.
  const auto frames = static_cast<int64_t>(framesFromClockPosition(position, clockFrequency,
    static_cast<uint64_t>(getSampleRate())));
  {
    // The frequency is the part a clock problem hides in: it is the endpoint's own, so a
    // report that the counters advance too fast or too slowly is read against it.
    static std::atomic_int sClockCount{ 0 };
    if (sClockCount.fetch_add(1, std::memory_order_relaxed) < 5) {
      MINILOG_DEBUG("The device clock reports {} ticks at {} Hz, which is {} frames at {} Hz",
        static_cast<int64_t>(position), static_cast<int64_t>(clockFrequency), frames,
        static_cast<int64_t>(getSampleRate()));
    }
  }

  // A reset restarts the device clock, and the counters this feeds are documented as monotonic,
  // so what the previous clock run reported is carried over instead of being lost. Only the
  // exchange that observes the decrease adds, so one restart is counted once even when two
  // threads query the clock at the same time.
  const auto previous = static_cast<int64_t>(mLastClockPosition.exchange(static_cast<uint64_t>(frames)));
  if (frames < previous) {
    mFramesPlayedBase.fetch_add(previous);
  }

  framePosition = mFramesPlayedBase.load() + frames;
  qpc = clockQpc;
  return Result::OK;
}

void AudioStreamWasapi::updateFramesRead() {
  // Only a render stream plays frames, and only the frames that were played are counted here.
  if (!isRender()) {
    return;
  }
  ensureComInitialized();

  std::shared_lock<std::shared_mutex> lock(mClientLock);

  if (!isCallbackMode()) {
    // The endpoint reports how much of what the stream handed over is still queued in it, so what is left
    // is what the device played. Without this counter the sink that reports the playback position falls
    // back to the frames written, i.e. it reports the audio that is still buffered as if it had been
    // played: the media clock runs ahead of the media by the whole buffer, the frames of the video are
    // all late by that much, and they are dropped from the first seconds of every playback.
    if (mAudioClient == nullptr) {
      return;
    }
    UINT32 padding = 0;
    if (FAILED(mAudioClient->GetCurrentPadding(&padding))) {
      return;
    }
    const int64_t played = mFramesWritten.load() - static_cast<int64_t>(padding);
    int64_t current = mFramesRead.load();
    while (played > current && !mFramesRead.compare_exchange_weak(current, played)) {
    }
    return;
  }

  int64_t played = 0;
  uint64_t qpc = 0;
  if (readDeviceClock(played, qpc) != Result::OK) {
    return;
  }
  int64_t current = mFramesRead.load();
  while (played > current && !mFramesRead.compare_exchange_weak(current, played)) {
  }
}

Result AudioStreamWasapi::getTimestamp(const ClockMode mode,
  int64_t* framePosition,
  int64_t* timeNanoseconds) {
  if (mState.load() != StreamState::Started) {
    return Result::ErrorInvalidState;
  }
  ensureComInitialized();

  std::shared_lock<std::shared_mutex> lock(mClientLock);

  // The clock is read exactly as `getFramesRead()` reads it, so the two report one basis: a
  // position in frames of the stream's own timeline, and the performance counter value that
  // goes with it.
  int64_t position = 0;
  uint64_t qpc = 0;
  const auto result = readDeviceClock(position, qpc);
  if (result != Result::OK) {
    return result;
  }
  if (qpc == 0) {
    // The endpoint has not recorded a position yet, which is what it reports right after the
    // stream started, and it hands out no host time with it. Reporting the zero would date
    // these frames to the epoch of the clock mode (1970 in realtime mode), so the caller is
    // told that there is no timestamp yet instead.
    return Result::ErrorUnavailable;
  }

  if (framePosition != nullptr) {
    *framePosition = position;
  }
  if (timeNanoseconds != nullptr) {
    *timeNanoseconds = mode == ClockMode::Realtime ? qpcToRealtimeNanos(qpc) : qpcToNanos(qpc);
  }
  return Result::OK;
}

OpResult<double> AudioStreamWasapi::calculateLatencyMillis() {
  if (mState.load() != StreamState::Started) {
    return Result::ErrorInvalidState;
  }
  const auto sampleRate = static_cast<uint32_t>(getSampleRate());
  if (sampleRate == 0) {
    return Result::ErrorInvalidState;
  }
  ensureComInitialized();

  std::shared_lock<std::shared_mutex> lock(mClientLock);
  if (mAudioClient == nullptr) {
    return Result::ErrorClosed;
  }

  // How much audio is between the application and the speaker, or between the microphone
  // and the application: what the endpoint still holds plus the period it is working on.
  uint32_t pendingFrames = 0;
  HRESULT hr = E_FAIL;
  if (isRender()) {
    hr = mAudioClient->GetCurrentPadding(&pendingFrames);
  } else if (mCaptureClient != nullptr) {
    hr = mCaptureClient->GetNextPacketSize(&pendingFrames);
  }
  if (FAILED(hr)) {
    return resultFromHresult(hr);
  }

  const double frames = static_cast<double>(pendingFrames) + static_cast<double>(mFramesPerBurst);
  return OpResult(frames * 1000.0 / static_cast<double>(sampleRate));
}

// =============================================================================
//  Helpers
// =============================================================================

Result AudioStreamWasapi::resultFromHresult(const HRESULT hr) {
  if (SUCCEEDED(hr)) {
    return Result::OK;
  }
  if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
    return Result::ErrorDisconnected;
  }
  if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
    return Result::ErrorInvalidFormat;
  }
  if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
    return Result::ErrorUnavailable;
  }
  if (hr == AUDCLNT_E_ENDPOINT_CREATE_FAILED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
    return Result::ErrorNoService;
  }
  if (hr == AUDCLNT_E_NOT_INITIALIZED ||
      hr == AUDCLNT_E_NOT_STOPPED ||
      hr == AUDCLNT_E_OUT_OF_ORDER) {
    return Result::ErrorInvalidState;
  }
  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED ||
      hr == AUDCLNT_E_INVALID_DEVICE_PERIOD ||
      hr == AUDCLNT_E_BUFFER_SIZE_ERROR) {
    return Result::ErrorInvalidRate;
  }
  if (hr == E_OUTOFMEMORY) {
    return Result::ErrorNoMemory;
  }
  if (hr == E_INVALIDARG) {
    return Result::ErrorIllegalArgument;
  }
  if (hr == E_POINTER) {
    return Result::ErrorNull;
  }
  if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
      hr == E_NOINTERFACE ||
      hr == REGDB_E_CLASSNOTREG) {
    return Result::ErrorUnavailable;
  }
  return Result::ErrorInternal;
}

uint32_t AudioStreamWasapi::framesFromHns(const REFERENCE_TIME hns, const uint32_t sampleRate) {
  if (hns <= 0 || sampleRate == 0) {
    return 0;
  }
  return static_cast<uint32_t>(hns * sampleRate / kHnsPerSecond);
}

REFERENCE_TIME AudioStreamWasapi::hnsFromFrames(const uint32_t frames, const uint32_t sampleRate) {
  if (frames == 0 || sampleRate == 0) {
    return 0;
  }
  return static_cast<REFERENCE_TIME>(frames) * kHnsPerSecond / sampleRate;
}

int64_t AudioStreamWasapi::qpcToNanos(const uint64_t qpc) {
  static const int64_t frequency = []() {
    LARGE_INTEGER value{};
    ::QueryPerformanceFrequency(&value);
    return value.QuadPart > 0 ? value.QuadPart : static_cast<LONGLONG>(kHnsPerSecond);
  }();
  // Split into seconds and remainder: multiplying a raw counter by a billion would
  // overflow after a few months of uptime.
  const auto value = static_cast<int64_t>(qpc);
  const int64_t seconds = value / frequency;
  const int64_t remainder = value % frequency;
  return seconds * Definitions::kNanosPerSecond + remainder * Definitions::kNanosPerSecond / frequency;
}

int64_t AudioStreamWasapi::qpcToRealtimeNanos(const uint64_t qpc) {
  // The device clock reports a performance counter value, which is monotonic since boot.
  // Sampling the wall clock next to it is what turns it into a realtime timestamp.
  LARGE_INTEGER nowQpc{};
  ::QueryPerformanceCounter(&nowQpc);
  FILETIME nowFileTime{};
  ::GetSystemTimeAsFileTime(&nowFileTime);

  ULARGE_INTEGER fileTime{};
  fileTime.LowPart = nowFileTime.dwLowDateTime;
  fileTime.HighPart = nowFileTime.dwHighDateTime;

  // The wall clock counts 100-nanosecond intervals since 1601, which no longer fits in
  // 64 bits of nanoseconds, so it is rebased on the Unix epoch first.
  constexpr int64_t kFileTimeToUnixEpoch = 116444736000000000LL;
  const int64_t nowNanos = (static_cast<int64_t>(fileTime.QuadPart) - kFileTimeToUnixEpoch) * 100;
  const int64_t offsetNanos = nowNanos - qpcToNanos(static_cast<uint64_t>(nowQpc.QuadPart));
  return qpcToNanos(qpc) + offsetNanos;
}

} // namespace coral
