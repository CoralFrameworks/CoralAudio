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

#include "AudioStreamCoreAudio.h"

#include <coral/audio/AudioUtils.h>

#include <media/Utilities.h>
#include <minilog/log.h>

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <sys/time.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace coral {

namespace {

/** Sample rate used when neither the builder nor the device supplies one. */
constexpr uint64_t kDefaultSampleRate = 48000;

/** Channel count used when neither the builder nor the device supplies one. */
constexpr uint32_t kDefaultChannelCount = 2;

/** Device buffer size assumed when the HAL does not report one. */
constexpr uint32_t kDefaultBufferFrames = 512;

/** The number of frames one input callback can receive, at worst. */
constexpr uint32_t kMinimumCaptureFrames = 4096;

UInt32 getDeviceUInt32(const AudioObjectID device,
  const AudioObjectPropertySelector selector,
  const AudioObjectPropertyScope scope) {
  AudioObjectPropertyAddress address{ selector, scope, kAudioObjectPropertyElementMain };
  UInt32 value = 0;
  UInt32 size = sizeof(value);
  const OSStatus status = AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value);
  return status == noErr ? value : 0;
}

/** Compares a device property that is a CFString with the value the environment asked for. */
bool matchesCFString(CFStringRef value, const char* expected) {
  if (value == nullptr || expected == nullptr) {
    return false;
  }
  char buffer[512];
  if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8) == 0) {
    return false;
  }
  // An exact UID or name wins; a substring makes the name easier to type in shell.
  return std::strcmp(buffer, expected) == 0 || std::strstr(buffer, expected) != nullptr;
}

std::vector<AudioDeviceID> allDevices() {
  AudioObjectPropertyAddress address{
    kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr ||
      size == 0) {
    return {};
  }
  std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) !=
      noErr) {
    return {};
  }
  return devices;
}

bool deviceHasNameOrUID(const AudioDeviceID device, const char* expected) {
  AudioObjectPropertyAddress address{
    kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  CFStringRef name = nullptr;
  UInt32 size = sizeof(name);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) == noErr && name != nullptr) {
    const bool match = matchesCFString(name, expected);
    CFRelease(name);
    if (match) {
      return true;
    }
  }

  address.mSelector = kAudioDevicePropertyDeviceUID;
  CFStringRef uid = nullptr;
  size = sizeof(uid);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &uid) == noErr && uid != nullptr) {
    const bool match = matchesCFString(uid, expected);
    CFRelease(uid);
    return match;
  }
  return false;
}

} // namespace anonymous

// =============================================================================
//  Construction
// =============================================================================

std::shared_ptr<AudioStream> AudioStreamCoreAudio::Create(const AudioStreamBuilder& builder) {
  return std::make_shared<AudioStreamCoreAudio>(builder);
}

AudioStreamCoreAudio::AudioStreamCoreAudio(const AudioStreamBuilder& builder) noexcept
  : AudioStream(builder),
    mRequestedFramesPerCallback(builder.getFramesPerDataCallback()),
    mRequestedBufferCapacity(builder.getBufferCapacityInFrames()),
    mRequestedSampleRate(builder.getSampleRate()),
    mRequestedChannelCount(builder.getChannelCount()),
    mRequestedFormat(builder.getFormat()) {
}

AudioStreamCoreAudio::~AudioStreamCoreAudio() {
  // Dropping a stream without closing it must not leave a thread or an audio unit behind.
  close();
}

// =============================================================================
//  Opening
// =============================================================================

