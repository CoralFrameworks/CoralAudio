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

#ifndef CORAL_AUDIO_CHANNEL_LAYOUT_H
#define CORAL_AUDIO_CHANNEL_LAYOUT_H

#include <coral/audio/AudioTypes.h>

#include <vector>

namespace coral {

/**
 * These constants are for use in the mChannelLabel field of a ChannelDescription structure.
 */
enum class ChannelLabel : uint32_t {
  // clang-format off
  kUnknown                  = 0xFFFFFFFF,   // unknown or unspecified other use
  kUnused                   = 0,            // channel is present, but has no intended use or destination
  kUseCoordinates           = 100,          // channel is described by the mCoordinates fields.

  kLeft                     = 1,
  kRight                    = 2,
  kCenter                   = 3,
  kLFEScreen                = 4,
  kLeftSurround             = 5,            // WAVE: "Back Left"
  kRightSurround            = 6,            // WAVE: "Back Right"
  kLeftCenter               = 7,
  kRightCenter              = 8,
  kCenterSurround           = 9,            // WAVE: "Back Center" or plain "Rear Surround"
  kLeftSurroundDirect       = 10,           // WAVE: "Side Left"
  kRightSurroundDirect      = 11,           // WAVE: "Side Right"
  kTopCenterSurround        = 12,
  kVerticalHeightLeft       = 13,           // WAVE: "Top Front Left"
  kVerticalHeightCenter     = 14,           // WAVE: "Top Front Center"
  kVerticalHeightRight      = 15,           // WAVE: "Top Front Right"
  // clang-format on
};

/**
 * These constants are for use in the mChannelBitmap field of a ChannelLayout structure.
 */
enum class ChannelBitmap : uint32_t {
  // clang-format off
  kLeft                     = (1U << 0),
  kRight                    = (1U << 1),
  kCenter                   = (1U << 2),
  kLFEScreen                = (1U << 3),
  kLeftSurround             = (1U << 4), // WAVE: "Back Left"
  kRightSurround            = (1U << 5), // WAVE: "Back Right"
  kLeftCenter               = (1U << 6),
  kRightCenter              = (1U << 7),
  kCenterSurround           = (1U << 8), // WAVE: "Back Center"
  kLeftSurroundDirect       = (1U << 9), // WAVE: "Side Left"
  kRightSurroundDirect      = (1U << 10), // WAVE: "Side Right"
  kTopCenterSurround        = (1U << 11),
  kVerticalHeightLeft       = (1U << 12), // WAVE: "Top Front Left"
  kVerticalHeightCenter     = (1U << 13), // WAVE: "Top Front Center"
  kVerticalHeightRight      = (1U << 14), // WAVE: "Top Front Right"
  kTopBackLeft              = (1U << 15),
  kTopBackCenter            = (1U << 16),
  kTopBackRight             = (1U << 17)
  // clang-format on
};

/**
 * These constants are used in the mChannelFlags field of a ChannelDescription structure.
 */
enum class ChannelFlags : uint32_t {
  kAllOff = 0,
  kRectangularCoordinates = (1U << 0),
  kSphericalCoordinates = (1U << 1),
  kMeters = (1U << 2)
};

/**
 * A tag identifying a particular pre-defined channel layout.
 */
enum class ChannelLayoutTag : uint32_t {
  // clang-format off
  kUseChannelDescriptions   = (0U<<16) | 0,     // use the array of AudioChannelDescriptions to define the mapping.
  kUseChannelBitmap         = (1U<<16) | 0,     // use the bitmap to define the mapping.

  kMono                     = (100U<<16) | 1,   // a standard mono stream
  kStereo                   = (101U<<16) | 2,   // a standard stereo stream (L R) - implied playback
  // clang-format on
};

/**
 * The ChannelDescription describes a single channel.
 *
 * @field mChannelLabel
 *            The ChannelLabel that describes the channel.
 * @field mChannelFlags
 *            Flags that control the interpretation of mCoordinates.
 * @field mCoordinates
 *            An ordered triple that specifies a precise speaker location.
 */
typedef struct ChannelDescription {
  // clang-format off
  ChannelLabel mChannelLabel;
  ChannelFlags mChannelFlags;
  Float32      mCoordinates[3];
  // clang-format on
} ChannelDescription;

inline bool operator==(const ChannelDescription& lhs, const ChannelDescription& rhs) {
  return lhs.mChannelLabel == rhs.mChannelLabel &&
         lhs.mChannelFlags == rhs.mChannelFlags &&
         lhs.mCoordinates[0] == rhs.mCoordinates[0] &&
         lhs.mCoordinates[1] == rhs.mCoordinates[1] &&
         lhs.mCoordinates[2] == rhs.mCoordinates[2];
}

/**
 * The AudioChannelLayout is used to specify channel layouts in files and hardware.
 *
 * type   | channels | description
 * --------------------------------
 * Mono   | 1        | single channel
 * Stereo | 2        | left and right channels
 * 2.1    | 3        |
 * 5.1    | 6        |
 * 7.1    | 8        |
 */
class AudioChannelLayout {
public:
  /**
   * Returns the number of channels of audio data.
   */
  [[nodiscard]] AudioChannelCount getChannelCount() const {
    return mChannelCount;
  }

  /**
   * Returns the ChannelLayoutTag that indicates the layout.
   */
  [[nodiscard]] ChannelLayoutTag getChannelLayoutTag() const {
    return mChannelLayoutTag;
  }

  void setChannelLayoutTag(const ChannelLayoutTag tag) {
    mChannelLayoutTag = tag;
  }

  /**
   * If mChannelLayoutTag is set to kUseChannelBitmap, this field is the channel usage bitmap.
   */
  [[nodiscard]] ChannelBitmap getChannelBitmap() const {
    return mChannelBitmap;
  }

  void setChannelBitmap(const ChannelBitmap channelBitmap) {
    mChannelBitmap = channelBitmap;
  }

  /**
   * Returns the number of items in the mChannelDescriptions array.
   */
  [[nodiscard]] uint32_t getNumberChannelDescriptions() const {
    return mChannelDescriptions.size();
  }

  /**
   * Returns a variable length array of ChannelDescriptions that describe the layout.
   */
  [[nodiscard]] const std::vector<ChannelDescription>& getChannelDescriptions() const {
    return mChannelDescriptions;
  }

  friend bool operator==(const AudioChannelLayout& lhs, const AudioChannelLayout& rhs) {
    return lhs.mChannelCount == rhs.mChannelCount &&
           lhs.mChannelLayoutTag == rhs.mChannelLayoutTag &&
           lhs.mChannelBitmap == rhs.mChannelBitmap &&
           lhs.mChannelDescriptions == rhs.mChannelDescriptions;
  }

private:
  AudioChannelLayout()
    : mChannelCount(0),
      mChannelLayoutTag(ChannelLayoutTag::kMono),
      mChannelBitmap(ChannelBitmap::kCenter) {
  }

  AudioChannelCount mChannelCount;
  ChannelLayoutTag mChannelLayoutTag;
  ChannelBitmap mChannelBitmap;
  std::vector<ChannelDescription> mChannelDescriptions;
};

} // namespace coral

#endif //CORAL_AUDIO_CHANNEL_LAYOUT_H
