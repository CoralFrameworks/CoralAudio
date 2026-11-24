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

#include "private/graph/AudioUnitTimePitchImpl.h"

#include "private/graph/AudioGraphUtils.h"

#include <minilog/log.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace coral {

namespace {

constexpr float kMinRate = 1.0f / 32.0f;
constexpr float kMaxRate = 32.0f;
constexpr float kMinOverlap = 3.0f;
constexpr float kMaxOverlap = 32.0f;
constexpr float kDefaultOverlap = 8.0f;
constexpr double kTwoPi = 6.283185307179586;

double wrapPhase(const double phase) {
  return phase - kTwoPi * std::round(phase / kTwoPi);
}
//!< A guard against a pathological parameter combination making one block unbounded.
constexpr int kMaxFramesPerBlock = 512;
//!< The output FIFO is filled to this much beyond a block before the loop stops.
constexpr size_t kOutputMargin = 0;

/**
 * The FFT size that keeps the analysis window around 40 ms at the usual rates.
 */
size_t fftSizeForSampleRate(const double sampleRate) {
  if (sampleRate >= 32000.0) {
    return 2048;
  }
  if (sampleRate >= 16000.0) {
    return 1024;
  }
  return 512;
}

/** The extra frames a block may request on top of `frames * rate`, to prime the window. */
size_t primingMargin(const size_t fftSize) {
  return fftSize + 2 * (fftSize / 3) + 8;
}

/**
 * A fixed-capacity interleaved frame FIFO.
 *
 * @discussion
 * The capacity is chosen by `prepare` and never changes, so pushing and reading from a render
 * thread allocates nothing. Frame offsets are relative to the oldest frame still held.
 */
class FloatFifo {
public:
  void init(const size_t capacityFrames, const size_t channels) {
    mCapacity = std::max<size_t>(capacityFrames, 1);
    mChannels = std::max<size_t>(channels, 1);
    mData.assign(mCapacity * mChannels, 0.0f);
    mRead = 0;
    mCount = 0;
  }

  [[nodiscard]] size_t available() const {
    return mCount;
  }

  [[nodiscard]] size_t space() const {
    return mCapacity - mCount;
  }

  [[nodiscard]] size_t channels() const {
    return mChannels;
  }

  bool push(const float* interleaved, const size_t frames) {
    if (frames == 0) {
      return true;
    }
    if (frames > space()) {
      return false;
    }
    const auto write = (mRead + mCount) % mCapacity;
    const auto first = std::min(frames, mCapacity - write);
    std::copy_n(interleaved, first * mChannels, mData.data() + write * mChannels);
    if (frames > first) {
      std::copy_n(interleaved + first * mChannels, (frames - first) * mChannels, mData.data());
    }
    mCount += frames;
    return true;
  }

  [[nodiscard]] float at(const size_t frame, const size_t channel) const {
    return mData[((mRead + frame) % mCapacity) * mChannels + channel];
  }

  size_t pop(float* interleaved, const size_t frames) {
    const auto count = std::min(frames, mCount);
    const auto first = std::min(count, mCapacity - mRead);
    std::copy_n(mData.data() + mRead * mChannels, first * mChannels, interleaved);
    if (count > first) {
      std::copy_n(mData.data(), (count - first) * mChannels, interleaved + first * mChannels);
    }
    mRead = (mRead + count) % mCapacity;
    mCount -= count;
    return count;
  }

  void discard(const size_t frames) {
    const auto count = std::min(frames, mCount);
    mRead = (mRead + count) % mCapacity;
    mCount -= count;
  }