Result AudioStreamCoreAudio::open() {
  std::lock_guard<std::mutex> lifecycle(mLifecycleLock);

  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  if (mState.load() != StreamState::Uninitialized || mAudioUnit != nullptr) {
    return Result::ErrorInvalidState;
  }

  const Result config = isValidConfig();
  if (config != Result::OK) {
    MINILOG_WARN("Invalid audio configuration: {}", convertToText(config));
    return config;
  }

  // The builder may leave any of these unspecified. Core Audio needs concrete values, and the stream
  // reports what the client format ended up as, so the device supplies the defaults where it can.
  mSampleRate = mRequestedSampleRate != 0 ? mRequestedSampleRate : kDefaultSampleRate;
  mChannelCount = mRequestedChannelCount != 0 ? mRequestedChannelCount : kDefaultChannelCount;
  mFormat = (mRequestedFormat == AudioSampleFormat::kDefault || mRequestedFormat == AudioSampleFormat::kInvalid)
              ? AudioSampleFormat::kFloat
              : mRequestedFormat;
  mFramesPerCallback = mRequestedFramesPerCallback;
  mBufferCapacityInFrames = mRequestedBufferCapacity;
  mBufferSizeInFrames = 0;
  mFramesPerBurst = 0;

  mFramesRead.store(0);
  mFramesWritten.store(0);
  mXRunCount.store(0);
  mFatalErrorReported.store(false);
  mDeviceRunning.store(false);
  mExitRequested = false;
  mCommand = Command::None;
  mTimestampFramePosition.store(0);
  mTimestampHostTime.store(0);
  mCaptureBuffer.clear();

  Result result = createUnit();
  if (result == Result::OK) {
    result = configureFormat();
  }
  if (result == Result::OK) {
    result = configureCallbacks();
  }
  if (result == Result::OK) {
    result = initializeUnit();
  }
  if (result != Result::OK) {
    teardownDevice();
    setState(StreamState::Uninitialized);
    return result;
  }

  // The device-is-alive listener is installed only once the unit is usable; before that, a missing
  // device is reported by the calls in createUnit()/configureFormat() instead.
  AudioObjectPropertyAddress aliveAddress{
    kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  if (AudioObjectAddPropertyListener(mDevice, &aliveAddress, deviceListenerEntry, this) == noErr) {
    mDeviceListenerInstalled = true;
  }

  // The HAL callback delivers at most one device buffer, but a device can be reconfigured while the
  // stream is open; sizing the scratch generously keeps the input callback from ever having to grow
  // it on the real-time thread.
  const size_t captureFrames = std::max(mDeviceBufferFrames, kMinimumCaptureFrames);
  if (!isRender()) {
    mCaptureBuffer.assign(captureFrames * getBytesPerFrame(), 0);
    mCaptureBufferList.mNumberBuffers = 1;
    mCaptureBufferList.mBuffers[0].mNumberChannels = mChannelCount;
    mCaptureBufferList.mBuffers[0].mDataByteSize = static_cast<UInt32>(mCaptureBuffer.size());
    mCaptureBufferList.mBuffers[0].mData = mCaptureBuffer.data();
  }

  if (!mRingBuffer.init(mBufferCapacityInFrames, mChannelCount)) {
    MINILOG_ERROR("The Core Audio stream could not allocate a ring buffer of {} frames",
      mBufferCapacityInFrames);
    teardownDevice();
    setState(StreamState::Uninitialized);
    return Result::ErrorNoMemory;
  }

  // Mach host times are monotonic, but their epoch is not the one `AudioUtils::getNanoseconds()`
  // uses. Both offsets are captured once, at the same instant, rather than queried per call.
  mach_timebase_info_data_t timebase{ 1, 1 };
  mach_timebase_info(&timebase);
  mTimebaseNumer = timebase.numer != 0 ? timebase.numer : 1;
  mTimebaseDenom = timebase.denom != 0 ? timebase.denom : 1;
  mMonotonicOffsetNanos = AudioUtils::getNanoseconds() - hostTimeToNanos(mach_absolute_time());
  timeval now{};
  ::gettimeofday(&now, nullptr);
  const int64_t realtimeNanos =
    static_cast<int64_t>(now.tv_sec) * Definitions::kNanosPerSecond + static_cast<int64_t>(now.tv_usec) * 1000;
  mRealtimeOffsetNanos = realtimeNanos - AudioUtils::getNanoseconds();

  result = startControlThread();
  if (result != Result::OK) {
    teardownDevice();
    setState(StreamState::Uninitialized);
    return result;
  }

  calculateDefaultDelayBeforeCloseMillis();
  setState(StreamState::Open);

  MINILOG_DEBUG("AudioStreamCoreAudio opened: direction={}, device={}, sampleRate={}, channels={}, format={}, "
                "deviceBufferFrames={}, bufferCapacity={}",
    convertToText(getDirection()), static_cast<unsigned int>(mDevice), static_cast<int>(getSampleRate()),
    static_cast<int>(getChannelCount()), convertToText(getFormat()),
    static_cast<int>(mDeviceBufferFrames), static_cast<int>(mBufferCapacityInFrames));
  return Result::OK;
}

Result AudioStreamCoreAudio::createUnit() {
  AudioComponentDescription description{};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_HALOutput;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;

  const AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (component == nullptr) {
    MINILOG_ERROR("Core Audio has no HAL output audio unit available");
    return Result::ErrorUnavailable;
  }

  OSStatus status = AudioComponentInstanceNew(component, &mAudioUnit);
  if (status != noErr) {
    mAudioUnit = nullptr;
    return resultFromOSStatus(status, "AudioComponentInstanceNew");
  }

  mDevice = selectDeviceFor(getDirection());
  if (mDevice == kAudioObjectUnknown) {
    MINILOG_ERROR("Core Audio has no default {} device", isRender() ? "output" : "input");
    return Result::ErrorUnavailable;
  }

  if (channelCountForDevice(mDevice, getDirection()) == 0) {
    MINILOG_ERROR("The Core Audio device {} has no {} streams", static_cast<unsigned int>(mDevice),
      isRender() ? "output" : "input");
    return Result::ErrorUnavailable;
  }

  status = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
    &mDevice, sizeof(mDevice));
  if (status != noErr) {
    return resultFromOSStatus(status, "kAudioOutputUnitProperty_CurrentDevice");
  }

  // The HAL unit has both elements; turning the unused one off is what makes the unit a pure render
  // or a pure capture unit.
  UInt32 enableOutput = isRender() ? 1 : 0;
  status = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
    &enableOutput, sizeof(enableOutput));
  if (status != noErr) {
    return resultFromOSStatus(status, "kAudioOutputUnitProperty_EnableIO (output)");
  }

  UInt32 enableInput = isRender() ? 0 : 1;
  status = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
    &enableInput, sizeof(enableInput));
  if (status != noErr) {
    return resultFromOSStatus(status, "kAudioOutputUnitProperty_EnableIO (input)");
  }
  return Result::OK;
}

