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

#include "private/graph/AudioUnitVarispeedImpl.h"

#include "private/graph/AudioGraphUtils.h"

#include <minilog/log.h>

#include <algorithm>
#include <cmath>

namespace coral {

AudioUnitVarispeedImpl::AudioUnitVarispeedImpl() = default;

AudioUnitVarispeedImpl::~AudioUnitVarispeedImpl() {
  if (mSonic != nullptr) {
    sonicDestroyStream(mSonic);
    mSonic = nullptr;
  }
}

void AudioUnitVarispeedImpl::setRate(const float newRate) {
  const auto clamped = std::clamp(newRate, kMinRate, kMaxRate);
  if (rate.load(std::memory_order_relaxed) == clamped) {
    return;
  }
  rate.store(clamped, std::memory_order_relaxed);
  // The stream belongs to the render thread, so the change is only flagged here.
  mRateChanged.store(true, std::memory_order_release);
}

AudioFrameCount AudioUnitVarispeedImpl::inputFramesForBlock(const AudioFrameCount frames) const {
  // A bypassed unit passes its input through untouched, so it consumes exactly the block it
  // renders, whatever the rate says.
  if (isBypass()) {
    return frames;
  }
  // Sonic resamples the input by the rate, so a block of `frames` output frames consumes
  // `frames * rate` input frames. Rounding keeps the two sides in step over time; the ring
  // buffer absorbs the one frame the rounding can add or leave out.
  const auto speed = static_cast<double>(std::clamp(getRate(), kMinRate, kMaxRate));
  const auto wanted = std::llround(static_cast<double>(frames) * speed);
  return static_cast<AudioFrameCount>(std::max<int64_t>(wanted, 1));
}

AudioFrameCount AudioUnitVarispeedImpl::maxInputFramesForBlock(const AudioFrameCount frames) const {
  // The rate can change without a new prepare, so the upstream buffers are sized for the
  // largest rate the API allows.
  return static_cast<AudioFrameCount>(std::ceil(static_cast<double>(frames) * static_cast<double>(kMaxRate)));
}

void AudioUnitVarispeedImpl::onReset() {
  if (mSonic != nullptr) {
    sonicFlushStream(mSonic);
  }
  mRingBuffer.reset();
  mRateChanged.store(false, std::memory_order_relaxed);
}

void AudioUnitVarispeedImpl::prepareBeforeRendering() {
  const auto format = renderFormat();
  if (!format.has_value() || !format->isValid()) {
    MINILOG_ERROR("Failed to prepare: the output bus has no valid format");
    return;
  }

  // The stream and its buffers are created here, while the engine is stopped and this node's
  // render path is not running.
  initWithInputFormat(format.value());
  AudioNodeImpl::prepareBeforeRendering();
}

void AudioUnitVarispeedImpl::applyPendingRate() {
  if (mSonic == nullptr || !mRateChanged.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  sonicSetRate(mSonic, getRate());
}

void AudioUnitVarispeedImpl::initWithInputFormat(const AudioFormat& format) {
  if (!format.isValid()) {
    return;
  }
  const auto channels = std::max(format.getChannelCount(), 1u);
  if (mSonic != nullptr && mSonicFormat.has_value() && *mSonicFormat == format) {
    applyPendingRate();
    return;
  }

  if (mSonic != nullptr) {
    sonicDestroyStream(mSonic);
    mSonic = nullptr;
  }
  mSonic = sonicCreateStream(static_cast<int>(format.getSampleRate()), static_cast<int>(channels));
  if (mSonic == nullptr) {
    MINILOG_ERROR("Failed to create the sonic stream of the varispeed unit");
    mSonicFormat.reset();
    return;
  }

  // Only the rate is used: it resamples the input, so the speed and the pitch change together.
  sonicSetSpeed(mSonic, 1.0f);
  sonicSetPitch(mSonic, 1.0f);
  sonicSetRate(mSonic, getRate());
  sonicFlushStream(mSonic);
  mRateChanged.store(false, std::memory_order_release);

  const auto blockFrames = std::max(mMaxRenderFrames, kDefaultNumberFrames);
  // A quarter of a second of slack absorbs the rounding of the rate, the latency of the sonic
  // stream and the burst a rate change produces, and it is what bounds the extra latency the
  // node introduces.
  const auto ringFrames = std::max<AudioFrameCount>(
    static_cast<AudioFrameCount>(format.getSampleRate() / 4.0), blockFrames * 4);
  mRingBuffer.init(ringFrames, channels);
  // The input side is larger than the output block: a rate above 1.0 pulls more frames than
  // the block it renders on the output side.
  const auto maxInputFrames = std::max(maxInputFramesForBlock(blockFrames), blockFrames);
  mInterleavedInput.assign(static_cast<size_t>(maxInputFrames) * channels, 0.0f);
  mInterleavedOutput.assign(static_cast<size_t>(blockFrames) * channels, 0.0f);

  // Sonic grows its internal buffers with the input it is given, which would allocate on the
  // render thread the first time a block larger than its initial size arrives. A rate above
  // 1.0 makes exactly that happen, so one maximum-sized block of silence is pushed through the
  // stream here, while the engine is stopped, and thrown away.
  if (sonicWriteFloatToStream(mSonic, mInterleavedInput.data(), static_cast<int>(maxInputFrames)) != 0) {
    sonicFlushStream(mSonic);
    for (;;) {
      const auto available = sonicSamplesAvailable(mSonic);
      if (available <= 0) {
        break;
      }
      const auto toRead = std::min<AudioFrameCount>(available, blockFrames);
      if (sonicReadFloatFromStream(mSonic, mInterleavedOutput.data(), static_cast<int>(toRead)) <= 0) {
        break;
      }
    }
  }
  mSonicFormat = format;
}

void AudioUnitVarispeedImpl::drainSonic() {
  const auto channels = mRingBuffer.channels();
  if (mSonic == nullptr || channels == 0 || mInterleavedOutput.size() < channels) {
    return;
  }
  const auto scratchFrames = static_cast<AudioFrameCount>(mInterleavedOutput.size() / channels);

  // Move everything sonic has produced into the ring buffer. A rate of 1.0 produces one frame
  // per input frame, and the input the unit pulls follows the rate, so in steady state this
  // moves about one block per call: the loop is bounded by the ring's own capacity and by
  // `kMaxDrainRounds`, which keeps a normal render block short.
  for (int round = 0; round < kMaxDrainRounds; ++round) {
    auto space = static_cast<AudioFrameCount>(mRingBuffer.space());
    if (space == 0) {
      // Only a rate change or the rounding of a fractional rate can fill the buffer: the
      // oldest frames are dropped so that the node keeps up with its input instead of
      // drifting behind it for ever.
      if (mRingBuffer.discard(std::min(kDrainBlockFrames, static_cast<AudioFrameCount>(mRingBuffer.available()))) ==
          0) {
        break;
      }
      space = static_cast<AudioFrameCount>(mRingBuffer.space());
      if (space == 0) {
        break;
      }
    }

    const auto wanted = std::min(space, scratchFrames);
    if (wanted == 0) {
      break;
    }
    const auto read = sonicReadFloatFromStream(mSonic, mInterleavedOutput.data(), static_cast<int>(wanted));
    if (read <= 0) {
      break;
    }
    if (!mRingBuffer.write(mInterleavedOutput.data(), static_cast<size_t>(read))) {
      break;
    }
  }
}

void AudioUnitVarispeedImpl::renderBlock(const AudioBufferList* const* inputs,
  const uint32_t numInputs,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& when) {
  // The render buffer is silent and holds `numFrames` valid frames before this call, so a
  // block the node cannot produce renders as silence.
  if (outputData == nullptr || numFrames == 0) {
    return;
  }

  const auto* input = numInputs > 0 ? inputs[0] : nullptr;
  if (input == nullptr) {
    return;
  }

  // A bypassed time effect passes its input through untouched.
  if (isBypass()) {
    const auto copied = AudioGraphUtils::copyFloatBufferList(*input, *outputData, numFrames);
    if (copied > 0) {
      const auto position = mWrittenFrames + copied;
      mWrittenFrames = position;
      updateLastRenderTime(position, renderSampleRate(when));
    }
    return;
  }

  if (mSonic == nullptr || !mSonicFormat.has_value()) {
    // The engine prepares the node before it renders, so this only happens when the node was
    // never prepared.
    return;
  }
  // The input of a time effect is the format of its own output bus, which is the format the
  // sonic stream was created for: a different one would need a new stream, which cannot be
  // built on a render thread.
  if (const auto graphFormat = renderFormat();
      !graphFormat.has_value() || *mSonicFormat != *graphFormat) {
    if (!mReportedFormatMismatch) {
      mReportedFormatMismatch = true;
      MINILOG_ERROR("The varispeed unit received a format it was not prepared for");
    }
    return;
  }

  applyPendingRate();

  const auto channels = mRingBuffer.channels();
  if (channels == 0) {
    return;
  }

  // The graph pulled the input with the frame count this node asked for, which is the block
  // scaled by the rate, so read the frames the input bus actually holds. The graph pads a
  // short input with silence, and clamps a request to the capacity of the upstream buffer, so
  // the count comes from the list rather than from the rate.
  const auto requestedInputFrames = std::max(maxInputFramesForBlock(numFrames), numFrames);
  if (mInterleavedInput.size() < static_cast<size_t>(requestedInputFrames) * channels) {
    return;
  }
  const auto inputFrames = AudioGraphUtils::interleaveBufferListToFloat(
    *input, mInterleavedInput.data(), requestedInputFrames);
  if (inputFrames > 0 &&
      sonicWriteFloatToStream(mSonic, mInterleavedInput.data(), static_cast<int>(inputFrames)) == 0) {
    MINILOG_ERROR("Failed to write into the sonic stream of the varispeed unit");
    return;
  }

  drainSonic();

  const auto got = static_cast<AudioFrameCount>(mRingBuffer.read(mInterleavedOutput.data(), numFrames));
  if (got > 0) {
    if (AudioGraphUtils::interleavedFloatIntoBufferList(mInterleavedOutput.data(), *outputData, got) == 0) {
      // A block that could not be written is silent rather than partly written.
      AudioGraphUtils::silenceFloatBufferList(*outputData, numFrames);
      return;
    }
    mWrittenFrames += got;
    updateLastRenderTime(mWrittenFrames, renderSampleRate(when));
  }
}

} // namespace coral