  void clear() {
    mRead = 0;
    mCount = 0;
  }

private:
  std::vector<float> mData;
  size_t mCapacity = 0;
  size_t mChannels = 0;
  size_t mRead = 0;
  size_t mCount = 0;
};

/**
 * An iterative radix-2 complex FFT.
 *
 * @discussion
 * The twiddles and the bit-reversal table are built when the unit is prepared, so a transform
 * only performs arithmetic on its two scratch arrays.
 */
class Fft {
public:
  void init(const size_t size) {
    mSize = size;
    mReverse.resize(size);
    auto bits = 0u;
    while ((size_t{ 1 } << bits) < size) {
      ++bits;
    }
    for (size_t i = 0; i < size; ++i) {
      size_t reversed = 0;
      for (auto bit = 0u; bit < bits; ++bit) {
        if ((i & (size_t{ 1 } << bit)) != 0) {
          reversed |= size_t{ 1 } << (bits - 1 - bit);
        }
      }
      mReverse[i] = reversed;
    }
    mCos.resize(size / 2);
    mSin.resize(size / 2);
    for (size_t i = 0; i < size / 2; ++i) {
      const auto angle = kTwoPi * static_cast<double>(i) / static_cast<double>(size);
      mCos[i] = static_cast<float>(std::cos(angle));
      mSin[i] = static_cast<float>(std::sin(angle));
    }
  }

  void forward(std::vector<float>& real, std::vector<float>& imaginary) const {
    transform(real, imaginary, false);
  }

  void inverse(std::vector<float>& real, std::vector<float>& imaginary) const {
    transform(real, imaginary, true);
  }

private:
  void transform(std::vector<float>& real, std::vector<float>& imaginary, const bool inverse) const {
    const auto size = mSize;
    for (size_t i = 0; i < size; ++i) {
      const auto j = mReverse[i];
      if (i < j) {
        std::swap(real[i], real[j]);
        std::swap(imaginary[i], imaginary[j]);
      }
    }

    for (size_t length = 2; length <= size; length <<= 1) {
      const auto half = length / 2;
      const auto step = size / length;
      for (size_t start = 0; start < size; start += length) {
        for (size_t j = 0; j < half; ++j) {
          const auto twiddle = j * step;
          const auto wr = mCos[twiddle];
          const auto wi = inverse ? mSin[twiddle] : -mSin[twiddle];
          const auto a = start + j;
          const auto b = a + half;
          const auto br = real[b] * wr - imaginary[b] * wi;
          const auto bi = real[b] * wi + imaginary[b] * wr;
          real[b] = real[a] - br;
          imaginary[b] = imaginary[a] - bi;
          real[a] += br;
          imaginary[a] += bi;
        }
      }
    }

    if (inverse) {
      const auto scale = 1.0f / static_cast<float>(size);
      for (size_t i = 0; i < size; ++i) {
        real[i] *= scale;
        imaginary[i] *= scale;
      }
    }
  }

  size_t mSize = 0;
  std::vector<size_t> mReverse;
  std::vector<float> mCos;
  std::vector<float> mSin;
};

} // namespace anonymous

/**
 * The render-thread state of the phase vocoder.
 */
class AudioUnitTimePitchImpl::Impl {
public:
  Impl() = default;

  void init(const AudioFormat& format, const AudioFrameCount maxRenderFrames) {
    mFormat = format;
    mChannels = std::max<size_t>(format.getChannelCount(), 1);
    mFftSize = fftSizeForSampleRate(format.getSampleRate());
    mMaxRenderFrames = std::max<AudioFrameCount>(maxRenderFrames, 1);
    mMaxInputFrames = maxInputFor(mMaxRenderFrames);

    mInput.init(mMaxInputFrames + 8, mChannels);
    mStretched.init(std::max<size_t>(mFftSize * 4, mMaxRenderFrames * 2), mChannels);
    mOutput.init(std::max<size_t>(mFftSize * 2, mMaxRenderFrames * 4), mChannels);

    mInputScratch.assign(static_cast<size_t>(mMaxInputFrames) * mChannels, 0.0f);
    mOutputScratch.assign(static_cast<size_t>(mMaxRenderFrames) * mChannels, 0.0f);
    mStretchScratch.assign(mFftSize * mChannels, 0.0f);
    mResampleScratch.assign(mChannels, 0.0f);

    mWindow.resize(mFftSize);
    for (size_t i = 0; i < mFftSize; ++i) {
      // A periodic Hann window: its square sums to a constant over the analysis hops.
      mWindow[i] = static_cast<float>(
        0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(mFftSize)));
    }