Result AudioStreamCoreAudio::configureFormat() {
  // A rate or channel count the builder left unspecified follows the device. The client format may
  // still differ from the device format, because the unit converts between the two.
  if (mRequestedSampleRate == 0) {
    const Float64 deviceRate = sampleRateForDevice(mDevice);
    if (deviceRate > 0) {
      mSampleRate = static_cast<uint64_t>(deviceRate + 0.5);
    }
  }
  if (mRequestedChannelCount == 0) {
    mChannelCount = channelCountForDevice(mDevice, getDirection());
    if (mChannelCount == 0) {
      mChannelCount = kDefaultChannelCount;
    }
  }

  // For capture the unit's output scope of element 1 describes the device's own format; the HAL
  // wants it set explicitly before the client format is applied on the other side of the element.
  if (!isRender()) {
    AudioStreamBasicDescription hardware{};
    UInt32 hardwareSize = sizeof(hardware);
    OSStatus status = AudioUnitGetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
      &hardware, &hardwareSize);
    if (status == noErr) {
      status = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
        &hardware, sizeof(hardware));
    }
    if (status != noErr) {
      MINILOG_ERROR("Core Audio could not configure the input side of the device: {}",
        static_cast<int>(status));
      return resultFromOSStatus(status, "kAudioUnitProperty_StreamFormat (device side)");
    }
  }

  AudioStreamBasicDescription client = streamDescriptionFor(mSampleRate, mChannelCount, mFormat);
  const AudioUnitScope clientScope = isRender() ? kAudioUnitScope_Input : kAudioUnitScope_Output;
  const AudioUnitElement clientElement = isRender() ? 0 : 1;
  OSStatus status = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, clientScope,
    clientElement, &client, sizeof(client));
  if (status != noErr) {
    MINILOG_ERROR("Core Audio does not accept {} Hz, {} channels, format {} on the client side: {}",
      static_cast<int>(mSampleRate), static_cast<int>(mChannelCount), convertToText(mFormat),
      static_cast<int>(status));
    return Result::ErrorInvalidFormat;
  }

  AudioStreamBasicDescription accepted{};
  UInt32 acceptedSize = sizeof(accepted);
  if (AudioUnitGetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, clientScope, clientElement,
        &accepted, &acceptedSize) == noErr) {
    if (accepted.mSampleRate > 0) {
      mSampleRate = static_cast<uint64_t>(accepted.mSampleRate + 0.5);
    }
    if (accepted.mChannelsPerFrame > 0) {
      mChannelCount = accepted.mChannelsPerFrame;
    }
  }

  mDeviceBufferFrames = bufferFramesForDevice(mDevice);
  mFramesPerBurst = mDeviceBufferFrames;
  // The value the application asked for is a bound, not a promise: the HAL hands over one device
  // buffer at a time, so a larger callback is delivered as one callback per buffer.
  mFramesPerCallback = mRequestedFramesPerCallback != 0
                         ? std::min(mRequestedFramesPerCallback, mDeviceBufferFrames)
                         : mDeviceBufferFrames;
  mBufferCapacityInFrames = std::max(mDeviceBufferFrames, mRequestedBufferCapacity);
  mBufferSizeInFrames = mDeviceBufferFrames;
  return Result::OK;
}

Result AudioStreamCoreAudio::configureCallbacks() {
  AURenderCallbackStruct callback{};
  OSStatus status = noErr;
  if (isRender()) {
    callback.inputProc = renderCallbackEntry;
    callback.inputProcRefCon = this;
    status = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
      &callback, sizeof(callback));
    if (status != noErr) {
      return resultFromOSStatus(status, "kAudioUnitProperty_SetRenderCallback");
    }
  } else {
    callback.inputProc = inputCallbackEntry;
    callback.inputProcRefCon = this;
    status = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_SetInputCallback,
      kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    if (status != noErr) {
      return resultFromOSStatus(status, "kAudioOutputUnitProperty_SetInputCallback");
    }
  }
  return Result::OK;
}

Result AudioStreamCoreAudio::initializeUnit() {
  const OSStatus status = AudioUnitInitialize(mAudioUnit);
  if (status != noErr) {
    return resultFromOSStatus(status, "AudioUnitInitialize");
  }
  return Result::OK;
}

Result AudioStreamCoreAudio::startControlThread() {
  try {
    mControlThread = std::thread(&AudioStreamCoreAudio::controlLoop, this);
  } catch (const std::system_error& error) {
    MINILOG_ERROR("Could not create the Core Audio control thread: {}", error.what());
    return Result::ErrorNoMemory;
  }
  return Result::OK;
}

void AudioStreamCoreAudio::stopControlThread() {
  if (!mControlThread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mLock);
    mExitRequested = true;
    mCommandAvailable.notify_all();
  }
  if (std::this_thread::get_id() == mControlThread.get_id()) {
    // Closing from the control thread cannot join itself; the same restriction the other backends
    // document applies. The thread observes the exit flag when its current command returns.
    return;
  }
  mControlThread.join();
}

// =============================================================================
//  Teardown
// =============================================================================

Result AudioStreamCoreAudio::release() {
  std::lock_guard<std::mutex> lifecycle(mLifecycleLock);

  if (mControlThread.joinable() && std::this_thread::get_id() == mControlThread.get_id()) {
    MINILOG_ERROR("AudioStreamCoreAudio::release() was called from its own control thread");
    requestStop();
    return Result::ErrorInvalidState;
  }

  if (mState.load() == StreamState::Uninitialized) {
    return Result::OK;
  }
  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }

  {
    std::lock_guard<std::mutex> lock(mLock);
    mState.store(StreamState::Closing);
    mExitRequested = true;
    mCommandAvailable.notify_all();
  }
  mStateChanged.notify_all();
  notifyBufferWaiters();

  stopControlThread();
  teardownDevice();
  resetRingBuffer();
  setState(StreamState::Uninitialized);
  return Result::OK;
}

Result AudioStreamCoreAudio::close() {
  std::lock_guard<std::mutex> lifecycle(mLifecycleLock);

  if (mControlThread.joinable() && std::this_thread::get_id() == mControlThread.get_id()) {
    MINILOG_ERROR("AudioStreamCoreAudio::close() was called from its own control thread");
    requestStop();
    return Result::ErrorInvalidState;
  }

  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }

  {
    std::lock_guard<std::mutex> lock(mLock);
    mState.store(StreamState::Closing);
    mExitRequested = true;
    mCommandAvailable.notify_all();
  }
  mStateChanged.notify_all();
  notifyBufferWaiters();

  stopControlThread();
  teardownDevice();
  resetRingBuffer();
  setState(StreamState::Closed);
  return Result::OK;
}

