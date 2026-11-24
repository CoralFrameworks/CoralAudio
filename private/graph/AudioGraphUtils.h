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

#ifndef CORAL_AUDIO_GRAPH_UTILS_H
#define CORAL_AUDIO_GRAPH_UTILS_H

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioTypes.h>

#include "private/audio/AudioStream.h"

#include <optional>

namespace coral {

/**
 * Sample format and buffer layout helpers shared by the render graph nodes.
 *
 * @discussion
 * The graph always renders in the standard deinterleaved 32-bit float format
 * (see `AudioFormat::isStandard()`), so every conversion a node needs is
 * expressible as "float PCM buffer" on one side. The I/O nodes are the only
 * places where a hardware or manual rendering format meets the graph, and they
 * convert here instead of duplicating the sample format switch.
 *
 * All the helpers are allocation free, so they are safe to call from a render
 * or from an audio device callback.
 */
class AudioGraphUtils final {
public:
  /**
   * Copies `numFrames` frames of a float PCM buffer into another PCM buffer.
   *
   * @param src       the source buffer, in the standard float format.
   * @param srcOffset the first source frame to copy.
   * @param dst       the destination buffer, in any PCM format.
   * @param dstOffset the first destination frame to write.
   * @param numFrames the number of frames to copy.
   * @return the number of frames actually copied.
   */
  static AudioFrameCount copyPCMBuffer(const AudioPCMBuffer& src,
    AudioFrameCount srcOffset,
    AudioPCMBuffer& dst,
    AudioFrameCount dstOffset,
    AudioFrameCount numFrames);

  /**
   * Copies `numFrames` frames between two PCM buffers that share the same format.
   *
   * @discussion Unlike `copyPCMBuffer` this works for any PCM format, because it never
   * converts: it moves bytes, one channel at a time for a deinterleaved pair.
   */
  static AudioFrameCount copySameFormatPCMBuffer(const AudioPCMBuffer& src,
    AudioFrameCount srcOffset,
    AudioPCMBuffer& dst,
    AudioFrameCount dstOffset,
    AudioFrameCount numFrames);

  /**
   * Copies frames from an AudioBufferList into a PCM buffer that shares its format.
   *
   * @discussion This is how a buffer list that a client supplied (or replaced) is read back
   * into a PCM buffer. Layouts may differ: an interleaved list can fill a deinterleaved
   * buffer and the other way around. The list's own `mDataByteSize` limits how much is
   * copied, so a short list simply copies fewer frames.
   *
   * @param src       the source list, in `format`.
   * @param format    the PCM format of both the list and the destination buffer.
   * @param dst       the destination buffer, in the same format.
   * @param numFrames the number of frames to copy at most.
   * @return the number of frames actually copied.
   */
  static AudioFrameCount copyBufferListIntoPCMBuffer(const AudioBufferList& src,
    const AudioFormat& format,
    AudioPCMBuffer& dst,
    AudioFrameCount numFrames);

  /**
   * Converts a PCM buffer of any format into interleaved float samples.
   *
   * @param src       the source buffer, in any PCM format and layout.
   * @param dst       the interleaved float destination, with as many channels as `src`.
   * @param numFrames the number of frames to convert.
   * @return the number of frames actually converted.
   */
  static AudioFrameCount
  toInterleavedFloat(const AudioPCMBuffer& src, float* dst, AudioFrameCount numFrames);

  /**
   * Converts a float PCM buffer into an interleaved buffer of the given format.
   *
   * @param src       the source buffer, in the standard float format.
   * @param dst       the interleaved destination buffer.
   * @param dstFormat the PCM format of the destination buffer.
   * @param numFrames the number of frames to convert.
   * @return the number of frames actually converted.
   */
  static AudioFrameCount convertFromPCMBuffer(const AudioPCMBuffer& src,
    void* dst,
    const AudioFormat& dstFormat,
    AudioFrameCount numFrames);

  /**
   * Converts an interleaved buffer of the given format into a float PCM buffer.
   *
   * @param src       the interleaved source buffer.
   * @param srcFormat the PCM format of the source buffer.
   * @param dst       the destination buffer, in the standard float format.
   * @param numFrames the number of frames to convert.
   * @return the number of frames actually converted.
   */
  static AudioFrameCount convertIntoPCMBuffer(const void* src,
    const AudioFormat& srcFormat,
    AudioPCMBuffer& dst,
    AudioFrameCount numFrames);

