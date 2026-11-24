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

#ifndef CORAL_AUDIO_CONVERTER_H
#define CORAL_AUDIO_CONVERTER_H

#include <coral/audio/AudioBuffer.h>
#include <coral/audio/AudioFormat.h>
#include <coral/audio/AudioTypes.h>

#include <codec/Error.h>
#include <resampler/MultiChannelResampler.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace coral {

/**
 * AudioConverter converts streams of audio between various formats.
 */
class AudioConverter {
public:
  enum class PrimeMethod : uint8_t {
    //!< Primes with leading + trailing input frames.
    kPre = 0,
    //!< Only primes with trailing (zero latency). Leading frames are assumed to be silence.
    kNormal = 1,
    //!< Acts in "latency" mode. Both leading and trailing frames assumed to be silence.
    kNone = 2
  };

  /**
   * This struct is the value of the primeInfo property and specifies priming information.
   *
   * @field leadingFrames
   *     Specifies the number of leading (previous) input frames, relative to the normal/desired
   *     start input frame, required by the converter to perform a high quality conversion. If
   *     using PrimeMethod::kPre, the client should "pre-seek" the input stream provided through
   *     the input proc by leadingFrames. If no frames are available previous to the desired
   *     input start frame (because, for example, the desired start frame is at the very
   *     beginning of available audio), then provide "leadingFrames" worth of initial zero frames
   *     in the input proc.  Do not "pre-seek" in the default case of PrimeMethod::kNormal or
   *     when using PrimeMethod::kNone.
   *
   * @field trailingFrames
   *     Specifies the number of trailing input frames (past the normal/expected end input frame)
   *     required by the converter to perform a high quality conversion.  The client should be
   *     prepared to provide this number of additional input frames except when using PrimeMethod::kNone.
   *     If no more frames of input are available in the input stream (because, for example,
   *     the desired end frame is at the end of an audio file), then zero (silent) trailing frames
   *     will be synthesized for the client.
   */
  struct PrimeInfo {
    AudioFrameCount leadingFrames;
    AudioFrameCount trailingFrames;
  };

  /**
   * Creates an audio converter object from the specified input and output formats.
   *
   * @param fromFormat The input format.
   * @param toFormat The output format.
   * @return nullptr if the format conversion is not possible.
   */
  static std::unique_ptr<AudioConverter>
  InitFromFormat(const AudioFormat& fromFormat, const AudioFormat& toFormat);

  /**
   * Resets the converter so that a new stream may be converted.
   */
  void reset();

  /**
   * Returns the format of the input audio stream. (NB. AudioFormat includes the channel layout)
   */
  [[nodiscard]] AudioFormat getInputFormat() const {
    return mInputFormat;
  }

  /**
   * Returns the format of the output audio stream. (NB. AudioFormat includes the channel layout)
   */
  [[nodiscard]] AudioFormat getOutputFormat() const {
    return mOutputFormat;
  }

  /**
   * If true and channel remapping is necessary, then channels will be mixed as appropriate instead of remapped.
   * Default value is false.
   */
  [[nodiscard]] bool isDownmix() const {
    return mDownMix;
  }

  void setDownMix(const bool downMix) {
    mDownMix = downMix;
  }

  /**
   * If true will turn on dither, if dither makes sense in given the current formats and settings.
   * Default value is false.
   */
  [[nodiscard]] bool isDither() const {
    return mDither;
  }

  void setDither(const bool dither) {
    mDither = dither;
  }

  /**
   * Returns the AudioQuality for sample rate converter.
   */
  [[nodiscard]] AudioQuality getSampleRateConverterQuality() const {
    return mSampleRateConverterQuality;
  }

  void setSampleRateConverterQuality(const AudioQuality Quality) {
    mSampleRateConverterQuality = Quality;
  }

  /**
   * Returns the priming method to be used by the sample rate converter or decoder.
   */
  [[nodiscard]] PrimeMethod getPrimeMethod() const {
    return mPrimeMethod;
  }