    mFft.init(mFftSize);
    mFftReal.assign(mFftSize, 0.0f);
    mFftImaginary.assign(mFftSize, 0.0f);
    mLastPhase.assign(mChannels, std::vector<float>(mFftSize / 2 + 1, 0.0f));
    mSumPhase.assign(mChannels, std::vector<float>(mFftSize / 2 + 1, 0.0f));
    mMagnitude.assign(mFftSize / 2 + 1, 0.0f);
    mPhase.assign(mFftSize / 2 + 1, 0.0f);
    mDeviation.assign(mFftSize / 2 + 1, 0.0f);
    mPeakSum.assign(mFftSize / 2 + 1, 0.0f);
    mNormalSum.assign(mFftSize / 2 + 1, 0.0f);
    mPeaks.reserve(mFftSize / 2 + 1);
    mOla.assign(mChannels, std::vector<float>(mFftSize, 0.0f));
    mNormalization.assign(mChannels, std::vector<float>(mFftSize, 0.0f));
    mPrepared = true;
    reset();
  }

  void reset() {
    mInput.clear();
    mStretched.clear();
    mOutput.clear();
    for (size_t c = 0; c < mChannels; ++c) {
      std::fill(mOla[c].begin(), mOla[c].end(), 0.0f);
      std::fill(mNormalization[c].begin(), mNormalization[c].end(), 0.0f);
      std::fill(mLastPhase[c].begin(), mLastPhase[c].end(), 0.0f);
      std::fill(mSumPhase[c].begin(), mSumPhase[c].end(), 0.0f);
    }
    mOlaWrite = 0;
    mOlaExtract = 0;
    mInputPosition = 0.0;
    mResamplePosition = 0.0;
    mWarmupRemaining = mFftSize;
    mFramesProcessed = 0;
    mWrittenFrames = 0;
    mOverlapTransition = OverlapTransition::kNone;
    mTransitionGain = 1.0f;
    mTransitionStep = 0.0f;
  }

  [[nodiscard]] size_t hopFor(const float overlap) const {
    return std::max<size_t>(1,
      static_cast<size_t>(std::llround(static_cast<double>(mFftSize) /
                                       static_cast<double>(std::clamp(overlap, kMinOverlap, kMaxOverlap)))));
  }

  /**
   * Starts fading the output out before the overlap, and therefore the synthesis hop, changes.
   */
  void beginOverlapChange(const size_t newHop, const double sampleRate) {
    mPendingHop = newHop;
    if (mOverlapTransition != OverlapTransition::kNone) {
      return;
    }
    mFadeSamples = std::max<size_t>(1, static_cast<size_t>(sampleRate * 0.005));
    mOverlapTransition = OverlapTransition::kFadeOut;
    mTransitionGain = 1.0f;
    mTransitionStep = -1.0f / static_cast<float>(mFadeSamples);
  }

  /**
   * Rebuilds the overlap-add grid for the new hop after the fade out, then fades back in.
   *
   * @discussion The input FIFO and the phase accumulators are kept: only the synthesis grid
   * changes, so the audio stream continues from the same input position and no stop is needed.
   */
  void finishOverlapChange() {
    for (size_t c = 0; c < mChannels; ++c) {
      std::fill(mOla[c].begin(), mOla[c].end(), 0.0f);
      std::fill(mNormalization[c].begin(), mNormalization[c].end(), 0.0f);
    }
    mStretched.clear();
    mOutput.clear();
    mOlaWrite = 0;
    mOlaExtract = 0;
    mResamplePosition = 0.0;
    mWarmupRemaining = 0;
    mActiveHop = mPendingHop != 0 ? mPendingHop : mActiveHop;
    mOverlapTransition = OverlapTransition::kFadeIn;
    mTransitionGain = 0.0f;
    mTransitionStep = 1.0f / static_cast<float>(std::max<size_t>(mFadeSamples, 1));
  }

  [[nodiscard]] AudioFrameCount maxInputFor(const AudioFrameCount frames) const {
    const auto base = static_cast<uint64_t>(frames) * static_cast<uint64_t>(kMaxRate);
    const auto total = base + primingMargin(mFftSize);
    return static_cast<AudioFrameCount>(
      std::min<uint64_t>(total, std::numeric_limits<AudioFrameCount>::max()));
  }

  [[nodiscard]] size_t maxHop() const {
    return std::max<size_t>(mFftSize / static_cast<size_t>(kMinOverlap), 1);
  }

  void processFrame(const double analysisHop, const size_t hop, const double stretch) {
    const auto size = mFftSize;
    const auto bins = size / 2 + 1;
    const auto firstFrame = mFramesProcessed == 0;

    for (size_t c = 0; c < mChannels; ++c) {
      for (size_t i = 0; i < size; ++i) {
        const auto position = mInputPosition + static_cast<double>(i);
        const auto index = static_cast<size_t>(position);
        const auto fraction = static_cast<float>(position - static_cast<double>(index));
        const auto a = mInput.at(index, c);
        const auto b = mInput.at(index + 1, c);
        mFftReal[i] = (a + (b - a) * fraction) * mWindow[i];
        mFftImaginary[i] = 0.0f;
      }

      mFft.forward(mFftReal, mFftImaginary);

      if (firstFrame) {
        // The first frame defines the phase of every bin, so the vocoder starts from the
        // spectrum instead of from an arbitrary zero phase.
        for (size_t k = 0; k < bins; ++k) {
          const auto phase = std::atan2(mFftImaginary[k], mFftReal[k]);
          mLastPhase[c][k] = phase;
          mSumPhase[c][k] = phase;
        }
        if (mFramesProcessed == 0) {
          // The DC and Nyquist bins are real; keep them that way.
          mFftImaginary[0] = 0.0f;
          mFftImaginary[size / 2] = 0.0f;
        }
      } else {
        // Analyze the frame first: the peaks drive the phase of the bins around them, which
        // is what keeps a tone from beating when the hop is not the identity.
        for (size_t k = 0; k < bins; ++k) {
          const auto real = mFftReal[k];
          const auto imaginary = mFftImaginary[k];
          mMagnitude[k] = std::sqrt(real * real + imaginary * imaginary);
          mPhase[k] = static_cast<float>(std::atan2(imaginary, real));
          if (k == 0 || k == size / 2) {
            mDeviation[k] = 0.0f;
          } else {
            const auto omega = kTwoPi * static_cast<double>(k) / static_cast<double>(size);
            const auto deviation =
              static_cast<double>(mPhase[k]) - mLastPhase[c][k] - omega * analysisHop;
            mDeviation[k] = static_cast<float>(wrapPhase(deviation));
          }
        }

        mPeaks.clear();
        for (size_t k = 1; k + 1 < bins; ++k) {
          if (mMagnitude[k] > mMagnitude[k - 1] && mMagnitude[k] >= mMagnitude[k + 1] &&
              mMagnitude[k] > 1.0e-7f) {
            mPeaks.push_back(static_cast<int>(k));
          }
        }

        // Every bin keeps its own phase accumulator, even when its output phase is locked to a
        // peak. Overwriting the accumulator of a bin that is not a peak would corrupt it when it
        // becomes one on a later frame, which is heard as a slow pitch drift.
        for (size_t k = 1; k + 1 < bins; ++k) {
          const auto omega = kTwoPi * static_cast<double>(k) / static_cast<double>(size);
          const auto sum = static_cast<double>(mSumPhase[c][k]) + omega * static_cast<double>(hop) +
                           static_cast<double>(mDeviation[k]) * stretch;
          mNormalSum[k] = static_cast<float>(wrapPhase(sum));
          mSumPhase[c][k] = mNormalSum[k];
          mLastPhase[c][k] = mPhase[k];
        }
        for (const auto peak: mPeaks) {
          const auto k = static_cast<size_t>(peak);
          mPeakSum[k] = mNormalSum[k];
        }

        size_t nearest = 0;
        for (size_t k = 1; k + 1 < bins; ++k) {
          double sum = mNormalSum[k];
          if (!mPeaks.empty()) {
            while (nearest + 1 < mPeaks.size() &&
                   std::abs(mPeaks[nearest + 1] - static_cast<int>(k)) <
                     std::abs(mPeaks[nearest] - static_cast<int>(k))) {
              ++nearest;
            }
            const auto peak = static_cast<size_t>(mPeaks[nearest]);
            if (peak != k) {
              sum = static_cast<double>(mPeakSum[peak]) +
                    (static_cast<double>(mPhase[k]) - static_cast<double>(mPhase[peak]));
            }
          }
          sum = wrapPhase(sum);
          mFftReal[k] = static_cast<float>(mMagnitude[k] * std::cos(sum));
          mFftImaginary[k] = static_cast<float>(mMagnitude[k] * std::sin(sum));
          const auto mirror = size - k;
          mFftReal[mirror] = mFftReal[k];
          mFftImaginary[mirror] = -mFftImaginary[k];
        }

        // The DC and Nyquist bins are real.
        mLastPhase[c][0] = mPhase[0];
        mSumPhase[c][0] = 0.0f;
        mFftReal[0] = mMagnitude[0];
        mFftImaginary[0] = 0.0f;
        const auto nyquist = size / 2;
        mLastPhase[c][nyquist] = mPhase[nyquist];
        mSumPhase[c][nyquist] = 0.0f;
        mFftReal[nyquist] = mMagnitude[nyquist];
        mFftImaginary[nyquist] = 0.0f;
      }

      mFft.inverse(mFftReal, mFftImaginary);

      for (size_t i = 0; i < size; ++i) {
        const auto index = (mOlaWrite + i) % size;
        mOla[c][index] += mWindow[i] * mFftReal[i];
        mNormalization[c][index] += mWindow[i] * mWindow[i];
      }
    }

    mOlaWrite += hop;

    // Every sample before the next write position is complete: no later window reaches it. The
    // first window is normalized by a window sum that is still ramping in, so those samples are
    // discarded as a warmup.
    for (size_t i = 0; i < hop; ++i) {
      const auto index = mOlaExtract % size;
      for (size_t c = 0; c < mChannels; ++c) {
        const auto normalization = mNormalization[c][index];
        const auto value = normalization > 1.0e-8f ? mOla[c][index] / normalization : 0.0f;
        mStretchScratch[i * mChannels + c] = value;
        mOla[c][index] = 0.0f;
        mNormalization[c][index] = 0.0f;
      }
      ++mOlaExtract;
    }

    size_t skip = 0;
    if (mWarmupRemaining > 0) {
      skip = std::min(hop, mWarmupRemaining);
      mWarmupRemaining -= skip;
    }
    if (skip < hop) {
      if (!mStretched.push(mStretchScratch.data() + skip * mChannels, hop - skip)) {
        // The FIFO is sized for several windows; this only happens if the resampler is not
        // consuming, in which case dropping the oldest tail is better than growing forever.
        mStretched.discard(std::min(mStretched.available(), hop - skip));
        (void) mStretched.push(mStretchScratch.data() + skip * mChannels, hop - skip);
      }
    }

    mInputPosition += analysisHop;
    const auto consumed = static_cast<size_t>(mInputPosition);
    if (consumed > 0) {
      mInput.discard(consumed);
      mInputPosition -= static_cast<double>(consumed);
    }

    ++mFramesProcessed;
  }

  void resample(const double pitchRatio) {
    while (mOutput.space() > 0) {
      const auto index = static_cast<size_t>(mResamplePosition);
      if (index + 1 >= mStretched.available()) {
        break;
      }
      const auto fraction = static_cast<float>(mResamplePosition - static_cast<double>(index));
      for (size_t c = 0; c < mChannels; ++c) {
        const auto a = mStretched.at(index, c);
        const auto b = mStretched.at(index + 1, c);
        mResampleScratch[c] = a + (b - a) * fraction;
      }
      if (!mOutput.push(mResampleScratch.data(), 1)) {
        break;
      }
      mResamplePosition += pitchRatio;
      const auto consumed = static_cast<size_t>(mResamplePosition);
      if (consumed > 0) {
        mStretched.discard(consumed);
        mResamplePosition -= static_cast<double>(consumed);
      }
    }
  }

  std::optional<AudioFormat> mFormat;
  size_t mChannels = 0;
  size_t mFftSize = 0;
  AudioFrameCount mMaxRenderFrames = 0;
  AudioFrameCount mMaxInputFrames = 0;
  bool mPrepared = false;

  //!< The smoothed parameters the render thread is currently using.
  float mCurrentRate = 1.0f;
  float mCurrentPitch = 1.0f;

  //!< The hop currently in use, and the one an overlap change is moving to.
  size_t mActiveHop = 0;
  size_t mPendingHop = 0;

  enum class OverlapTransition : uint8_t {
    kNone,
    kFadeOut,
    kFadeIn,
  };

  OverlapTransition mOverlapTransition = OverlapTransition::kNone;
  float mTransitionGain = 1.0f;
  float mTransitionStep = 0.0f;
  size_t mFadeSamples = 1;

  FloatFifo mInput;
  FloatFifo mStretched;
  FloatFifo mOutput;
  std::vector<float> mInputScratch;
  std::vector<float> mOutputScratch;
  std::vector<float> mStretchScratch;
  std::vector<float> mResampleScratch;

  Fft mFft;
  std::vector<float> mFftReal;
  std::vector<float> mFftImaginary;
  std::vector<float> mWindow;
  std::vector<std::vector<float>> mLastPhase;
  std::vector<std::vector<float>> mSumPhase;
  //!< Per-frame analysis data used by the phase locking.
  std::vector<float> mMagnitude;
  std::vector<float> mPhase;
  std::vector<float> mDeviation;
  std::vector<float> mPeakSum;
  std::vector<float> mNormalSum;
  std::vector<int> mPeaks;
  std::vector<std::vector<float>> mOla;
  std::vector<std::vector<float>> mNormalization;

  size_t mOlaWrite = 0;
  size_t mOlaExtract = 0;
  double mInputPosition = 0.0;
  double mResamplePosition = 0.0;
  size_t mWarmupRemaining = 0;
  size_t mFramesProcessed = 0;
  AudioFramePosition mWrittenFrames = 0;
  bool mReportedFormatMismatch = false;
};

