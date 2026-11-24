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

#ifndef CORAL_AUDIO_COMMON_QUIRKS_MANAGER_H
#define CORAL_AUDIO_COMMON_QUIRKS_MANAGER_H

#include "private/audio/AudioStream.h"
#include "private/audio/AudioStreamBuilder.h"

#ifdef __ANDROID__
#include "aaudio/AudioStreamAAudio.h"
#endif

#include <memory>

namespace coral {

/**
 * INTERNAL USE ONLY.
 *
 * Based on manufacturer, model and System version number
 * decide whether data conversion needs to occur.
 *
 * This also manages device and version specific workarounds.
 */
class QuirksManager {
public:
  static QuirksManager &getInstance() {
    static QuirksManager instance; // singleton
    return instance;
  }

  QuirksManager();

  virtual ~QuirksManager() = default;

  /**
   * Do we need to do channel, format or rate conversion to provide a low
   * latency stream for this builder? If so then provide a builder for the
   * native child stream that will be used to get low latency.
   *
   * @param builder builder provided by application
   * @param childBuilder modified builder appropriate for the underlying device
   * @return true if conversion is needed
   */
  bool isConversionNeeded(const AudioStreamBuilder &builder, AudioStreamBuilder &childBuilder);

  static bool isMMapUsed(AudioStream *stream) {
    bool answer = false;
#ifdef __ANDROID__
    auto streamAAudio = dynamic_cast<AudioStreamAAudio *>(stream);
    if (streamAAudio) {
      answer = streamAAudio->isMMapUsed();
    }
#endif
    return answer;
  }

  virtual int32_t clipBufferSize(AudioStream *stream, int32_t bufferSize) {
    return mDeviceQuirks->clipBufferSize(stream, bufferSize);
  }

  class DeviceQuirks {
  public:
    virtual ~DeviceQuirks() = default;

    /**
     * Restrict buffer size. This is mainly to avoid glitches caused by MMAP
     * timestamp inaccuracies.
     * @param stream
     * @param requestedSize
     * @return
     */
    int32_t clipBufferSize(AudioStream *stream, int32_t requestedSize);

    // Exclusive MMAP streams can have glitches because they are using a timing
    // model of the DSP to control IO instead of direct synchronization.
    [[nodiscard]] virtual int32_t getExclusiveBottomMarginInBursts() const {
      return kDefaultBottomMarginInBursts;
    }

    [[nodiscard]] virtual int32_t getExclusiveTopMarginInBursts() const {
      return kDefaultTopMarginInBursts;
    }

    // On some devices, you can open a mono stream but it is actually running in
    // stereo!
    [[nodiscard]] virtual bool isMonoMMapActuallyStereo() const {
      return false;
    }

    [[nodiscard]] virtual bool
    isAAudioMMapPossible(const AudioStreamBuilder &builder) const;

    virtual bool isMMapSafe(const AudioStreamBuilder & /* builder */) {
      return true;
    }

    // On some devices, Float does not work so it should be converted to I16.
    static bool shouldConvertFloatToI16ForOutputStreams();

    static constexpr int32_t kDefaultBottomMarginInBursts = 0;
    static constexpr int32_t kDefaultTopMarginInBursts = 0;

    // For Legacy streams, do not let the buffer go below one burst.
    // b/129545119 | AAudio Legacy allows setBufferSizeInFrames too low
    // Fixed in Q
    static constexpr int32_t kLegacyBottomMarginInBursts = 1;
    // very typical native sample rate
    static constexpr int32_t kCommonNativeRate = 48000;
  };

  bool isMMapSafe(AudioStreamBuilder &builder);

private:
  static constexpr int32_t kChannelCountMono = 1;
  static constexpr int32_t kChannelCountStereo = 2;

  std::unique_ptr<DeviceQuirks> mDeviceQuirks{};
};

} // namespace coral

#endif // CORAL_AUDIO_COMMON_QUIRKS_MANAGER_H
