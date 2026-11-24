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

#ifndef CORAL_PRIVATE_AUDIO_STREAM_BUILDER_H
#define CORAL_PRIVATE_AUDIO_STREAM_BUILDER_H

#include "private/audio/AudioStreamBase.h"

#include <memory>

namespace coral {

class AudioStreamBuilder : public AudioStreamBase {
public:

  /**
   * Request the direction for a stream. The default is Direction::Output.
   *
   * @param direction Direction::Output or Direction::Input
   */
  AudioStreamBuilder& setDirection(Direction direction);

  /**
   * Set the requested buffer capacity in frames.
   * BufferCapacityInFrames is the maximum possible BufferSizeInFrames.
   *
   * The final stream capacity may differ. For AAudio it should be at least this big.
   * For OpenSL ES, it could be smaller.
   *
   * Default is kUnspecified.
   *
   * @param bufferCapacityInFrames the desired buffer capacity in frames or kUnspecified
   * @return pointer to the builder so calls can be chained
   */
  AudioStreamBuilder& setBufferCapacityInFrames(uint32_t bufferCapacityInFrames);

  /**
   * Request a specific number of channels.
   *
   * Default is kUnspecified. If the value is unspecified then
   * the application should query for the actual value after the stream is
   * opened.
   *
   * As the channel count here may be different from the corresponding channel
   * count of provided channel mask used in setChannelMask(). The last called
   * will be respected if this function and setChannelMask() are called.
   */
  AudioStreamBuilder& setChannelCount(AudioChannelCount channelCount);

  /**
   * Request a specific sample rate in Hz.
   *
   * Default is kUnspecified. If the value is unspecified then
   * the application should query for the actual value after the stream is
   * opened.
   *
   * Technically, this should be called the "frame rate" or "frames per second",
   * because it refers to the number of complete frames transferred per second.
   * But it is traditionally called "sample rate". Se we use that term.
   */
  AudioStreamBuilder& setSampleRate(uint64_t sampleRate);

  /**
   * Specifies an object to handle data related callbacks from the underlying API.
   *
   * <strong>Important: See AudioStreamCallback for restrictions on what may be
   * called from the callback methods.</strong>
   *
   * We pass a shared_ptr so that the callback object cannot be deleted before
   * the stream is deleted.
   *
   * @param callback The pointer of the DataCallback
   * @return pointer to the builder so calls can be chained
   */
  AudioStreamBuilder& setDataCallback(const std::shared_ptr<AudioStreamDataCallback>& callback);

  /**
   * Specifies an object to handle error related callbacks from the underlying
   * API. This can occur when a stream is disconnected because a headset is
   * plugged in or unplugged. It can also occur if the audio service fails or if
   * an exclusive stream is stolen by another stream.
   *
   * <strong>Important: See AudioStreamCallback for restrictions on what may be
   * called from the callback methods.</strong>
   *
   * <strong>When an error callback occurs, the associated stream must be
   * stopped and closed in a separate thread.</strong>
   *
   * We pass a shared_ptr so that the errorCallback object cannot be deleted
   * before the stream is deleted. If the stream was created using a shared_ptr
   * then the stream cannot be deleted before the error callback has finished
   * running.
   *
   * @param callback The pointer of ErrorCallback.
   * @return pointer to the builder so calls can be chained
   */
  AudioStreamBuilder& setErrorCallback(const std::shared_ptr<AudioStreamErrorCallback>& callback);

  /**
   * Request a sample data format, for example Format::kFloat.
   *
   * Default is Format::kUnspecified. If the value is unspecified then
   * the application should query for the actual value after the stream is opened.
   */
  AudioStreamBuilder& setFormat(AudioSampleFormat format);

  /**
   * Request a specific number of frames for the data callback.
   *
   * Default is kUnspecified. If the value is unspecified then
   * the actual number may vary from callback to callback.
   *
   * If an application can handle a varying number of frames then we recommend
   * leaving this unspecified. This allow the underlying API to optimize
   * the callbacks. But if your application is, for example, doing FFTs or other block
   * oriented operations, then call this function to get the sizes you need.
   *
   * Calling setFramesPerDataCallback() does not guarantee anything about timing.
   * This just collects the data into a the number of frames that your app requires.
   * We encourage leaving this unspecified in most cases.
   *
   * If this number is larger than the burst size, some bursts will not receive a callback.
   * If this number is smaller than the burst size, there may be multiple callbacks in a single
   * burst.
   *
   * @param framesPerCallback
   * @return pointer to the builder so calls can be chained
   */
  AudioStreamBuilder& setFramesPerDataCallback(uint32_t framesPerCallback);

  /**
   * Request a performance level for the stream.
   *
   * This will determine the latency, the power consumption, and the level of
   * protection from glitches.
   *
   * @param mode for example, PerformanceMode::LowLatency
   * @return pointer to the builder so calls can be chained
   */
  AudioStreamBuilder& setPerformanceMode(PerformanceMode mode);

  /**
   * Request a mode for sharing the device.
   * The requested sharing mode may not be available.
   * So the application should query for the actual mode after the stream is
   * opened.
   *
   * @param mode SharingMode::Shared or SharingMode::Exclusive
   * @return pointer to the builder so calls can be chained
   */
  AudioStreamBuilder& setSharingMode(SharingMode mode);

  /**
   * Create and open a stream object based on the current settings.
   *
   * The caller shares the pointer to the AudioStream object.
   * The shared_ptr is used to prevent the stream from being deleted while it is being used by callbacks.
   */
  Result openStream(std::shared_ptr<AudioStream>& stream);

private:
  /**
   * Use this internally to implement opening with a shared_ptr.
   *
   * @param streamPP pointer to a variable to receive the stream address
   * @return Result::OK if successful or a negative error code.
   */
  Result openStreamInternal(AudioStream **streamPP);

  /**
   * Create an AudioStream object.
   * The AudioStream must be opened before use.
   */
  [[nodiscard]] AudioStream* build() const;
};

} // namespace coral

#endif //CORAL_PRIVATE_AUDIO_STREAM_BUILDER_H
