/*
 * Copyright (C) 2022 The Coral Project
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

#ifndef CORAL_AUDIO_RESAMPLE_H
#define CORAL_AUDIO_RESAMPLE_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>

#include <media/Configuration.h>
#include <media/SampleBuffer.h>

#include <memory>
#include <string>

namespace coral {

/**
 * The Resample class is used to resample audio data for many sequential frames.
 */
class Resample : public AudioNoncopyable {
public:
  /**
   * The factory to create a Resample
   */
  class Factory {
  public:
    virtual ~Factory() = default;

    virtual std::unique_ptr<Resample>
    createResample(const AudioConfiguration& inputConfig, const AudioConfiguration& outputConfig) = 0;

    virtual std::unique_ptr<Resample>
    createResample(const AudioFormat& inputFormat, const AudioFormat& outputFormat) = 0;
  };

  /**
   * Init the resample using input format and output format
   *
   * @param inputConfig   The input configuration of the audio buffer
   * @param outputConfig  The output configuration of the audio buffer
   *
   * @return Returns true if success, otherwise, return false
   */
  virtual bool init(const AudioConfiguration& inputConfig, const AudioConfiguration& outputConfig) = 0;

  /**
   * Init the resample using input format and output format
   *
   * @param inputFormat The input format of the audio buffer
   * @param outputFormat The output format of the audio buffer
   * @return Returns true if success, otherwise, return false
   */
  virtual bool init(const AudioFormat& inputFormat, const AudioFormat& outputFormat) = 0;

  /**
   * Resamples audio sample data.
   *
   * @param inputBuffer  The input sample buffer to be resampled.
   * @param outputBuffer The output sample buffer to store resampled data.
   * @return the resampled sample buffer if success, otherwise, returns nullptr
   */
  virtual bool resample(const std::shared_ptr<AVSampleBuffer>& inputBuffer,
    std::shared_ptr<AVSampleBuffer>& outputBuffer) = 0;

  /**
   * Resamples audio sample data.
   *
   * @param inputBuffer  The input sample buffer to be resampled.
   *                     It's format must be same as input config.
   * @param outputBuffer The output sample buffer to store resampled data.
   *                     It's format must be same as output config.
   * @return the resampled sample buffer if success, otherwise, returns nullptr
   */
  virtual bool resample(const std::unique_ptr<AudioPCMBuffer>& inputBuffer,
    std::unique_ptr<AudioPCMBuffer>& outputBuffer) = 0;

  /**
   * Resamples audio pcm data
   *
   * @param inputData       the input audio data buffer.
   *                        It's format must be same as the input's config.
   * @param inputFrames     the input number of frames
   * @param outputData      the output audio data buffer.
   *                        It's format must be same as the output's config
   * @param maxOutputFrames the max resampled frames
   * @return the actually resampled number of frames if success, or negative
   * value on failure.
   */
  virtual int resample(const uint8_t* inputData, int inputFrames, uint8_t* outputData, int maxOutputFrames) = 0;

  /**
   * Clears any buffered data and pending output.
   */
  virtual void flush() = 0;

  /**
   * Release the audio resample
   */
  virtual void release() = 0;

  /**
   * Return the name of the audio resample
   */
  [[nodiscard]] virtual std::string getName() const = 0;
};

using ResampleFactory = Resample::Factory;

/**
 * Creates a builtin Resample factory
 */
std::unique_ptr<ResampleFactory> CreateBuiltinResampleFactory();

} // namespace coral

#endif // CORAL_AUDIO_RESAMPLE_H
