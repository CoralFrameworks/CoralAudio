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

#include "private/graph/AudioSourceNodeImpl.h"

#include "private/graph/AudioGraphUtils.h"

#include <minilog/log.h>

#include <algorithm>
#include <utility>

namespace coral {

AudioSourceNodeImpl::AudioSourceNodeImpl(const std::optional<AudioFormat>& format, RenderBlock block) noexcept
  : mRenderBlock(std::move(block)),
    mRequestedBlockFormat(format),
    mBufferList(std::make_unique<AudioBufferList>()) {
}

AudioSourceNodeImpl::~AudioSourceNodeImpl() {
  mRenderBlock = nullptr;
}

void AudioSourceNodeImpl::onReset() {
  // The render block stays installed: a reset clears the node's timeline and the converter's
  // state, not the client's callback.
  mWrittenFrames = 0;
  mSuppliedPointers.clear();
  mReportedBlockError = false;
  mBlockBuffer.reset();
  mConvertedBuffer.reset();
  mConverter.reset();
  mFormatsMatch = false;
}

void AudioSourceNodeImpl::prepareBeforeRendering() {
  const auto outputFormat = renderFormat();
  if (!outputFormat.has_value() || !outputFormat->isValid()) {
    MINILOG_ERROR("Failed to prepare: the output bus has no valid format");
    return;
  }
  mOutputFormat = outputFormat;

  // The documentation promises that an unnamed block format follows the output format, and
  // that a reconnect with a different output format changes the block format too.
  const auto blockFormat =
    mRequestedBlockFormat.has_value() && mRequestedBlockFormat->isValid() ? mRequestedBlockFormat : outputFormat;
  if (!blockFormat.has_value() || !blockFormat->isValid()) {
    MINILOG_ERROR("Failed to prepare: the format of the render block is invalid");
    return;
  }
  mBlockFormat = blockFormat;

  const auto capacity = std::max(mMaxRenderFrames, kDefaultNumberFrames);
  mFormatsMatch = blockFormat.value() == outputFormat.value();
  mConverter.reset();
  mConvertedBuffer.reset();

  if (mFormatsMatch) {
    mBlockBuffer = AudioPCMBuffer::InitWithPCMFormat(blockFormat.value(), capacity);
  } else {
    // The block runs in its own format, so it fills a scratch buffer that is then converted.
    mBlockBuffer = AudioPCMBuffer::InitWithPCMFormat(blockFormat.value(), capacity);
    mConverter = AudioConverter::InitFromFormat(blockFormat.value(), outputFormat.value());
    if (mConverter != nullptr) {
      // A block with more channels than the graph is mixed down rather than truncated.
      mConverter->setDownMix(true);
    } else {
      MINILOG_ERROR("Failed to prepare: the render block's format cannot be converted to the output format");
    }
    const auto ratio = outputFormat->getSampleRate() / std::max<Float64>(blockFormat->getSampleRate(), 1.0);
    const auto convertedCapacity = static_cast<AudioFrameCount>(
      static_cast<Float64>(capacity) * (std::max<Float64>(ratio, 1.0) + 1.0));
    mConvertedBuffer = AudioPCMBuffer::InitWithPCMFormat(outputFormat.value(), convertedCapacity);
  }

  if (mBlockBuffer == nullptr || (!mFormatsMatch && mConvertedBuffer == nullptr)) {
    MINILOG_ERROR("Failed to prepare: the render block's buffers could not be allocated");
  }

  mWrittenFrames = 0;
  mReportedBlockError = false;
  AudioNodeImpl::prepareBeforeRendering();
}

AudioFrameCount AudioSourceNodeImpl::describeBlockOutput(AudioBufferList& target, const AudioFrameCount frames) {
  const auto format = mBlockFormat;
  if (!format.has_value() || !format->isValid() || frames == 0) {
    return 0;
  }
  const auto channels = format->getChannelCount();
  const auto bytesPerSample = format->getBitsPerChannel() / 8;
  if (channels == 0 || bytesPerSample == 0 || target.numberOfBuffers() == 0) {
    return 0;
  }

  mSuppliedPointers.clear();
  if (format->isInterleaved()) {
    const auto* buffer = target.getBuffer(0);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    mBufferList->resize(1);
    auto* entry = mBufferList->getBuffer(0);
    entry->mNumberChannels = channels;
    entry->mData = buffer->mData;
    entry->mDataByteSize = frames * bytesPerSample * channels;
    mSuppliedPointers.push_back(entry->mData);
    return frames;
  }

  mBufferList->resize(channels);
  for (uint32_t c = 0; c < channels; ++c) {
    const auto* buffer = target.getBuffer(c);
    if (buffer == nullptr || buffer->mData == nullptr) {
      return 0;
    }
    auto* entry = mBufferList->getBuffer(c);
    entry->mNumberChannels = 1;
    entry->mData = buffer->mData;
    entry->mDataByteSize = frames * bytesPerSample;
    mSuppliedPointers.push_back(entry->mData);
  }
  return frames;
}

bool AudioSourceNodeImpl::didBlockReplaceBuffers() const {
  if (mSuppliedPointers.size() != mBufferList->numberOfBuffers()) {
    return true;
  }
  for (size_t i = 0; i < mSuppliedPointers.size(); ++i) {
    const auto* buffer = mBufferList->getBuffer(i);
    if (buffer == nullptr || buffer->mData != mSuppliedPointers[i]) {
      return true;
    }
  }
  return false;
}

AudioFrameCount AudioSourceNodeImpl::moveBlockOutputInto(AudioBufferList& outputData, const AudioFrameCount frames) {
  const auto blockFormat = mBlockFormat;
  if (!blockFormat.has_value()) {
    return 0;
  }
  const auto bytesPerSample = blockFormat->getBitsPerChannel() / 8;
  if (bytesPerSample == 0) {
    return 0;
  }

  // The block reports how much it produced through the byte size of the buffers it left
  // behind, which the documentation requires to be consistent with the frame count.
  auto produced = frames;
  for (size_t i = 0; i < mBufferList->numberOfBuffers(); ++i) {
    const auto* buffer = mBufferList->getBuffer(i);
    if (buffer == nullptr) {
      return 0;
    }
    produced = std::min(produced,
      static_cast<AudioFrameCount>(buffer->mDataByteSize / (buffer->mNumberChannels * bytesPerSample)));
  }
  if (produced == 0) {
    return 0;
  }

  if (mFormatsMatch) {
    // The block wrote straight into the render buffer unless it replaced the pointers.
    if (!didBlockReplaceBuffers()) {
      return produced;
    }
    return AudioGraphUtils::copyFloatBufferList(*mBufferList, outputData, produced);
  }

  if (mConverter == nullptr || mBlockBuffer == nullptr || mConvertedBuffer == nullptr) {
    return 0;
  }
  if (didBlockReplaceBuffers()) {
    // The block supplied its own memory, so the frames are moved into the scratch buffer
    // before the conversion.
    if (AudioGraphUtils::copyBufferListIntoPCMBuffer(*mBufferList, blockFormat.value(), *mBlockBuffer, produced) == 0) {
      return 0;
    }
  }
  mBlockBuffer->setFrameLength(produced);

  if (const auto error = mConverter->convertToBuffer(mBlockBuffer, mConvertedBuffer); error.has_value()) {
    if (!mReportedBlockError) {
      mReportedBlockError = true;
      MINILOG_ERROR("Failed to convert the audio of the source node: {}", error->toString());
    }
    return 0;
  }

  const auto converted = mConvertedBuffer->getFrameLength();
  if (converted == 0) {
    return 0;
  }
  return AudioGraphUtils::copyFloatBufferList(*mConvertedBuffer->audioBufferList(), outputData, converted);
}

void AudioSourceNodeImpl::renderBlock(const AudioBufferList* const* /* inputs */,
  const uint32_t /* numInputs */,
  AudioBufferList* outputData,
  const AudioFrameCount numFrames,
  const AudioTime& when) {
  // A source node has no inputs: it generates its own frames. The render buffer is silent and
  // holds `numFrames` valid frames before this call, so whatever the block does not supply is
  // silence, which is also what a missing or failed block leaves behind.
  if (outputData == nullptr || numFrames == 0) {
    return;
  }
  if (!mRenderBlock || mBlockBuffer == nullptr || !mBlockFormat.has_value()) {
    return;
  }

  // When the formats match the block writes straight into the render buffer, so the common
  // case costs no copy at all. Otherwise it fills the node's scratch buffer, which is sized
  // for the largest block the engine announced.
  auto* target = mFormatsMatch ? outputData : mBlockBuffer->mutableAudioBufferList();
  const auto frames = mFormatsMatch ? numFrames : std::min(numFrames, mBlockBuffer->getFrameCapacity());

  const auto requested = describeBlockOutput(*target, frames);
  if (requested == 0) {
    return;
  }

  // The block is told the time of the frames it is asked for. The render cycle supplies it;
  // a cycle without a usable rate falls back to the node's own render format.
  std::optional<AudioTime> timestamp = when;
  if (!when.isSampleTimeValid()) {
    timestamp = AudioTime(mWrittenFrames, renderSampleRate(when), 0);
  }
  const auto sampleRate = renderSampleRate(when);

  bool isSilence = false;
  const auto result = mRenderBlock(&isSilence, timestamp, requested, mBufferList);
  if (result != Result::OK) {
    if (!mReportedBlockError) {
      mReportedBlockError = true;
      MINILOG_ERROR("Failed to call the render block of the source node: {}", static_cast<int32_t>(result));
    }
    // A failed block invalidates the data it supplied, so the timeline still advances: the
    // graph asked for these frames and got silence.
    mWrittenFrames += requested;
    updateLastRenderTime(mWrittenFrames, sampleRate);
    return;
  }

  if (!isSilence) {
    (void) moveBlockOutputInto(*outputData, requested);
  }

  mWrittenFrames += requested;
  updateLastRenderTime(mWrittenFrames, sampleRate);
}

} // namespace coral
