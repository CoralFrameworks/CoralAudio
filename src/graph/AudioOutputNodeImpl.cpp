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

#include "private/graph/AudioOutputNodeImpl.h"

#include "private/audio/AudioStreamBuilder.h"
#include "private/graph/AudioEngineImpl.h"
#include "private/graph/AudioGraphUtils.h"

#include <coral/audio/AudioUtils.h>

#include <minilog/log.h>

#include <algorithm>

namespace coral {

namespace {

class AudioGraphOutputNodeCallback : public AudioStreamDataCallback {
public:
  explicit AudioGraphOutputNodeCallback(AudioOutputNodeImpl& node) noexcept
    : mNode(node) {
  }

  DataCallbackResult onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) override {
    return mNode.onAudioReady(stream, audioData, numFrames);
  }

private:
  AudioOutputNodeImpl& mNode;
};

} // namespace anonymous

AudioOutputNodeImpl::AudioOutputNodeImpl()
  : mAudioFormat(AudioFormat::DefaultFormat()),
    mGraphFormat(AudioGraphUtils::standardFormat(AudioFormat::DefaultFormat())) {
}

AudioOutputNodeImpl::~AudioOutputNodeImpl() {
  mAudioStream.reset();
  mAudioFormat.reset();
  mGraphFormat.reset();
  mRingBuffer.reset();
}

void AudioOutputNodeImpl::setOutputFormat(const AudioFormat& format) {
  if (!format.isValid()) {
    MINILOG_ERROR("Failed to set the output format: the format is invalid");
    return;
  }
  mAudioFormat = format;
  if (const auto graphFormat = AudioGraphUtils::standardFormat(format); graphFormat.has_value()) {
    mGraphFormat = graphFormat.value();
  }
}

std::optional<AudioFormat>
AudioOutputNodeImpl::inputFormatForBus(const AudioNodeBus bus) const {
  if (bus == 0) {
    // The graph feeds this node standard deinterleaved float, whatever the device wants.
    if (const auto format = AudioNodeImpl::inputFormatForBus(bus); format.has_value()) {
      return format;
    }
    return mGraphFormat;
  }
  return AudioNodeImpl::inputFormatForBus(bus);
}

std::optional<AudioFormat>
AudioOutputNodeImpl::outputFormatForBus(const AudioNodeBus bus) const {
  if (bus == 0) {
    // The output scope is what the device (or the manual rendering mode) consumes.
    return mAudioFormat;
  }
  return AudioNodeImpl::outputFormatForBus(bus);
}

std::optional<AudioFormat> AudioOutputNodeImpl::renderFormat() const {
  return mGraphFormat;
}