  void setPrimeMethod(const PrimeMethod primeMethod) {
    mPrimeMethod = primeMethod;
    mPrimeInfo = preparePrimeInfo();
  }

  /**
   * Returns the the number of priming frames .
   */
  [[nodiscard]] PrimeInfo getPrimeInfo() const {
    return mPrimeInfo;
  }

  /**
   * Perform a simple conversion. That is, a conversion which does not involve codecs or sample rate conversion.
   *
   * @param inputBuffer The input buffer.
   * @param outputBuffer The output buffer.
   * @return An error if the conversion fails or std::nullopt on success.
   *
   * @discussion
   * The output buffer's frameCapacity should be at least at large as the inputBuffer's frameLength.
   */
  std::optional<Error>
  convertToBuffer(const std::unique_ptr<AudioPCMBuffer>& inputBuffer, std::unique_ptr<AudioPCMBuffer>& outputBuffer);

  /**
   * You must return one of these codes from your InputBlock.
   */
  enum class InputStatus : uint8_t {
    /**
     * This is the normal case where you supply data to the converter.
     */
    HaveData = 0,
    /**
     * If you are out of data for now, set *inNumberOfPackets = 0 and return NoDataNow and the conversion
     * routine will return as much output as could be converted with the input already supplied.
     */
    NoDataNow = 1,
    /**
     * If you are at the end of stream, set *inNumberOfPackets = 0 and return EndOfStream.
     */
    EndOfStream = 2
  };

  enum class OutputStatus : uint8_t {
    /**
     * All of the requested data was returned.
     */
    HaveData = 0,
    /**
     * Not enough input was available to satisfy the request at the current time.
     * The output buffer contains as much as could be converted.
     */
    InputRanDry = 1,
    /**
     * The end of stream has been reached. No data was returned.
     */
    EndOfStream = 2,
    /**
     * An error occurred.
     */
    Error = 3,
  };

  /**
   * A block which will be called by convertToBuffer to get input data as needed.
   *
   * @param inNumberOfPackets
   *     This will be the number of packets required to complete the request.
   *     You may supply more or less that this amount. If less, then the input block will get called again.
   * @param outStatus
   *     The block must set the appropriate InputStatus enum value.
   *     If you have supplied data, set outStatus to InputStatus::HaveData and return an AudioBuffer.
   *     If you are out of data for now, set outStatus to InputStatus::NoDataNow and return nil, and the
   *     conversion routine will return as much output as could be converted with the input already supplied.
   *     If you are at the end of stream, set outStatus to InputStatus::EndOfStream, and return nil.
   * @return
   *     An AudioBuffer containing data to be converted, or nil if at end of stream or no data is available.
   */
  typedef
  std::function<std::unique_ptr<AudioBuffer>(AudioPacketCount inNumberOfPackets, InputStatus* outStatus)> InputBlock;

  /**
   *
   * @param outputBuffer The output buffer.
   * @param outError An error if the conversion fails.
   * @param inputBlock The input block to provide audio data.
   * @return An OutputStatus is returned.
   * @discussion
   *     It attempts to fill the buffer to its capacity. On return, the buffer's length indicates the number
   *     of sample frames successfully converted.
   */
  OutputStatus
  convertToBuffer(const std::unique_ptr<AudioBuffer>& outputBuffer, Error** outError, const InputBlock& inputBlock);

private:
  explicit AudioConverter(AudioFormat inputFormat, AudioFormat outputFormat) noexcept
    : mInputFormat(std::move(inputFormat)),
      mOutputFormat(std::move(outputFormat)) {
    configure();
  }

  /** Number of filter taps a quality level asks for. */
  [[nodiscard]] static int tapsForQuality(AudioQuality quality) noexcept;

  /** Caches the channel counts and seeds the noise generator. */
  void configure();