AudioUnitTimePitchImpl::AudioUnitTimePitchImpl() = default;

AudioUnitTimePitchImpl::~AudioUnitTimePitchImpl() = default;

void AudioUnitTimePitchImpl::setPitch(const float newPitch) {
  const auto value = std::isfinite(newPitch) ? std::clamp(newPitch, -2400.0f, 2400.0f) : 0.0f;
  pitch.store(value, std::memory_order_relaxed);
}

void AudioUnitTimePitchImpl::setRate(const float newRate) {
  const auto value = std::isfinite(newRate) ? std::clamp(newRate, kMinRate, kMaxRate) : 1.0f;
  rate.store(value, std::memory_order_relaxed);
}

void AudioUnitTimePitchImpl::setOverlap(const float newOverlap) {
  const auto value =
    std::isfinite(newOverlap) ? std::clamp(newOverlap, kMinOverlap, kMaxOverlap) : kDefaultOverlap;
  overlap.store(value, std::memory_order_relaxed);
}

void AudioUnitTimePitchImpl::prepareBeforeRendering() {
  const auto format = renderFormat();
  if (!format.has_value() || !format->isValid()) {
    MINILOG_ERROR("Failed to prepare: the output bus has no valid format");
    return;
  }
  mImpl = std::make_unique<Impl>();
  mImpl->init(format.value(), std::max<AudioFrameCount>(mMaxRenderFrames, 1));
  mImpl->mActiveHop = mImpl->hopFor(getOverlap());
  mImpl->mPendingHop = mImpl->mActiveHop;
  mImpl->mCurrentRate = std::clamp(getRate(), kMinRate, kMaxRate);
  mImpl->mCurrentPitch = static_cast<float>(
    std::pow(2.0, static_cast<double>(getPitch()) / 1200.0));
  AudioNodeImpl::prepareBeforeRendering();
}

