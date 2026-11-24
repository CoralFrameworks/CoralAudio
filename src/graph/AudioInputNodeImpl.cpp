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

#include "private/graph/AudioInputNodeImpl.h"

#include "private/audio/AudioStreamBuilder.h"
#include "private/graph/AudioEngineImpl.h"
#include "private/graph/AudioGraphUtils.h"

#include <coral/audio/AudioTime.h>
#include <coral/audio/AudioUtils.h>

#include <minilog/log.h>

#include <algorithm>
#include <utility>

namespace coral {

namespace {

class AudioGraphInputNodeCallback : public AudioStreamDataCallback {
public:
  explicit AudioGraphInputNodeCallback(AudioInputNodeImpl& node) noexcept
    : mNode(node) {
  }

  DataCallbackResult onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) override {
    return mNode.onAudioReady(stream, audioData, numFrames);
  }

private:
  AudioInputNodeImpl& mNode;
};

} // namespace anonymous

AudioInputNodeImpl::AudioInputNodeImpl() {
  mAudioFormat = AudioFormat::DefaultFormat();
  mGraphFormat = AudioGraphUtils::standardFormat(mAudioFormat.value());
}

AudioInputNodeImpl::~AudioInputNodeImpl() {
  mAudioFormat.reset();
  mGraphFormat.reset();
  mInputBlock = nullptr;
  mAudioStream.reset();
}

std::optional<AudioFormat>
AudioInputNodeImpl::inputFormatForBus(const AudioNodeBus bus) const {
  if (bus == 0) {
    // The input scope is the format the node captures in: the hardware format when the
    // engine renders to a device, the manual rendering format otherwise.
    return mAudioFormat;
  }
  return AudioNodeImpl::inputFormatForBus(bus);
}

std::optional<AudioFormat>
AudioInputNodeImpl::outputFormatForBus(const AudioNodeBus bus) const {
  if (bus == 0) {
    // The graph always receives standard deinterleaved float, whatever the device
    // handed the capture callback.
    return mGraphFormat;
  }
  return AudioNodeImpl::outputFormatForBus(bus);
}

std::optional<AudioFormat> AudioInputNodeImpl::renderFormat() const {
  return mGraphFormat;
}

void AudioInputNodeImpl::setManualRenderingFormat(const std::optional<AudioFormat>& format) {
  if (format == std::nullopt || !format->isValid()) {
    return;
  }
  mAudioFormat = format;
  if (const auto graphFormat = AudioGraphUtils::standardFormat(format.value()); graphFormat.has_value()) {
    mGraphFormat = graphFormat.value();
  }
}

void AudioInputNodeImpl::setManualRenderingInputPCMFormat(std::optional<AudioFormat> format, InputBlock block) {
  setManualRenderingFormat(format);
  mInputBlock = std::move(block);
}