void AudioOutputNodeImpl::prepareBeforeRendering() {
  const auto engine = getEngine();
  if (engine == nullptr) {
    MINILOG_ERROR("Failed to prepare: The audio node is not attached to the AudioEngine.");
    return;
  }

  mManualRendering.store(engine->isInManualRenderingMode(), std::memory_order_relaxed);

  if (isManualRendering()) {
    // In manual rendering mode the node never opens a render stream: the engine pulls it
    // when the client asks for audio.
    if (mAudioStream != nullptr) {
      mAudioStream->requestStop();
      mAudioStream.reset();
    }
    if (mAudioFormat == std::nullopt || !mAudioFormat->isValid()) {
      if (const auto format = engine->manualRenderingFormat(); format.has_value()) {
        setOutputFormat(format.value());
      }
    }
    if (mAudioFormat == std::nullopt || !mAudioFormat->isValid()) {
      mAudioFormat = inputFormatForBus(0).value_or(AudioFormat::DefaultFormat());
      if (const auto graphFormat = AudioGraphUtils::standardFormat(mAudioFormat.value()); graphFormat.has_value()) {
        mGraphFormat = graphFormat.value();
      }
    }
    mRingBuffer.reset();
    mInterleavedBuffer.clear();
    mFramesWritten = 0;
    mRenderPosition = 0;
    AudioNodeImpl::prepareBeforeRendering();
    return;
  }

  // When rendering to a device, the hardware format is what the platform gives the
  // stream, so the node asks for the graph's rate and channel count and then adopts
  // whatever the stream reports.
  if (mAudioFormat == std::nullopt || !mAudioFormat->isValid()) {
    mAudioFormat = inputFormatForBus(0).value_or(AudioFormat::DefaultFormat());
  }

  auto sampleFormat = mAudioFormat->getSampleFormat();
  if (sampleFormat == AudioSampleFormat::kInvalid || sampleFormat == AudioSampleFormat::kDefault) {
    sampleFormat = AudioSampleFormat::kFloat;
  }

  // A prepare always releases the stream of the previous prepare.
  mAudioStream.reset();

  auto builder = AudioStreamBuilder();
  const auto result =
    builder.setDirection(Direction::Render)
           .setChannelCount(mAudioFormat->getChannelCount())
           .setSampleRate(static_cast<uint64_t>(mAudioFormat->getSampleRate()))
           .setFormat(sampleFormat)
           .setDataCallback(std::make_shared<AudioGraphOutputNodeCallback>(*this))
           .openStream(mAudioStream);
  if (result != Result::OK || mAudioStream == nullptr) {
    mAudioStream.reset();
    MINILOG_ERROR("Failed to create AudioStream: {}", convertToText(result));
    return;
  }

  mAudioFormat = AudioGraphUtils::interleavedFormatForStream(*mAudioStream);
  if (const auto graphFormat = AudioGraphUtils::standardFormat(mAudioFormat.value()); graphFormat.has_value()) {
    mGraphFormat = graphFormat.value();
  }

  const auto capacityFrames = std::max(mAudioStream->getBufferCapacityInFrames(), kDefaultNumberFrames);
  mMaxRenderFrames = std::max(mMaxRenderFrames, capacityFrames);
  mRingBuffer.init(static_cast<size_t>(capacityFrames) * kMaxNumberPacket, mAudioFormat->getChannelCount());
  mInterleavedBuffer.assign(static_cast<size_t>(mMaxRenderFrames) * mAudioFormat->getChannelCount(), 0.0f);
  mFramesWritten = 0;
  mRenderPosition = 0;

  AudioNodeImpl::prepareBeforeRendering();
}

bool AudioOutputNodeImpl::start() {
  if (getEngine() == nullptr) {
    MINILOG_ERROR("Failed to start: The output node is not attached to the AudioEngine.");
    return false;
  }
  if (numberOfInputs() != 1) {
    MINILOG_ERROR("Failed to start: The output node is not connected to any input AudioNode.");
    return false;
  }
  if (numberOfOutputs() != 0) {
    MINILOG_ERROR("Failed to start: The output node does not support any output connections.");
    return false;
  }
  if (isInputBusAvailable(0)) {
    MINILOG_ERROR("Failed to start: The output node has no connected input bus.");
    return false;
  }

  // In manual rendering mode there is no render stream to start.
  if (isManualRendering()) {
    mRunning.store(true, std::memory_order_relaxed);
    mFramesWritten = 0;
    return true;
  }

  if (mAudioStream == nullptr) {
    MINILOG_ERROR("Failed to start: The AudioStream is not prepared.");
    return false;
  }

  const auto sampleRate = mAudioStream->getSampleRate();
  if (sampleRate > 0) {
    auto maxFrames = static_cast<uint32_t>(kDefaultCacheDuration) * static_cast<uint32_t>(sampleRate);
    maxFrames = std::max(maxFrames, mAudioStream->getBufferCapacityInFrames());
    mPresentationLatency.store(
      static_cast<int64_t>(mAudioStream->getBufferCapacityInFrames() / sampleRate), std::memory_order_relaxed);
    mLatency = maxFrames / sampleRate;
  }

  // Starting an OpenSL ES stream calls the data callback before it returns, to prime the buffer
  // queue, and the callback decides whether the stream keeps running from the running flag. The
  // node therefore counts as running before the stream is asked to start, and goes back to
  // stopped if the stream could not be started.
  mRunning.store(true, std::memory_order_relaxed);
  if (const auto result = mAudioStream->start(); result != Result::OK) {
    mRunning.store(false, std::memory_order_relaxed);
    MINILOG_ERROR("Failed to start the render stream: {}", convertToText(result));
    return false;
  }

  // The latency is asked for once the stream runs: a backend that reports it from the stream's
  // own timing (AAudio does) cannot answer before the stream was started.
  if (sampleRate > 0) {
    if (const auto latencyResult = mAudioStream->calculateLatencyMillis(); !latencyResult) {
      if (latencyResult.error() == Result::ErrorUnimplemented) {
        // A backend that cannot report a latency is not a failure: the presentation latency stays
        // at zero and the rest of the node works as usual.
        MINILOG_DEBUG("The render backend does not report an output latency");
      } else {
        MINILOG_ERROR("Failed to calculate the output latency of the AudioStream");
      }
    } else {
      mOutputPresentationLatency = static_cast<uint64_t>(latencyResult.value() / Definitions::kMillisPerSecond);
    }
  }

  mFramesWritten = 0;
  return true;
}