void AudioUnitTimePitchImpl::onReset() {
  if (mImpl != nullptr) {
    mImpl->reset();
    mImpl->mActiveHop = mImpl->hopFor(getOverlap());
    mImpl->mPendingHop = mImpl->mActiveHop;
    mImpl->mCurrentRate = std::clamp(getRate(), kMinRate, kMaxRate);
    mImpl->mCurrentPitch = static_cast<float>(
      std::pow(2.0, static_cast<double>(getPitch()) / 1200.0));
  }
}

AudioFrameCount AudioUnitTimePitchImpl::maxInputFramesForBlock(const AudioFrameCount frames) const {
  const auto fftSize = mImpl != nullptr ? mImpl->mFftSize : size_t{ 2048 };
  const auto total = static_cast<uint64_t>(frames) * static_cast<uint64_t>(kMaxRate) +
                     primingMargin(fftSize);
  return static_cast<AudioFrameCount>(
    std::min<uint64_t>(total, std::numeric_limits<AudioFrameCount>::max()));
}

AudioFrameCount AudioUnitTimePitchImpl::inputFramesForBlock(const AudioFrameCount frames) const {
  if (isBypass()) {
    return frames;
  }
  const auto maxFrames = maxInputFramesForBlock(frames);
  if (mImpl == nullptr || !mImpl->mPrepared) {
    return maxFrames;
  }

  const auto currentRate = std::clamp(getRate(), kMinRate, kMaxRate);
  // While a rate change is smoothing, the render loop may still consume a little more (or less)
  // than the new target, so reserve for the larger of the two.
  const auto reservationRate = std::max(currentRate, mImpl->mCurrentRate);
  const auto consumed = static_cast<int64_t>(
    std::ceil(static_cast<double>(frames) * static_cast<double>(reservationRate)));
  // The render loop wants a window plus everything the block will consume in the FIFO when it
  // starts. After it has consumed the block, the same margin is left for the next one, so the
  // request only has to replace what this block took. Requesting the block on top of the whole
  // target would overfill the FIFO, which a rate of 32.0 exposes as dropped input.
  const auto target = static_cast<int64_t>(mImpl->mFftSize) + consumed +
                      2 * static_cast<int64_t>(mImpl->maxHop()) + 8;
  auto needed = std::max<int64_t>(0, target - static_cast<int64_t>(mImpl->mInput.available()));

  needed = std::clamp<int64_t>(needed, 1, static_cast<int64_t>(maxFrames));
  return static_cast<AudioFrameCount>(needed);
}

