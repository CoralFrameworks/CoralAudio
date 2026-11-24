/*
 * Copyright (C) 2024 The Coral Project
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

#include "QuirksManager.h"
#include "Utilities.h"

#include "private/audio/AudioStream.h"

#include <minilog/log.h>

#ifndef __ANDROID_API_R__
#define __ANDROID_API_R__ 30
#endif

namespace coral {

int32_t QuirksManager::DeviceQuirks::clipBufferSize(AudioStream *stream, int32_t requestedSize) {
  if (!stream) {
    return requestedSize;
  }
  int bottomMargin = kDefaultBottomMarginInBursts;
  int topMargin = kDefaultTopMarginInBursts;
  if (isMMapUsed(stream)) {
    if (stream->getSharingMode() == SharingMode::Exclusive) {
      bottomMargin = getExclusiveBottomMarginInBursts();
      topMargin = getExclusiveTopMarginInBursts();
    }
  } else {
    bottomMargin = kLegacyBottomMarginInBursts;
  }

  int32_t burst = stream->getFramesPerBurst();
  int32_t minSize = bottomMargin * burst;
  int32_t adjustedSize = requestedSize;
  if (adjustedSize < minSize) {
    adjustedSize = minSize;
  } else {
    int32_t maxSize = stream->getBufferCapacityInFrames() - (topMargin * burst);
    if (adjustedSize > maxSize) {
      adjustedSize = maxSize;
    }
  }
  return adjustedSize;
}

bool QuirksManager::DeviceQuirks::isAAudioMMapPossible(const AudioStreamBuilder &builder) const {
  bool isSampleRateCompatible =
      builder.getSampleRate() == Definitions::kUnspecified ||
          builder.getSampleRate() == kCommonNativeRate;
  return builder.getPerformanceMode() == PerformanceMode::LowLatency &&
      isSampleRateCompatible &&
      builder.getChannelCount() <= kChannelCountStereo;
}

bool QuirksManager::DeviceQuirks::shouldConvertFloatToI16ForOutputStreams() {
#if defined(ANDROID) || defined(__ANDROID__)
  const auto productManufacturer = getPropertyString("ro.product.manufacturer");
  if (getSdkVersion() < __ANDROID_API_L__) {
    return true;
  } else if ((productManufacturer == "vivo") && (getSdkVersion() < __ANDROID_API_M__)) {
    return true;
  }
#endif
  return false;
}

/**
 * This is for Samsung Exynos quirks. Samsung Mobile uses Qualcomm chips so
 * the QualcommDeviceQuirks would apply.
 */
class SamsungExynosDeviceQuirks : public QuirksManager::DeviceQuirks {
public:
  SamsungExynosDeviceQuirks() {
    std::string chipname = getPropertyString("ro.hardware.chipname");
    isExynos9810 = (chipname == "exynos9810");
    isExynos990 = (chipname == "exynos990");
    isExynos850 = (chipname == "exynos850");

    mBuildChangelist = getPropertyInteger("ro.build.changelist", 0);
  }

  virtual ~SamsungExynosDeviceQuirks() = default;

  int32_t getExclusiveBottomMarginInBursts() const override {
    return kBottomMargin;
  }

  int32_t getExclusiveTopMarginInBursts() const override {
    return kTopMargin;
  }

  bool isMonoMMapActuallyStereo() const override {
    return isExynos9810 || isExynos850; // TODO We can make this version
    // specific if it gets fixed.
  }

  bool isAAudioMMapPossible(const AudioStreamBuilder &builder) const override {
    return DeviceQuirks::isAAudioMMapPossible(builder) &&
        // Samsung says they use Legacy for Camcorder
        builder.getInputPreset() != InputPreset::Camcorder;
  }

  bool isMMapSafe(const AudioStreamBuilder &builder) override {
    const bool isInput = builder.getDirection() == Direction::Capture;
    // This detects b/159066712 , S20 LSI has corrupt low latency audio
    // recording and turns off MMAP.
    bool isRecordingCorrupted =
        isInput && isExynos990 && mBuildChangelist < 19350896;

    // Certain S9+ builds record silence when using MMAP and not using the
    // VoiceCommunication preset.
    bool wouldRecordSilence = isInput &&
        isExynos9810 &&
        mBuildChangelist <= 18847185 &&
        (builder.getInputPreset() != InputPreset::VoiceCommunication);

    if (wouldRecordSilence) {
      MINILOG_INFO("Requested stream configuration would result in silence on this device. Switching off MMAP.");
    }

    return !isRecordingCorrupted && !wouldRecordSilence;
  }

private:
  // Stay farther away from DSP position on Exynos devices.
  static constexpr int32_t kBottomMargin = 2;
  static constexpr int32_t kTopMargin = 1;
  bool isExynos9810 = false;
  bool isExynos990 = false;
  bool isExynos850 = false;
  int mBuildChangelist = 0;
};

class QualcommDeviceQuirks : public QuirksManager::DeviceQuirks {
public:
  QualcommDeviceQuirks() {
    std::string modelName = getPropertyString("ro.soc.model");
    isSM8150 = (modelName == "SDM8150");
  }

  ~QualcommDeviceQuirks() override = default;

  [[nodiscard]] int32_t getExclusiveBottomMarginInBursts() const override {
    return kBottomMargin;
  }

  bool isMMapSafe(const AudioStreamBuilder &builder) override {
    bool isMMapBroken = false;
    if (isSM8150 && (getSdkVersion() <= __ANDROID_API_P__)) {
      isMMapBroken = true;
      MINILOG_INFO("MMAP not actually supported on this chip. Switching off MMAP.");
    }
    return !isMMapBroken;
  }

private:
  bool isSM8150 = false;
  static constexpr int32_t kBottomMargin = 1;
};