  /**
   * Copies float frames between two buffer lists.
   *
   * @discussion
   * This is the copy a node performs inside `renderBlock`, where both buffers are views
   * described by an `AudioBufferList` rather than owning `AudioPCMBuffer`s.
   *
   * The destination is always the standard deinterleaved float format, one plane per
   * channel, because that is the layout `AudioNodeImpl` gives its render buffers. The
   * source may be either that layout or the interleaved one. The byte sizes of both lists
   * limit how much is copied, so a short source copies fewer frames and a small destination
   * is filled up to what it can hold.
   *
   * @param src       the source list, in float.
   * @param dst       the destination list, deinterleaved float.
   * @param numFrames the number of frames to copy at most.
   * @return the number of frames actually copied.
   */
  static AudioFrameCount copyFloatBufferList(const AudioBufferList& src,
    AudioBufferList& dst,
    AudioFrameCount numFrames);

  /**
   * Writes silence into every frame of every buffer of a deinterleaved float list.
   *
   * @discussion
   * A render buffer is silent before `renderBlock` is called, so a node only needs this to
   * undo what it has already written, for instance when it fails halfway through a block.
   */
  static void silenceFloatBufferList(AudioBufferList& buffer, AudioFrameCount numFrames);

  /**
   * Deinterleaves interleaved float samples into a deinterleaved float buffer list.
   *
   * @param src       the interleaved source samples, with as many channels as the list.
   * @param dst       the destination list, deinterleaved float.
   * @param numFrames the number of frames to convert.
   * @return the number of frames actually converted.
   */
  static AudioFrameCount
  interleavedFloatIntoBufferList(const float* src, AudioBufferList& dst, AudioFrameCount numFrames);

  /**
   * Interleaves a deinterleaved float buffer list into interleaved float samples.
   *
   * @param src       the source list, deinterleaved float.
   * @param dst       the interleaved destination samples, with as many channels as the list.
   * @param numFrames the number of frames to convert.
   * @return the number of frames actually converted.
   */
  static AudioFrameCount
  interleaveBufferListToFloat(const AudioBufferList& src, float* dst, AudioFrameCount numFrames);

  /**
   * Converts an AudioBufferList of any PCM format into a deinterleaved float buffer list.
   *
   * @discussion
   * This is how a buffer list that a client supplied is read into a render buffer inside
   * `renderBlock`: the list may be interleaved or deinterleaved, and its layout may differ
   * from the graph's. Its own byte sizes limit how much is copied, so a short list simply
   * converts fewer frames.
   *
   * @param src       the source list, interleaved or deinterleaved, in `srcFormat`.
   * @param srcFormat the PCM format of the source buffer list.
   * @param dst       the destination list, deinterleaved float.
   * @param numFrames the number of frames to convert at most.
   * @return the number of frames actually converted.
   */
  static AudioFrameCount convertBufferListIntoFloatBufferList(const AudioBufferList& src,
    const AudioFormat& srcFormat,
    AudioBufferList& dst,
    AudioFrameCount numFrames);

  /**
   * Writes silence into an interleaved buffer of the given format.
   *
   * @param dst       the interleaved buffer to silence.
   * @param format    the PCM format of the buffer.
   * @param numFrames the number of frames to silence.
   */
  static void silenceBuffer(void* dst, const AudioFormat& format, AudioFrameCount numFrames);

  /**
   * Silences the first `numFrames` frames of a PCM buffer, whatever its layout.
   */
  static void silencePCMBuffer(AudioPCMBuffer& buffer, AudioFrameCount numFrames);

  /**
   * Silences `numFrames` frames of a PCM buffer starting at `startFrame`.
   *
   * A node that produces less than the block the graph asked for leaves the rest of its
   * buffer holding the samples of an earlier block, which the next node in the chain would
   * render as noise: this is what the graph uses to turn the tail of a short block into
   * silence.
   */
  static void silencePCMBuffer(AudioPCMBuffer& buffer, AudioFrameCount startFrame,
      AudioFrameCount numFrames);

  /**
   * Returns the standard deinterleaved float format with the sample rate and channel
   * count of the given format.
   *
   * @param format The format whose sample rate and channel count are kept.
   * @return The standard format, or `std::nullopt` when the channel count cannot be
   *         expressed as a standard format.
   */
  static std::optional<AudioFormat> standardFormat(const AudioFormat& format);

  /**
   * Returns the format a stream's data callback delivers, i.e. the stream's own format
   * interpreted as interleaved samples.
   */
  static AudioFormat interleavedFormatForStream(const AudioStream& stream);
};

} // namespace coral

#endif // CORAL_AUDIO_GRAPH_UTILS_H