void AudioStreamCoreAudio::teardownDevice() {
  mDeviceRunning.store(false);
  setDataCallbackEnabled(false);

  if (mDeviceListenerInstalled) {
    AudioObjectPropertyAddress aliveAddress{
      kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListener(mDevice, &aliveAddress, deviceListenerEntry, this);
    mDeviceListenerInstalled = false;
  }

  if (mAudioUnit != nullptr) {
    (void) AudioOutputUnitStop(mAudioUnit);
    (void) AudioUnitUninitialize(mAudioUnit);
    (void) AudioComponentInstanceDispose(mAudioUnit);
    mAudioUnit = nullptr;
  }

  mDevice = kAudioObjectUnknown;
  mDeviceBufferFrames = 0;
  mCaptureBuffer.clear();
  mCaptureBufferList = ::AudioBufferList{};
  mFramesPerBurst = 0;
}

// =============================================================================
//  State machine
// =============================================================================

StreamState AudioStreamCoreAudio::getState() {
  return mState.load();
}

Result AudioStreamCoreAudio::waitForStateChange(const StreamState inputState,
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

Result AudioStreamCoreAudio::requestStart() {
  std::lock_guard<std::mutex> lock(mLock);

  switch (mState.load()) {
    case StreamState::Open:
    case StreamState::Paused:
    case StreamState::Stopped:
    case StreamState::Flushed:
      break;
    case StreamState::Starting:
    case StreamState::Started:
      return Result::OK;
    case StreamState::Closed:
      return Result::ErrorClosed;
    default:
      return Result::ErrorInvalidState;
  }
  if (mAudioUnit == nullptr || !mControlThread.joinable()) {
    return Result::ErrorClosed;
  }

  mState.store(StreamState::Starting);
  mCommand = Command::Start;
  mCommandAvailable.notify_one();
  mStateChanged.notify_all();
  notifyBufferWaiters();
  return Result::OK;
}

Result AudioStreamCoreAudio::requestPause() {
  std::lock_guard<std::mutex> lock(mLock);

  switch (mState.load()) {
    case StreamState::Started:
    case StreamState::Starting:
      break;
    case StreamState::Pausing:
    case StreamState::Paused:
      return Result::OK;
    case StreamState::Closed:
      return Result::ErrorClosed;
    default:
      return Result::ErrorInvalidState;
  }
  if (mAudioUnit == nullptr || !mControlThread.joinable()) {
    return Result::ErrorClosed;
  }

  mState.store(StreamState::Pausing);
  mCommand = Command::Pause;
  mCommandAvailable.notify_one();
  mStateChanged.notify_all();
  notifyBufferWaiters();
  return Result::OK;
}

Result AudioStreamCoreAudio::requestFlush() {
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
    case StreamState::Closed:
      return Result::ErrorClosed;
    default:
      return Result::ErrorInvalidState;
  }
  if (mAudioUnit == nullptr || !mControlThread.joinable()) {
    return Result::ErrorClosed;
  }

  mState.store(StreamState::Flushing);
  mCommand = Command::Flush;
  mCommandAvailable.notify_one();
  mStateChanged.notify_all();
  notifyBufferWaiters();
  return Result::OK;
}

Result AudioStreamCoreAudio::requestStop() {
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
    case StreamState::Closed:
      return Result::ErrorClosed;
    default:
      return Result::ErrorInvalidState;
  }
  if (mAudioUnit == nullptr || !mControlThread.joinable()) {
    return Result::ErrorClosed;
  }

  mState.store(StreamState::Stopping);
  mCommand = Command::Stop;
  mCommandAvailable.notify_one();
  mStateChanged.notify_all();
  notifyBufferWaiters();
  return Result::OK;
}

void AudioStreamCoreAudio::setState(const StreamState state) {
  {
    std::lock_guard<std::mutex> lock(mLock);
    mState.store(state);
  }
  mStateChanged.notify_all();
  notifyBufferWaiters();
}

void AudioStreamCoreAudio::notifyBufferWaiters() {
  mBufferSpaceAvailable.notify_all();
  mBufferDataAvailable.notify_all();
}

// =============================================================================
//  Control thread
// =============================================================================

void AudioStreamCoreAudio::controlLoop() {
  for (;;) {
    Command command = Command::None;
    {
      std::unique_lock<std::mutex> lock(mLock);
      mCommandAvailable.wait(lock, [this] { return mExitRequested || mCommand != Command::None; });
      if (mExitRequested) {
        break;
      }
      command = mCommand;
      mCommand = Command::None;
    }

    switch (command) {
      case Command::Start:
        applyStart();
        break;
      case Command::Pause:
        applyPause();
        break;
      case Command::Flush:
        applyFlush();
        break;
      case Command::Stop:
        applyStop();
        break;
      case Command::None:
        break;
    }
  }
}

void AudioStreamCoreAudio::applyStart() {
  if (mState.load() != StreamState::Starting || mAudioUnit == nullptr) {
    return;
  }

  if (isCallbackMode()) {
    setDataCallbackEnabled(true);
  }
  // The HAL can call back as soon as the unit starts, before `Started` is published, so the running
  // flag is what the callbacks test; the state follows as soon as the start call returned.
  mDeviceRunning.store(true, std::memory_order_release);
  const OSStatus status = AudioOutputUnitStart(mAudioUnit);
  if (status != noErr) {
    mDeviceRunning.store(false, std::memory_order_release);
    if (isCallbackMode()) {
      setDataCallbackEnabled(false);
    }
    reportDeviceError(resultFromOSStatus(status, "AudioOutputUnitStart"), "AudioOutputUnitStart");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mLock);
    if (mExitRequested || mState.load() != StreamState::Starting) {
      // A close or a stop published while the unit was starting wins; the caller of this function
      // (or teardown) is responsible for stopping the unit again.
      return;
    }
    mState.store(StreamState::Started);
  }
  mStateChanged.notify_all();
  notifyBufferWaiters();
}

void AudioStreamCoreAudio::applyPause() {
  if (mState.load() != StreamState::Pausing) {
    return;
  }
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  mDeviceRunning.store(false, std::memory_order_release);
  if (mAudioUnit != nullptr) {
    (void) AudioOutputUnitStop(mAudioUnit);
  }
  setState(StreamState::Paused);
}

void AudioStreamCoreAudio::applyFlush() {
  if (mState.load() != StreamState::Flushing) {
    return;
  }
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  mDeviceRunning.store(false, std::memory_order_release);
  if (mAudioUnit != nullptr) {
    (void) AudioOutputUnitStop(mAudioUnit);
    (void) AudioUnitReset(mAudioUnit, kAudioUnitScope_Global, 0);
  }
  resetRingBuffer();
  setState(StreamState::Flushed);
}