void AudioOutputNodeImpl::pause() {
  const auto wasRunning = mRunning.exchange(false, std::memory_order_relaxed);
  if (mAudioStream != nullptr && wasRunning) {
    mAudioStream->requestPause();
  }
}

void AudioOutputNodeImpl::stop() {
  const auto wasRunning = mRunning.exchange(false, std::memory_order_relaxed);
  if (mAudioStream != nullptr && wasRunning) {
    mAudioStream->requestStop();
  }
}

void AudioOutputNodeImpl::onReset() {
  mRunning.store(false, std::memory_order_relaxed);
  if (mAudioStream != nullptr) {
    mAudioStream->flush();
    mAudioStream->close();
    mAudioStream.reset();
  }
  mRingBuffer.reset();
  mInterleavedBuffer.clear();
  mFramesWritten = 0;
  mRenderPosition = 0;
}

void AudioOutputNodeImpl::writeToRingBuffer(const AudioBufferList& source, const AudioFrameCount numFrames) {
  if (mAudioFormat == std::nullopt || mRingBuffer.capacity() == 0 || source.numberOfBuffers() == 0) {
    return;
  }

  const auto channels = static_cast<uint32_t>(source.numberOfBuffers());
  if (mInterleavedBuffer.size() < static_cast<size_t>(numFrames) * channels) {
    mInterleavedBuffer.resize(static_cast<size_t>(numFrames) * channels, 0.0f);
  }
  const auto interleaved = AudioGraphUtils::interleaveBufferListToFloat(source, mInterleavedBuffer.data(), numFrames);
  if (interleaved == 0) {
    return;
  }
  // The render pass and the callback that drains the buffer are the same thread when
  // rendering to a device, so the buffer only has to survive until the end of the block.
  if (const auto space = mRingBuffer.space(); space < interleaved) {
    mRingBuffer.discard(interleaved - space);
  }
  mRingBuffer.write(mInterleavedBuffer.data(), interleaved);
}

void AudioOutputNodeImpl::renderBlock(const AudioBufferList* const* inputs,
  const uint32_t numInputs,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& /* when */) {
  // The render buffer is in the format of the graph and is silent before this call, so a
  // block with no usable input renders as silence.
  if (outputData == nullptr || numFrames == 0) {
    return;
  }

  const auto* input = numInputs > 0 ? inputs[0] : nullptr;
  if (input != nullptr) {
    (void) AudioGraphUtils::copyFloatBufferList(*input, *outputData, numFrames);
  }

  // The manual rendering path reads the render buffer directly, so only the device path
  // has to stage the frames in the ring buffer, in the device's own format.
  if (!isManualRendering()) {
    writeToRingBuffer(*outputData, numFrames);
  }
}