void AudioUnitTimePitchImpl::renderBlock(const AudioBufferList* const* inputs,
  const uint32_t numInputs,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& when) {
  if (outputData == nullptr || numFrames == 0) {
    return;
  }
  const auto* input = numInputs > 0 ? inputs[0] : nullptr;
  if (input == nullptr) {
    return;
  }

  if (isBypass()) {
    const auto copied = AudioGraphUtils::copyFloatBufferList(*input, *outputData, numFrames);
    if (copied > 0) {
      if (mImpl != nullptr) {
        mImpl->mWrittenFrames += copied;
        updateLastRenderTime(mImpl->mWrittenFrames, renderSampleRate(when));
      } else {
        updateLastRenderTime(copied, renderSampleRate(when));
      }
    }
    return;
  }

  if (mImpl == nullptr || !mImpl->mPrepared) {
    return;
  }

  const auto graphFormat = renderFormat();
  if (!graphFormat.has_value() || !mImpl->mFormat.has_value() || *graphFormat != *mImpl->mFormat) {
    if (!mImpl->mReportedFormatMismatch) {
      mImpl->mReportedFormatMismatch = true;
      MINILOG_ERROR("The time pitch unit received a format it was not prepared for");
    }
    return;
  }

  const auto inputFrames = std::min<AudioFrameCount>(
    maxInputFramesForBlock(numFrames), mImpl->mMaxInputFrames);
  const auto wanted = AudioGraphUtils::interleaveBufferListToFloat(
    *input, mImpl->mInputScratch.data(), inputFrames);
  if (wanted > 0) {
    const auto pushed = std::min<size_t>(wanted, mImpl->mInput.space());
    if (!mImpl->mInput.push(mImpl->mInputScratch.data(), pushed) || pushed < wanted) {
      MINILOG_ERROR("The time pitch input FIFO is full: {} frames were dropped", wanted - pushed);
    }
  }

  const auto targetRate = std::clamp(getRate(), kMinRate, kMaxRate);
  const auto targetPitch = static_cast<float>(
    std::pow(2.0, static_cast<double>(getPitch()) / 1200.0));
  const auto targetOverlap = std::clamp(getOverlap(), kMinOverlap, kMaxOverlap);
  const auto targetHop = mImpl->hopFor(targetOverlap);

  // A hop change rebuilds the overlap-add grid, so it is faded out and back in instead of
  // being applied under the old windows. The rate and pitch are smoothed per frame.
  if (targetHop != mImpl->mActiveHop) {
    mImpl->beginOverlapChange(targetHop, mImpl->mFormat->getSampleRate());
  } else if (mImpl->mOverlapTransition == Impl::OverlapTransition::kFadeOut) {
    // The client changed its mind before the fade out finished; ramp back up instead of
    // snapping the gain to one.
    mImpl->mOverlapTransition = Impl::OverlapTransition::kFadeIn;
    mImpl->mTransitionStep = 1.0f / static_cast<float>(std::max<size_t>(mImpl->mFadeSamples, 1));
  }

  const auto hop = mImpl->mActiveHop;
  const auto smoothingTime = 0.02;
  const auto sampleRate = mImpl->mFormat->getSampleRate();

  for (auto guard = 0; guard < kMaxFramesPerBlock && mImpl->mOutput.available() < numFrames; ++guard) {
    const auto needed = static_cast<double>(mImpl->mFftSize) + 2.0;
    if (static_cast<double>(mImpl->mInput.available()) < mImpl->mInputPosition + needed) {
      break;
    }

    // The smoother advances once per analysis frame, which spans hop / pitch output samples.
    const auto frameOutputSamples =
      std::max(1.0, static_cast<double>(hop) / std::max<double>(mImpl->mCurrentPitch, 0.01));
    const auto smoothing = 1.0 - std::exp(-frameOutputSamples / (smoothingTime * sampleRate));
    mImpl->mCurrentRate += (targetRate - mImpl->mCurrentRate) * static_cast<float>(smoothing);
    mImpl->mCurrentPitch += (targetPitch - mImpl->mCurrentPitch) * static_cast<float>(smoothing);
    mImpl->mCurrentRate = std::clamp(mImpl->mCurrentRate, kMinRate, kMaxRate);
    mImpl->mCurrentPitch = std::clamp(mImpl->mCurrentPitch, 0.25f, 4.0f);

    const auto stretch = static_cast<double>(mImpl->mCurrentPitch) /
                         static_cast<double>(mImpl->mCurrentRate);
    const auto analysisHop = static_cast<double>(hop) / stretch;
    mImpl->processFrame(analysisHop, hop, stretch);
    mImpl->resample(mImpl->mCurrentPitch);
  }
  mImpl->resample(mImpl->mCurrentPitch);

  if (mImpl->mOutput.available() == 0) {
    return;
  }
  const auto got = mImpl->mOutput.pop(mImpl->mOutputScratch.data(),
    std::min<AudioFrameCount>(numFrames, mImpl->mMaxRenderFrames));
  if (got == 0) {
    return;
  }

  if (mImpl->mOverlapTransition != Impl::OverlapTransition::kNone) {
    auto gain = mImpl->mTransitionGain;
    auto fadeOutFinished = false;
    auto fadeInFinished = false;
    for (AudioFrameCount f = 0; f < got; ++f) {
      for (size_t c = 0; c < mImpl->mChannels; ++c) {
        mImpl->mOutputScratch[f * mImpl->mChannels + c] *= gain;
      }
      gain += mImpl->mTransitionStep;
      if (mImpl->mOverlapTransition == Impl::OverlapTransition::kFadeOut && gain <= 0.0f) {
        gain = 0.0f;
        fadeOutFinished = true;
      } else if (mImpl->mOverlapTransition == Impl::OverlapTransition::kFadeIn && gain >= 1.0f) {
        gain = 1.0f;
        fadeInFinished = true;
      }
    }
    mImpl->mTransitionGain = gain;
    if (fadeOutFinished) {
      mImpl->finishOverlapChange();
    } else if (fadeInFinished) {
      mImpl->mOverlapTransition = Impl::OverlapTransition::kNone;
      mImpl->mTransitionGain = 1.0f;
      mImpl->mTransitionStep = 0.0f;
    }
  }
  if (AudioGraphUtils::interleavedFloatIntoBufferList(mImpl->mOutputScratch.data(), *outputData, got) == 0) {
    AudioGraphUtils::silenceFloatBufferList(*outputData, numFrames);
    return;
  }
  mImpl->mWrittenFrames += got;
  updateLastRenderTime(mImpl->mWrittenFrames, renderSampleRate(when));
}

} // namespace coral