void AudioStreamCoreAudio::applyStop() {
  if (mState.load() != StreamState::Stopping) {
    return;
  }
  if (isCallbackMode()) {
    setDataCallbackEnabled(false);
  }
  mDeviceRunning.store(false, std::memory_order_release);
  if (mAudioUnit != nullptr) {
    (void) AudioOutputUnitStop(mAudioUnit);
    (void) AudioUnitReset(mAudioUnit, kAudioUnitScope_Global, 0);
  }
  resetRingBuffer();
  setState(StreamState::Stopped);
}

// =============================================================================
//  HAL callbacks
// =============================================================================

OSStatus AudioStreamCoreAudio::renderCallbackEntry(void* refCon,
  AudioUnitRenderActionFlags* flags,
  const AudioTimeStamp* timestamp,
  UInt32 /* busNumber */,
  UInt32 numFrames,
  ::AudioBufferList* ioData) {
  auto* stream = static_cast<AudioStreamCoreAudio*>(refCon);
  if (stream == nullptr) {
    return noErr;
  }
  return stream->onRender(flags, timestamp, numFrames, ioData);
}

OSStatus AudioStreamCoreAudio::inputCallbackEntry(void* refCon,
  AudioUnitRenderActionFlags* flags,
  const AudioTimeStamp* timestamp,
  UInt32 /* busNumber */,
  UInt32 numFrames,
  ::AudioBufferList* /* ioData */) {
  auto* stream = static_cast<AudioStreamCoreAudio*>(refCon);
  if (stream == nullptr) {
    return noErr;
  }
  return stream->onInput(flags, timestamp, numFrames);
}