  [[nodiscard]] PrimeInfo preparePrimeInfo() const;
/**
   * Creates (or reuses) the sample rate converter for `channels` output channels.
   *
   * @return `false` when no resampler could be built for the two sample rates.
   */
  bool ensureResampler(int32_t channels);

  /** Maps AudioQuality onto the resampler's own quality scale. */
  [[nodiscard]] MultiChannelResampler::Quality qualityToResamplerQuality() const;

  /** Reads `inputBuffer` into the planar float scratch of the input format. */
  bool prepareInput(const AudioPCMBuffer* inputBuffer);

  /** Reads any PCM buffer into `planar`, de-interleaving as needed. */
  static bool loadPlanarFloat(const AudioPCMBuffer& buffer,
      const AudioFormat& format,
      AudioFrameCount frames,
      std::vector<float>& planar,
      AudioFrameCount& planarFrames);

  /** Reads interleaved raw PCM from an InputBlock into the planar scratch. */
  bool loadInterleaved(const void* data, AudioFrameCount frames, uint32_t channels);

  /** Maps the planar input scratch onto the output channel layout. */
  bool remapChannels();

  /** Runs the sample rate converter over the mapped frames, interleaved. */
  size_t resampleMapped(size_t maxOutputFrames);

  /** Returns the next triangular-PDF dither sample, or 0 when dither is off. */
  float ditherNoise();

  /**
   * Writes a single sample at `byteOffset` inside a raw buffer.
   *
   * The caller computes the offset, because only it knows whether the buffer
   * holds one channel (planar) or all of them interleaved.
   */
  bool writeSample(AudioBuffer& out, size_t byteOffset, float value);

  /** Writes `frames` planar frames into a single raw buffer. */
  bool writeFrames(AudioBuffer& out,
      AudioFrameCount outOffset,
      AudioFrameCount frames,
      const float* planar,
      size_t planarStride);

  /** Writes `frames` planar frames into a PCM buffer, using its buffer list. */
  bool writePlanarFrames(AudioPCMBuffer& out,
      AudioFrameCount outOffset,
      AudioFrameCount frames,
      const float* planar,
      size_t planarStride);

  AudioFormat mInputFormat;
  AudioFormat mOutputFormat;
  bool mDownMix = false;
  bool mDither = false;
  //!< An AudioQuality value as defined in AudioTypes.h
  AudioQuality mSampleRateConverterQuality = AudioQuality::Fastest;
  //!< Indicates the priming method to be used by the sample rate converter or decoder.
  PrimeMethod mPrimeMethod = PrimeMethod::kNormal;
  //!< Indicates the the number of priming frames .
  PrimeInfo mPrimeInfo{};

  // Caches channels
  uint32_t mInChannels = 0;
  uint32_t mOutChannels = 0;

  // sample rate conversion
  std::unique_ptr<MultiChannelResampler> mResampler;
  int32_t mResamplerRate = 0;
  int32_t mResamplerChannels = 0;
  uint64_t mResamplerFrames = 0;
  bool mReachedEndOfStream = false;

  /** Input frames, planar float, one plane per channel. */
  std::vector<float> mPlanarInput;
  AudioFrameCount mPlanarFrames = 0;
  /** Channel-mapped frames, planar float, one plane per output channel. */
  std::vector<float> mMapped;
  AudioFrameCount mMappedFrames = 0;
  /** Resampler input, interleaved. */
  std::vector<float> mResamplerInput;
  /** Resampler output, interleaved. */
  std::vector<float> mResampled;
  /** Frames already handed out at the current output position. */
  AudioFrameCount mOutputFramePos = 0;
  /** Capacity of the buffer passed to the InputBlock based overload. */
  AudioFrameCount mOutputFrameCapacity = 0;
  /** State of the dither noise generator. */
  uint32_t mDitherSeed = 0x2545F491u;
};

} // namespace coral

#endif //CORAL_AUDIO_CONVERTER_H