DataCallbackResult
AudioOutputNodeImpl::onAudioReady(AudioStream* stream, void* audioData, const AudioFrameCount numFrames) {
  if (audioData == nullptr || numFrames == 0) {
    return DataCallbackResult::Stop;
  }
  if (mAudioFormat == std::nullopt || !mAudioFormat->isValid()) {
    return isRunning() ? DataCallbackResult::Continue : DataCallbackResult::Stop;
  }
  if (mRingBuffer.capacity() == 0) {
    AudioGraphUtils::silenceBuffer(audioData, mAudioFormat.value(), numFrames);
    return isRunning() ? DataCallbackResult::Continue : DataCallbackResult::Stop;
  }

  // The output node is the root of the graph, so the audio driver thread pulls it here. The
  // cycle is timed in the graph's own format, which is the rate the graph renders at.
  const auto sampleRate = mGraphFormat.has_value() ? mGraphFormat->getSampleRate() : 0;
  const auto when = AudioTime(mRenderPosition, sampleRate, 0);
  if (render(++mRenderEpoch, numFrames, when) == nullptr) {
    AudioGraphUtils::silenceBuffer(audioData, mAudioFormat.value(), numFrames);
    return isRunning() ? DataCallbackResult::Continue : DataCallbackResult::Stop;
  }
  mRenderPosition += numFrames;
  {
    static std::atomic_int sRenderPassCount{ 0 };
    if (sRenderPassCount.fetch_add(1, std::memory_order_relaxed) < 5) {
      MINILOG_DEBUG("The render pass of the output node runs: {} frames, the ring holds {}",
        numFrames, mRingBuffer.available());
    }
  }

  // Input chains that end in a sink node are rendered in this same pass and on this same
  // thread, so a client receiver block sees the input audio on the audio driver thread.
  if (const auto engine = getEngine(); engine != nullptr) {
    engine->renderSinkChains(mRenderEpoch, numFrames, when);
  }

  // Drain what the render pass produced, converted into the device's own format.
  const auto toRead = static_cast<AudioFrameCount>(std::min<size_t>(mRingBuffer.available(), numFrames));
  auto framesGot = AudioFrameCount{ 0 };
  switch (mAudioFormat->getSampleFormat()) {
    case AudioSampleFormat::kFloat:
      framesGot = static_cast<AudioFrameCount>(mRingBuffer.read(static_cast<float*>(audioData), toRead));
      break;
    case AudioSampleFormat::kInt16:
      framesGot = static_cast<AudioFrameCount>(mRingBuffer.readInt16(static_cast<int16_t*>(audioData), toRead));
      break;
    case AudioSampleFormat::kInt24:
      framesGot = static_cast<AudioFrameCount>(mRingBuffer.readInt24(static_cast<uint8_t*>(audioData), toRead));
      break;
    case AudioSampleFormat::kInt32:
      framesGot = static_cast<AudioFrameCount>(mRingBuffer.readInt32(static_cast<int32_t*>(audioData), toRead));
      break;
    default:
      break;
  }

  if (framesGot < numFrames) {
    // An underrun leaves the rest of the device buffer silent rather than stale.
    auto* remainder =
      static_cast<uint8_t*>(audioData) + static_cast<size_t>(framesGot) * mAudioFormat->getBytesPerFrame();
    AudioGraphUtils::silenceBuffer(remainder, mAudioFormat.value(), numFrames - framesGot);
  }
  if (framesGot > 0) {
    mFramesWritten += framesGot;
    updateLastRenderTime(mFramesWritten, mAudioFormat->getSampleRate());
  }

  return isRunning() ? DataCallbackResult::Continue : DataCallbackResult::Stop;
}

} // namespace coral
