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

#ifndef CORAL_AUDIO_OUTPUT_SINK_H
#define CORAL_AUDIO_OUTPUT_SINK_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioFormat.h>

#include <media/Format.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace coral {

/**
 * An audio output sink that consumes audio data.
 *
 * Before starting playback, specify the input audio format by calling {@link configure(Format,
 * int, int[])}.
 *
 * Call {@link handleBuffer(ByteBuffer, int64_t, int)} to write data, and {@link
 * #handleDiscontinuity()} when the data being fed is discontinuous. Call {@link play()} to start
 * playing the written data.
 *
 * Call {@link configure(Format, int, int[])} whenever the input format changes. The sink will
 * be reinitialized on the next call to {@link handleBuffer(ByteBuffer, long, int)}.
 *
 * Call {@link flush()} to prepare the sink to receive audio data from a new playback position.
 *
 * Call {@link playToEndOfStream()} repeatedly to play out all data when no more input buffers
 * will be provided via {@link handleBuffer(ByteBuffer, long, int)} until the next {@link
 * #flush()}. Call {@link reset()} when the instance is no longer required.
 */
class AudioOutputSink : public AudioNoncopyable {
public:
  enum FormatSupport : uint8_t {
    /**
     * The output does not support the format.
     */
    FORMAT_UNSUPPORTED = 0,

    /**
     * The output supports the format, but needs to transcode it internally to do so. Internal
     * transcoding may result in lower quality and higher CPU load in some cases.
     */
    FORMAT_SUPPORTED_WITH_TRANSCODING = 1,

    /**
     * The output supports the format directly, without the need for internal transcoding.
     */
    FORMAT_SUPPORTED_DIRECTLY = 2,
  };

  /**
   * Returned by {@link getCurrentPositionUs(boolean)} when the position is not set.
   */
  static constexpr int64_t CURRENT_POSITION_NOT_SET = INT64_MIN;

  /**
   * Listener for audio sink events.
   */
  class Listener {
  public:
    virtual ~Listener() = default;

    /**
     * Called when the audio sink handles a buffer whose timestamp is discontinuous with the last
     * buffer handled since it was reset.
     */
    virtual void onPositionDiscontinuity() = 0;

    /** Called when the offload buffer has been partially emptied. */
    virtual void onOffloadBufferEmptying() {
    }

    /**
     * Called when the offload buffer has been filled completely.
     *
     * @param bufferEmptyingDeadlineMs Maximum time in milliseconds until {@link
     *     onOffloadBufferEmptying()} will be called.
     */
    virtual void onOffloadBufferFull(int64_t bufferEmptyingDeadlineMs) {
    }
  };

  /**
   * Creates a default AudioOutput
   */
  static std::shared_ptr<AudioOutputSink> CreateOutput();

  /**
   * Sets the listener for sink events, which should be the audio renderer.
   *
   * @param listener The listener for sink events, which should be the audio renderer.
   */
  virtual void setListener(const std::shared_ptr<Listener>& listener) = 0;

  /**
   * Returns whether the output supports a given {@link MediaFormat}.
   *
   * @param format The codec format.
   *
   * @return Whether the output supports the format.
   */
  bool supportsFormat(const std::shared_ptr<MediaFormat>& format);

  /**
   * Returns the level of support that the sink provides for a given {@link MediaFormat}.
   *
   * @param format The format.
   * @return The level of support provided.
   */
  FormatSupport getFormatSupport(const std::shared_ptr<MediaFormat>& format);

  /**
   * Returns the position in the stream starting at zero, in microseconds, or {@link
   * #CURRENT_POSITION_NOT_SET} if it is not yet available.
   *
   * @param sourceEnded Specify {@code true} if no more input buffers will be provided.
   * @return The position relative to the start of playback, in microseconds.
   */
  virtual int64_t getCurrentPositionUs(bool sourceEnded) = 0;