OSStatus AudioStreamCoreAudio::onRender(AudioUnitRenderActionFlags* /* flags */,
  const AudioTimeStamp* timestamp,
  const uint32_t numFrames,
  ::AudioBufferList* ioData) {
  if (ioData == nullptr || ioData->mNumberBuffers == 0 || ioData->mBuffers[0].mData == nullptr ||
      numFrames == 0) {
    return noErr;
  }

  auto* data = static_cast<uint8_t*>(ioData->mBuffers[0].mData);
  const size_t bytesPerFrame = getBytesPerFrame();
  const size_t byteCount = static_cast<size_t>(numFrames) * bytesPerFrame;

  if (timestamp != nullptr && (timestamp->mFlags & kAudioTimeStampHostTimeValid) != 0) {
    mTimestampHostTime.store(timestamp->mHostTime, std::memory_order_relaxed);
    mTimestampFramePosition.store(mFramesRead.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }

  if (!mDeviceRunning.load(std::memory_order_acquire)) {
    std::memset(data, 0, byteCount);
    return noErr;
  }

  bool stopRequested = false;
  if (isCallbackMode()) {
    if (isDataCallbackEnabled()) {
      const uint32_t chunkFrames = callbackChunkFrames(numFrames);
      for (uint32_t offset = 0; offset < numFrames && !stopRequested; offset += chunkFrames) {
        const uint32_t chunk = std::min(chunkFrames, numFrames - offset);
        if (fireDataCallback(data + static_cast<size_t>(offset) * bytesPerFrame, chunk) ==
            DataCallbackResult::Stop) {
          // The remainder of the device buffer is silence rather than the previous buffer's audio.
          std::memset(data + static_cast<size_t>(offset + chunk) * bytesPerFrame, 0,
            static_cast<size_t>(numFrames - offset - chunk) * bytesPerFrame);
          stopRequested = true;
        }
      }
    } else {
      std::memset(data, 0, byteCount);
    }
    mFramesWritten.fetch_add(numFrames);
    mFramesRead.fetch_add(numFrames);
  } else {
    size_t filled = 0;
    {
      std::lock_guard<std::mutex> lock(mBufferLock);
      filled = popFromRingBuffer(data, numFrames);
    }
    if (filled < numFrames) {
      // Nothing left to play: the tail is silence, and a buffer the device had to be given without
      // data is an underrun. The count is a diagnostic, not a fatal error.
      std::memset(data + filled * bytesPerFrame, 0, (numFrames - filled) * bytesPerFrame);
      mXRunCount.fetch_add(1);
    }
    mBufferSpaceAvailable.notify_all();
    mFramesRead.fetch_add(filled);
  }

  if (stopRequested) {
    // The control thread performs the actual stop; AudioOutputUnitStop must not run on the HAL
    // thread that is currently inside this callback.
    requestStop();
  }
  return noErr;
}

OSStatus AudioStreamCoreAudio::onInput(AudioUnitRenderActionFlags* flags,
  const AudioTimeStamp* timestamp,
  const uint32_t numFrames) {
  if (mAudioUnit == nullptr || numFrames == 0) {
    return noErr;
  }

  const size_t bytesPerFrame = getBytesPerFrame();
  if (mCaptureBuffer.size() < static_cast<size_t>(numFrames) * bytesPerFrame) {
    // The device grew its buffer after the scratch was sized. Growing it here would allocate on the
    // real-time thread, so the frames are dropped instead and the next open sizes the buffer again.
    MINILOG_ERROR("The Core Audio input buffer holds {} frames, more than the {} the stream sized",
      numFrames, mCaptureBuffer.size() / std::max<size_t>(bytesPerFrame, 1));
    return noErr;
  }

  mCaptureBufferList.mNumberBuffers = 1;
  mCaptureBufferList.mBuffers[0].mNumberChannels = mChannelCount;
  mCaptureBufferList.mBuffers[0].mDataByteSize = static_cast<UInt32>(static_cast<size_t>(numFrames) * bytesPerFrame);
  mCaptureBufferList.mBuffers[0].mData = mCaptureBuffer.data();

  // The input callback is not given the frames; it has to pull them out of the unit.
  const OSStatus status = AudioUnitRender(mAudioUnit, flags, timestamp, 1, numFrames, &mCaptureBufferList);
  if (status != noErr) {
    reportDeviceError(resultFromOSStatus(status, "AudioUnitRender"), "AudioUnitRender");
    return status;
  }

  if (timestamp != nullptr && (timestamp->mFlags & kAudioTimeStampHostTimeValid) != 0) {
    mTimestampHostTime.store(timestamp->mHostTime, std::memory_order_relaxed);
    mTimestampFramePosition.store(mFramesWritten.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }

  if (!mDeviceRunning.load(std::memory_order_acquire)) {
    return noErr;
  }

  uint8_t* data = mCaptureBuffer.data();
  bool stopRequested = false;
  if (isCallbackMode()) {
    if (isDataCallbackEnabled()) {
      const uint32_t chunkFrames = callbackChunkFrames(numFrames);
      for (uint32_t offset = 0; offset < numFrames && !stopRequested; offset += chunkFrames) {
        const uint32_t chunk = std::min(chunkFrames, numFrames - offset);
        if (fireDataCallback(data + static_cast<size_t>(offset) * bytesPerFrame, chunk) ==
            DataCallbackResult::Stop) {
          stopRequested = true;
        }
      }
      // The callback consumes the frames as they arrive, so the frames received and the frames
      // consumed are the same, and both counters report them.
      mFramesWritten.fetch_add(numFrames);
      mFramesRead.fetch_add(numFrames);
    }
  } else {
    size_t stored = 0;
    {
      std::lock_guard<std::mutex> lock(mBufferLock);
      stored = pushToRingBuffer(data, numFrames);
    }
    if (stored < numFrames) {
      // The application did not read in time; what does not fit is discarded.
      mXRunCount.fetch_add(1);
    }
    mFramesWritten.fetch_add(numFrames);
    mBufferDataAvailable.notify_all();
  }

  if (stopRequested) {
    requestStop();
  }
  return noErr;
}

// =============================================================================
//  Device notifications
// =============================================================================

OSStatus AudioStreamCoreAudio::deviceListenerEntry(const AudioObjectID /* objectId */,
  const UInt32 numberAddresses,
  const AudioObjectPropertyAddress* addresses,
  void* clientData) {
  auto* stream = static_cast<AudioStreamCoreAudio*>(clientData);
  if (stream == nullptr || addresses == nullptr) {
    return noErr;
  }
  for (UInt32 index = 0; index < numberAddresses; ++index) {
    stream->onDevicePropertyChanged(addresses[index]);
  }
  return noErr;
}

void AudioStreamCoreAudio::onDevicePropertyChanged(const AudioObjectPropertyAddress& address) {
  if (address.mSelector != kAudioDevicePropertyDeviceIsAlive) {
    return;
  }

  const StreamState state = mState.load();
  if (state == StreamState::Closed || state == StreamState::Uninitialized ||
      state == StreamState::Closing) {
    // The listener can be removed a moment after the state moved away from a live stream; a report
    // at that point would resurrect an error callback on an object that is already going away.
    return;
  }

  AudioObjectPropertyAddress aliveAddress{
    kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  UInt32 alive = 1;
  UInt32 size = sizeof(alive);
  const OSStatus status =
    AudioObjectGetPropertyData(mDevice, &aliveAddress, 0, nullptr, &size, &alive);
  if (status == noErr && alive == 0) {
    reportDeviceError(Result::ErrorDisconnected, "the audio device is no longer alive");
  }
}

// =============================================================================
//  Errors
// =============================================================================

void AudioStreamCoreAudio::reportDeviceError(const Result error, const char* operation) {
  if (mState.load() == StreamState::Closed) {
    return;
  }
  if (mFatalErrorReported.exchange(true)) {
    return;
  }

  mErrorCallbackResult = error;
  MINILOG_ERROR("Core Audio stream failed in {}: {}", operation, convertToText(error));
  setState(StreamState::Disconnected);

  const auto callback = getErrorCallback();
  if (callback == nullptr) {
    MINILOG_DEBUG("No Core Audio error callback was set, so the failure was only logged");
    return;
  }

  // The documented reaction to an error is to stop and close the stream, and close() joins the
  // control thread. Running that on a detached thread keeps the HAL thread free to return from its
  // callback and the control thread free to process the stop.
  auto shared = lockWeakThis();
  if (!shared) {
    return;
  }
  try {
    std::thread([shared, error] {
      auto* stream = shared.get();
      const auto errorCallback = stream->getErrorCallback();
      if (errorCallback == nullptr) {
        return;
      }
      if (errorCallback->onError(stream, error)) {
        return;
      }
      errorCallback->onErrorBeforeClose(stream, error);
      stream->close();
      errorCallback->onErrorAfterClose(stream, error);
    }).detach();
  } catch (const std::system_error& exception) {
    MINILOG_ERROR("Could not start the Core Audio error thread: {}", exception.what());
  }
}

// =============================================================================
//  Data path
// =============================================================================

uint32_t AudioStreamCoreAudio::callbackChunkFrames(const uint32_t frames) const {
  const uint32_t chunk = mFramesPerCallback != kUnspecified ? mFramesPerCallback : mDeviceBufferFrames;
  if (chunk == 0 || chunk >= frames) {
    return frames;
  }
  return chunk;
}

size_t AudioStreamCoreAudio::pushToRingBuffer(const uint8_t* source, const size_t frames) {
  if (source == nullptr || frames == 0) {
    return 0;
  }
  const size_t fit = std::min(frames, mRingBuffer.space());
  if (fit == 0) {
    return 0;
  }

  bool stored = false;
  switch (getFormat()) {
    case AudioSampleFormat::kFloat:
      stored = mRingBuffer.write(reinterpret_cast<const float*>(source), fit);
      break;
    case AudioSampleFormat::kInt16:
      stored = mRingBuffer.writeInt16(reinterpret_cast<const int16_t*>(source), fit);
      break;
    case AudioSampleFormat::kInt24:
      stored = mRingBuffer.writeInt24(source, fit);
      break;
    case AudioSampleFormat::kInt32:
      stored = mRingBuffer.writeInt32(reinterpret_cast<const int32_t*>(source), fit);
      break;
    default:
      break;
  }
  return stored ? fit : 0;
}

size_t AudioStreamCoreAudio::popFromRingBuffer(uint8_t* destination, const size_t frames) {
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

void AudioStreamCoreAudio::resetRingBuffer() {
  {
    std::lock_guard<std::mutex> lock(mBufferLock);
    mRingBuffer.reset();
  }
  notifyBufferWaiters();
}

OpResult<uint32_t> AudioStreamCoreAudio::write(const void* buffer,
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
  const int64_t deadline = AudioUtils::getNanoseconds() + std::max<int64_t>(timeoutNanoseconds, 0);

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
    mBufferSpaceAvailable.wait_for(lock, std::chrono::nanoseconds(remaining), [this] {
      return mRingBuffer.space() > 0 || mState.load() != StreamState::Started;
    });
  }
  lock.unlock();

  if (written > 0) {
    mFramesWritten.fetch_add(written);
    return OpResult(written);
  }
  return Result::ErrorTimeout;
}

OpResult<uint32_t> AudioStreamCoreAudio::read(void* buffer,
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
  const int64_t deadline = AudioUtils::getNanoseconds() + std::max<int64_t>(timeoutNanoseconds, 0);

  uint32_t framesRead = 0;
  std::unique_lock<std::mutex> lock(mBufferLock);
  while (framesRead < numFrames) {
    const size_t received =
      popFromRingBuffer(destination + static_cast<size_t>(framesRead) * bytesPerFrame, numFrames - framesRead);
    if (received > 0) {
      framesRead += static_cast<uint32_t>(received);
      continue;
    }

    if (timeoutNanoseconds <= 0 || mState.load() != StreamState::Started) {
      break;
    }
    const int64_t remaining = deadline - AudioUtils::getNanoseconds();
    if (remaining <= 0) {
      break;
    }
    mBufferDataAvailable.wait_for(lock, std::chrono::nanoseconds(remaining), [this] {
      return mRingBuffer.available() > 0 || mState.load() != StreamState::Started;
    });
  }
  lock.unlock();

  if (framesRead > 0) {
    mFramesRead.fetch_add(framesRead);
    return OpResult(framesRead);
  }
  return Result::ErrorTimeout;
}

// =============================================================================
//  Queries
// =============================================================================

OpResult<uint32_t> AudioStreamCoreAudio::setBufferSizeInFrames(const uint32_t requestedFrames) {
  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  if (requestedFrames > mBufferCapacityInFrames) {
    MINILOG_DEBUG("setBufferSizeInFrames({}) exceeds the capacity of {}", requestedFrames,
      mBufferCapacityInFrames);
  }
  // The HAL fixes the device buffer size when the unit is initialized, so the only honest answer is
  // the size in use. It is not a failure: the caller learns the real value either way.
  return OpResult(mBufferSizeInFrames);
}

OpResult<uint32_t> AudioStreamCoreAudio::getXRunCount() {
  if (mState.load() == StreamState::Closed) {
    return Result::ErrorClosed;
  }
  return OpResult(mXRunCount.load());
}

bool AudioStreamCoreAudio::isXRunCountSupported() const {
  return true;
}

Result AudioStreamCoreAudio::getTimestamp(const ClockMode mode,
  int64_t* framePosition,
  int64_t* timeNanoseconds) {
  if (mAudioUnit == nullptr) {
    return Result::ErrorClosed;
  }

  const int64_t frame = mTimestampFramePosition.load();
  const uint64_t hostTime = mTimestampHostTime.load();
  int64_t nanos = hostTime != 0 ? hostTimeToNanos(hostTime) + mMonotonicOffsetNanos
                                : AudioUtils::getNanoseconds();
  if (mode == ClockMode::Realtime) {
    nanos += mRealtimeOffsetNanos;
  }

  if (framePosition != nullptr) {
    *framePosition = frame;
  }
  if (timeNanoseconds != nullptr) {
    *timeNanoseconds = nanos;
  }
  return Result::OK;
}

OpResult<double> AudioStreamCoreAudio::calculateLatencyMillis() {
  if (mAudioUnit == nullptr) {
    return Result::ErrorClosed;
  }

  const auto sampleRate = getSampleRate();
  if (sampleRate == 0) {
    return Result::ErrorInvalidState;
  }

  // The device buffer, plus the latency the driver and the safety offset add. Core Audio does not
  // expose one "output latency" value, so this is the sum the HAL documents for the device side.
  const int64_t frames = static_cast<int64_t>(mDeviceBufferFrames) +
                         static_cast<int64_t>(deviceLatencyFrames());
  return OpResult<double>(static_cast<double>(frames) * 1000.0 / static_cast<double>(sampleRate));
}

void AudioStreamCoreAudio::updateFramesWritten() {
  // Every frame counted here is counted by the thread that moved it, so there is nothing to poll.
}

void AudioStreamCoreAudio::updateFramesRead() {
  // Every frame counted here is counted by the thread that moved it, so there is nothing to poll.
}

// =============================================================================
//  Helpers
// =============================================================================

int64_t AudioStreamCoreAudio::hostTimeToNanos(const uint64_t hostTime) const {
  if (hostTime == 0) {
    return 0;
  }
  return static_cast<int64_t>(static_cast<long double>(hostTime) * mTimebaseNumer / mTimebaseDenom);
}

UInt32 AudioStreamCoreAudio::deviceLatencyFrames() const {
  if (mDevice == kAudioObjectUnknown) {
    return 0;
  }
  const AudioObjectPropertyScope scope =
    isRender() ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
  UInt32 latency = getDeviceUInt32(mDevice, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeGlobal);
  if (latency == 0) {
    latency = getDeviceUInt32(mDevice, kAudioDevicePropertyLatency, scope);
  }
  const UInt32 safety = getDeviceUInt32(mDevice, kAudioDevicePropertySafetyOffset, scope);
  return latency + safety;
}

Result AudioStreamCoreAudio::resultFromOSStatus(const OSStatus status, const char* operation) {
  if (status == noErr) {
    return Result::OK;
  }
  MINILOG_ERROR("Core Audio reported {} for {}", static_cast<int>(status), operation);
  switch (status) {
    case kAudioUnitErr_InvalidProperty:
    case kAudioUnitErr_InvalidParameter:
    case kAudioUnitErr_InvalidParameterValue:
    case kAudioUnitErr_InvalidElement:
    case kAudioUnitErr_InvalidScope:
    case kAudioUnitErr_InvalidPropertyValue:
    case kAudioUnitErr_PropertyNotWritable:
    case kAudioUnitErr_PropertyNotInUse:
      return Result::ErrorIllegalArgument;
    case kAudioUnitErr_FormatNotSupported:
      return Result::ErrorInvalidFormat;
    case kAudioUnitErr_Uninitialized:
    case kAudioUnitErr_Initialized:
    case kAudioUnitErr_CannotDoInCurrentContext:
      return Result::ErrorInvalidState;
    case kAudioUnitErr_TooManyFramesToProcess:
    case kAudioUnitErr_InvalidFile:
    case kAudioUnitErr_UnknownFileType:
    case kAudioUnitErr_FileNotSpecified:
    case kAudioUnitErr_NoConnection:
    case kAudioUnitErr_Unauthorized:
      return Result::ErrorUnavailable;
    default:
      return Result::ErrorInternal;
  }
}

AudioStreamBasicDescription AudioStreamCoreAudio::streamDescriptionFor(const uint64_t sampleRate,
  const uint32_t channelCount,
  const AudioSampleFormat format) {
  AudioStreamBasicDescription description{};
  description.mSampleRate = static_cast<Float64>(sampleRate);
  description.mFormatID = kAudioFormatLinearPCM;
  description.mChannelsPerFrame = channelCount;
  description.mFramesPerPacket = 1;

  uint32_t bitsPerChannel = 32;
  switch (format) {
    case AudioSampleFormat::kInt16:
      bitsPerChannel = 16;
      break;
    case AudioSampleFormat::kInt24:
      bitsPerChannel = 24;
      break;
    case AudioSampleFormat::kInt32:
      bitsPerChannel = 32;
      break;
    case AudioSampleFormat::kFloat:
    case AudioSampleFormat::kDefault:
    default:
      bitsPerChannel = 32;
      break;
  }

  if (format == AudioSampleFormat::kFloat || format == AudioSampleFormat::kDefault) {
    description.mFormatFlags = static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked);
  } else {
    description.mFormatFlags = static_cast<UInt32>(kAudioFormatFlagsNativeEndian) |
                               static_cast<UInt32>(kAudioFormatFlagIsSignedInteger) |
                               static_cast<UInt32>(kAudioFormatFlagIsPacked);
  }

  description.mBitsPerChannel = bitsPerChannel;
  description.mBytesPerFrame = (bitsPerChannel / 8u) * channelCount;
  description.mBytesPerPacket = description.mBytesPerFrame;
  return description;
}

AudioDeviceID AudioStreamCoreAudio::selectDeviceFor(const Direction direction) {
  // The explicit selection comes first: a device with the right direction is usable even when the
  // system has no default for it, which is exactly the case CORAL_COREAUDIO_DEVICE exists for.
  const char* const requested = std::getenv("CORAL_COREAUDIO_DEVICE");
  if (requested != nullptr && requested[0] != '\0') {
    for (const AudioDeviceID candidate : allDevices()) {
      if (channelCountForDevice(candidate, direction) == 0) {
        continue;
      }
      if (deviceHasNameOrUID(candidate, requested)) {
        return candidate;
      }
    }
    MINILOG_WARN("CORAL_COREAUDIO_DEVICE names no {} device, using the default one: {}",
      direction == Direction::Render ? "output" : "input", requested);
  }

  AudioObjectPropertyAddress address{
    direction == Direction::Render ? kAudioHardwarePropertyDefaultOutputDevice
                                   : kAudioHardwarePropertyDefaultInputDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };
  AudioDeviceID device = kAudioObjectUnknown;
  UInt32 size = sizeof(device);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) != noErr) {
    return kAudioObjectUnknown;
  }
  return device;
}