QuirksManager::QuirksManager() {
  std::string productManufacturer = getPropertyString("ro.product.manufacturer");
  if (productManufacturer == "samsung") {
    std::string arch = getPropertyString("ro.arch");
    bool isExynos = (arch.rfind("exynos", 0) == 0); // starts with?
    if (isExynos) {
      mDeviceQuirks = std::make_unique<SamsungExynosDeviceQuirks>();
    }
  }
  if (!mDeviceQuirks) {
    std::string socManufacturer = getPropertyString("ro.soc.manufacturer");
    if (socManufacturer == "Qualcomm") {
      // This may include Samsung Mobile devices.
      mDeviceQuirks = std::make_unique<QualcommDeviceQuirks>();
    } else {
      mDeviceQuirks = std::make_unique<DeviceQuirks>();
    }
  }
}

bool QuirksManager::isConversionNeeded(const AudioStreamBuilder &builder, AudioStreamBuilder &childBuilder) {
  bool conversionNeeded = false;
  const bool isLowLatency = builder.getPerformanceMode() == PerformanceMode::LowLatency;
  const bool isInput = builder.getDirection() == Direction::Capture;
  const bool isFloat = builder.getFormat() == AudioSampleFormat::kFloat;

  // There are multiple bugs involving using callback with a specified callback
  // size. Issue #778: O to Q had a problem with Legacy INPUT streams for FLOAT
  // streams and a specified callback size. It would assert because of a bad
  // buffer size.
  //
  // Issue #973: O to R had a problem with Legacy output streams using callback
  // and a specified callback size. An AudioTrack stream could still be running
  // when the AAudio FixedBlockReader was closed. Internally
  // b/161914201#comment25
  //
  // Issue #983: O to R would glitch if the framesPerCallback was too small.
  //
  // Most of these problems were related to Legacy stream. MMAP was OK. But we
  // don't know if we will get an MMAP stream. So, to be safe, just do the
  // conversion in audio.
  if (isAAudioRecommended() &&
      builder.isDataCallbackSpecified() &&
      builder.getFramesPerDataCallback() != 0 &&
      getSdkVersion() <= __ANDROID_API_R__) {
    childBuilder.setFramesPerDataCallback(Definitions::kUnspecified);
    conversionNeeded = true;
  }

  // If a SAMPLE RATE is specified for low latency, let the native code choose
  // an optimal rate. This isn't really a workaround. It is a feature that is
  // convenient to place here.
  // TODO There may be a problem if the devices supports low latency at a higher
  //  rate than the default.
  if (isLowLatency &&
      builder.getSampleRate() != Definitions::kUnspecified) {
    // native API decides the best sample rate
    childBuilder.setSampleRate(Definitions::kUnspecified);
    conversionNeeded = true;
  }

  // Data Format
  // OpenSL ES and AAudio before P do not support FAST path for FLOAT capture.
  if (isFloat &&
      isInput &&
      isLowLatency &&
      builder.isFormatConversionAllowed() &&
      getSdkVersion() < __ANDROID_API_P__) {
    // needed for FAST track
    childBuilder.setFormat(AudioSampleFormat::kInt16);
    conversionNeeded = true;
    MINILOG_INFO("Forcing internal format to I16 for low latency");
  }

  // Add quirk for float output when needed.
  if (isFloat &&
      !isInput &&
      builder.isFormatConversionAllowed() &&
      mDeviceQuirks->shouldConvertFloatToI16ForOutputStreams()) {
    childBuilder.setFormat(AudioSampleFormat::kInt16);
    conversionNeeded = true;
    MINILOG_INFO("float was requested but not supported on pre-L devices and some devices like Vivo devices "
                 "may have issues on L devices, creating an underlying I16 stream and using format conversion "
                 "to provide a float stream.");
  }

  // Channel Count conversions
  if (isInput &&
      isLowLatency &&
      builder.isChannelConversionAllowed() &&
      builder.getChannelCount() == kChannelCountStereo &&
      (!isAAudioRecommended() && (getSdkVersion() == __ANDROID_API_O__))) {
    // Workaround for heap size regression in O.
    // b/66967812 AudioRecord does not allow FAST track for stereo capture in O
    childBuilder.setChannelCount(kChannelCountMono);
    conversionNeeded = true;
    MINILOG_INFO("Using mono internally for low latency on O");
  } else if (isInput &&
      isAAudioRecommended() &&
      builder.getChannelCount() == kChannelCountMono &&
      mDeviceQuirks->isMonoMMapActuallyStereo() &&
      // Note: we might use this workaround on a device that supports
      // MMAP but will use Legacy for this stream.  But this will only
      // happen on devices that have the broken mono.
      mDeviceQuirks->isAAudioMMapPossible(builder)) {
    // Workaround for mono actually running in stereo mode.
    childBuilder.setChannelCount(kChannelCountStereo); // Use stereo and extract first channel.
    conversionNeeded = true;
    MINILOG_INFO("Using stereo internally to avoid broken mon");
  }
  // Note that MMAP does not support mono in 8.1. But that would only matter on
  // Pixel 1 phones, and they have almost all been updated to 9.0.

  return conversionNeeded;
}

bool QuirksManager::isMMapSafe(AudioStreamBuilder &builder) {
  return mDeviceQuirks->isMMapSafe(builder);
}

} // namespace coral