  /**
   * Configures (or reconfigures) the output.
   *
   * @param format The format of audio data provided in the input buffers.
   */
  virtual void configure(const AudioFormat& format) = 0;

  /**
   * Starts or resumes consuming audio if initialized.
   */
  virtual void play() = 0;

  /**
   * Signals to the output that the next buffer may be discontinuous with the
   * previous buffer.
   */
  virtual void handleDiscontinuity() = 0;

  /**
   * Attempts to process data, starting from its current position and ending at
   * its limit (exclusive). The position of the buffer is advanced by the number
   * of bytes that were handled.
   * {@link Listener::onPositionDiscontinuity()} will be called if {@code
   * presentationTimeUs} is discontinuous with the last buffer handled since the
   * last reset.
   *
   * Returns whether the data was handled in full. If the data was not handled
   * in full then the same buffer must be provided to subsequent calls until it
   * has been fully consumed, except in the case of an intervening call to
   * {@link flush()} (or to {@link configure(AudioFormat)} that causes the
   * output to be flushed).
   *
   * @param buffer The buffer containing audio data.
   * @param byteBufferSize the byte size of the buffer.
   * @param presentationTimeUs The presentation timestamp of the buffer in microseconds.
   * @return Whether the buffer was handled fully.
   */
  virtual bool handleBuffer(const void* buffer, size_t byteBufferSize, int64_t presentationTimeUs) = 0;

  /**
   * Processes any remaining data. {@link isEnded()} will return {@code true}
   * when no data remains.
   */
  virtual void playToEndOfStream() = 0;

  /**
   * Returns whether {@link playToEndOfStream} has been called and all buffers
   * have been processed.
   */
  virtual bool isEnded() = 0;

  /**
   * Returns whether the output has data pending that has not been consumed yet.
   */
  virtual bool hasPendingData() = 0;

  /**
   * Returns whether the platform audio stream exposes an XRun (underrun/overrun) counter.
   */
  [[nodiscard]] virtual bool isXRunCountSupported() const {
    return false;
  }

  /**
   * Returns the platform XRun count, or 0 when the platform cannot expose one. The counter belongs
   * to the open stream; it is not reset by a seek or a flush.
   */
  [[nodiscard]] virtual uint32_t getXRunCount() {
    return 0;
  }

  /**
   * Returns whether the output can accept another buffer. A sink whose consumption runs on a data
   * callback is fed from inside that callback, so feedback is not the level of the queued data but
   * whether there is still room for more: reporting the queue as "pending" while it is full makes
   * the renderer that owns this sink look unready exactly when it is most able to take data.
   */
  virtual bool hasCapacityForNextBuffer() = 0;

  /**
   * Attempts to set the rate.
   *
   * @param rate The new rate to attempt to set.
   */
  virtual void setRate(float rate) = 0;

  /**
   * Returns the rate for consume
   */
  [[nodiscard]] virtual float getRate() const = 0;

  /**
   * Attempts to set the pitch.
   *
   * @param pitch The new pitch to attempt to set.
   */
  virtual void setPitch(float pitch) = 0;

  /**
   * Returns the pitch for consume
   */
  [[nodiscard]] virtual float getPitch() const = 0;

  /**
   * Sets the playback volume.
   *
   * @param volume A volume in the range [0.0, 1.0].
   */
  virtual void setVolume(float volume) = 0;

  /**
   * Pauses playback.
   */
  virtual void pause() = 0;

  /**
   * Flushes the output, after which it is ready to receive buffers from a new position.
   *
   * The audio session may remain active until {@link release()} is called.
   */
  virtual void flush() = 0;

  /**
   * Resets the renderer, releasing any resources that it currently holds.
   */
  virtual void release() = 0;

protected:
  std::atomic_bool mEnableFloatOutput{false};
};

} // namespace coral

#endif // CORAL_AUDIO_OUTPUT_SINK_H