uint32_t AudioStreamCoreAudio::channelCountForDevice(const AudioDeviceID device, const Direction direction) {
  AudioObjectPropertyAddress address{
    kAudioDevicePropertyStreamConfiguration,
    direction == Direction::Render ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput,
    kAudioObjectPropertyElementMain
  };
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0) {
    return 0;
  }

  // The ::AudioBufferList is variable length, so the storage has to be aligned for it.
  std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
  auto* list = reinterpret_cast<::AudioBufferList*>(storage.data());
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr) {
    return 0;
  }

  uint32_t channels = 0;
  for (UInt32 index = 0; index < list->mNumberBuffers; ++index) {
    channels += list->mBuffers[index].mNumberChannels;
  }
  return channels;
}

Float64 AudioStreamCoreAudio::sampleRateForDevice(const AudioDeviceID device) {
  AudioObjectPropertyAddress address{
    kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
  };
  Float64 rate = 0;
  UInt32 size = sizeof(rate);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate) != noErr) {
    return 0;
  }
  return rate;
}

uint32_t AudioStreamCoreAudio::bufferFramesForDevice(const AudioDeviceID device) {
  const UInt32 frames =
    getDeviceUInt32(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
  return frames > 0 ? frames : kDefaultBufferFrames;
}

} // namespace coral