void AudioInputNodeImpl::prepareBeforeRendering() {
  const auto engine = getEngine();
  if (engine == nullptr) {
    MINILOG_ERROR("Failed to prepare: The audio node is not attached to the AudioEngine.");
    return;
  }

  mManualRendering.store(engine->isInManualRenderingMode(), std::memory_order_relaxed);

  if (isManualRendering()) {
    // In manual rendering mode the node never opens a capture stream: the client's
    // input block supplies the samples instead.
    if (mAudioStream != nullptr) {
      mAudioStream->requestStop();
      mAudioStream.reset();
    }
    if (mAudioFormat == std::nullopt || !mAudioFormat->isValid()) {
      if (const auto format = engine->manualRenderingFormat(); format.has_value()) {
        setManualRenderingFormat(format);
      }
    }
    if (mAudioFormat == std::nullopt || !mAudioFormat->isValid()) {
      mAudioFormat = AudioFormat::DefaultFormat();
      mGraphFormat = AudioGraphUtils::standardFormat(mAudioFormat.value());
    }
    mRingBuffer.reset();
    mInterleavedBuffer.clear();
    mFramesWritten = 0;
    AudioNodeImpl::prepareBeforeRendering();
    return;
  }

  if (mAudioFormat == std::nullopt || !mAudioFormat->isValid()) {
    mAudioFormat = AudioFormat::DefaultFormat();
  }

  // A prepare always releases the stream of the previous prepare.
  mAudioStream.reset();

  auto builder = AudioStreamBuilder();
  const auto result =
    builder.setDirection(Direction::Capture)
           .setChannelCount(mAudioFormat->getChannelCount())
           .setSampleRate(static_cast<uint64_t>(mAudioFormat->getSampleRate()))
           .setFormat(mAudioFormat->getSampleFormat())
           .setDataCallback(std::make_shared<AudioGraphInputNodeCallback>(*this))
           .openStream(mAudioStream);
  if (result != Result::OK || mAudioStream == nullptr) {
    mAudioStream.reset();
    MINILOG_ERROR("Failed to create audio stream: {}", convertToText(result));
    return;
  }

  // Adopt the format the platform actually opened the stream with.
  mAudioFormat = AudioGraphUtils::interleavedFormatForStream(*mAudioStream);
  if (const auto graphFormat = AudioGraphUtils::standardFormat(mAudioFormat.value()); graphFormat.has_value()) {
    mGraphFormat = graphFormat.value();
  }

  const auto capacityFrames = std::max(mAudioStream->getFramesPerDataCallback(), kDefaultNumberFrames);
  mMaxRenderFrames = std::max(mMaxRenderFrames, capacityFrames);
  mRingBuffer.init(static_cast<size_t>(capacityFrames) * kMaxNumberPacket, mAudioFormat->getChannelCount());
  mInterleavedBuffer.assign(static_cast<size_t>(mMaxRenderFrames) * mAudioFormat->getChannelCount(), 0.0f);
  mFramesWritten = 0;

  AudioNodeImpl::prepareBeforeRendering();
}

bool AudioInputNodeImpl::start() {
  if (getEngine() == nullptr) {
    MINILOG_ERROR("Failed to start: The input node is not attached to the AudioEngine.");
    return false;
  }
  if (!hasOutputConnection() && !hasTapOnBus(0)) {
    MINILOG_ERROR("Failed to start: The input node is not connected to any output AudioNode and has no tap.");
    return false;
  }

  // Starting an OpenSL ES stream calls the data callback before it returns, to prime the buffer
  // queue, and the callback decides whether the stream keeps running from the running flag. The
  // node therefore counts as running before the stream is asked to start, and goes back to
  // stopped if the stream could not be started.
  mRunning.store(true, std::memory_order_relaxed);

  // In manual rendering mode there is no capture stream to start: the engine's render
  // call pulls the input block instead.
  if (!isManualRendering()) {
    if (mAudioStream == nullptr) {
      mRunning.store(false, std::memory_order_relaxed);
      MINILOG_ERROR("Failed to start: The AudioStream is not prepared.");
      return false;
    }
    if (const auto result = mAudioStream->start(); result != Result::OK) {
      mRunning.store(false, std::memory_order_relaxed);
      MINILOG_ERROR("Failed to start the capture stream: {}", convertToText(result));
      return false;
    }
  }
  return true;
}

void AudioInputNodeImpl::pause() {
  const auto wasRunning = mRunning.exchange(false, std::memory_order_relaxed);
  if (mAudioStream != nullptr && wasRunning) {
    mAudioStream->requestPause();
  }
}

void AudioInputNodeImpl::stop() {
  const auto wasRunning = mRunning.exchange(false, std::memory_order_relaxed);
  if (mAudioStream != nullptr && wasRunning) {
    mAudioStream->requestStop();
  }
}

void AudioInputNodeImpl::onReset() {
  mRunning.store(false, std::memory_order_relaxed);
  if (mAudioStream != nullptr) {
    mAudioStream->flush();
    mAudioStream->close();
    mAudioStream.reset();
  }
  mRingBuffer.reset();
  mInterleavedBuffer.clear();
  mFramesWritten = 0;
}

void AudioInputNodeImpl::renderBlock(const AudioBufferList* const* /* inputs */,
  const uint32_t /* numInputs */,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& /* when */) {
  // An input node has no input buses: its frames come from the capture stream, or from
  // the client's input block while the engine renders manually. The render buffer is silent
  // and holds `numFrames` valid frames before this call, so a capture stream that has not
  // delivered enough frames yet leaves the rest of the block silent, which is what keeps the
  // graph running at a stable block size.
  if (outputData == nullptr || numFrames == 0) {
    return;
  }
  const auto graphFormat = mGraphFormat;
  if (!graphFormat.has_value() || !graphFormat->isValid()) {
    return;
  }

  if (isManualRendering()) {
    if (!mInputBlock) {
      return;
    }
    const auto* list = mInputBlock(numFrames);
    if (list == nullptr) {
      return;
    }
    const auto& format = mAudioFormat.has_value() ? mAudioFormat.value() : graphFormat.value();
    const auto converted = AudioGraphUtils::convertBufferListIntoFloatBufferList(*list, format, *outputData, numFrames);
    if (converted > 0) {
      mFramesWritten += converted;
      updateLastRenderTime(mFramesWritten, format.getSampleRate());
    }
    return;
  }

  const auto available = static_cast<AudioFrameCount>(std::min<size_t>(mRingBuffer.available(), numFrames));
  if (available > 0) {
    const auto channels = mRingBuffer.channels();
    if (mInterleavedBuffer.size() < static_cast<size_t>(numFrames) * channels) {
      mInterleavedBuffer.resize(static_cast<size_t>(numFrames) * channels, 0.0f);
    }
    const auto read = static_cast<AudioFrameCount>(mRingBuffer.read(mInterleavedBuffer.data(), available));
    AudioGraphUtils::interleavedFloatIntoBufferList(mInterleavedBuffer.data(), *outputData, read);
    mFramesWritten += read;
    updateLastRenderTime(mFramesWritten, graphFormat->getSampleRate());
  }
}

DataCallbackResult
AudioInputNodeImpl::onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) {
  if (audioData == nullptr || numFrames == 0 || mAudioFormat == std::nullopt) {
    return DataCallbackResult::Stop;
  }

  // When nothing renders this node, the capture callback is the only clock there is, so
  // the taps are served straight from the recorded frames.
  if (!hasOutputConnection()) {
    if (!hasTapOnBus(0) || mOutputBuffer == nullptr) {
      MINILOG_ERROR("Failed to connect to any output AudioNode or no tap installed.");
      return DataCallbackResult::Stop;
    }
    const auto frames = AudioGraphUtils::convertIntoPCMBuffer(audioData, mAudioFormat.value(), *mOutputBuffer, numFrames);
    if (frames > 0) {
      mOutputBuffer->setFrameLength(frames);
      mFramesWritten += frames;
      updateLastRenderTime(mFramesWritten, mAudioFormat->getSampleRate());
      dispatchTaps(*mOutputBuffer, frames);
    }
    return isRunning() ? DataCallbackResult::Continue : DataCallbackResult::Stop;
  }

  // The graph pulls this node, so the recorded frames are cached for the next render
  // pass. If the cache is too full to take the block, the oldest frames are dropped to
  // keep the capture path lock free.
  if (const auto space = mRingBuffer.space(); space < numFrames) {
    mRingBuffer.discard(numFrames - space);
  }

  auto written = false;
  switch (mAudioFormat->getSampleFormat()) {
    case AudioSampleFormat::kFloat:
      written = mRingBuffer.write(static_cast<const float*>(audioData), numFrames);
      break;
    case AudioSampleFormat::kInt16:
      written = mRingBuffer.writeInt16(static_cast<const int16_t*>(audioData), numFrames);
      break;
    case AudioSampleFormat::kInt24:
      written = mRingBuffer.writeInt24(static_cast<const uint8_t*>(audioData), numFrames);
      break;
    case AudioSampleFormat::kInt32:
      written = mRingBuffer.writeInt32(static_cast<const int32_t*>(audioData), numFrames);
      break;
    default:
      break;
  }
  if (written) {
    // The graph cycle this callback drives starts at the frames that were already captured,
    // and it advances by the block the stream just delivered.
    const auto when = AudioTime(mFramesWritten, mAudioFormat->getSampleRate(), 0);
    mFramesWritten += numFrames;
    updateLastRenderTime(mFramesWritten, mAudioFormat->getSampleRate());

    // Without an output node there is no device render root, so this callback is the only
    // place the input chains that end in a sink node can be pulled from.
    if (const auto engine = getEngine(); engine != nullptr && !engine->hasOutputNode()) {
      engine->renderSinkChains(++mRenderEpoch, numFrames, when);
    }
  }

  return isRunning() ? DataCallbackResult::Continue : DataCallbackResult::Stop;
}

} // namespace coral
