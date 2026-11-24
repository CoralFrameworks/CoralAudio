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

#include "SampleRateConverter.h"

namespace coral {

SampleRateConverter::SampleRateConverter(const int32_t channelCount, MultiChannelResampler& resampler) noexcept
  : FlowGraphFilter(channelCount),
    mResampler(resampler) {
  setDataPulledAutomatically(false);
}

int32_t SampleRateConverter::onProcess(const int32_t numFrames) {
  float* outputBuffer = output.getBuffer();
  const auto channelCount = output.getChannelCount();
  int framesLeft = numFrames;
  while (framesLeft > 0) {
    // Gather input samples as needed.
    if (mResampler.isWriteNeeded()) {
      if (isInputAvailable()) {
        const float* frame = getNextInputFrame();
        mResampler.writeNextFrame(frame);
      } else {
        break;
      }
    } else {
      // Output frame is interpolated from input samples.
      mResampler.readNextFrame(outputBuffer);
      outputBuffer += channelCount;
      framesLeft--;
    }
  }
  return numFrames - framesLeft;
}

void SampleRateConverter::reset() {
  FlowGraphNode::reset();
  // Zero, not `kInitialCallCount`.
  //
  // `mInputCursor` is an offset into the input port buffer -- the header says so -- and `kInitialCallCount` is `-1`, a
  // sentinel for `FlowGraphNode::mLastCallCount`, which counts calls. Assigning the one to the other made the first
  // `getNextInputFrame()` compute `&buffer[-1 * channelCount]`, two floats before the buffer for stereo, and shifted
  // every subsequent read by a frame:
  //
  //   [probe] getNextInputFrame: buffer=0x...4300 frame=0x...42F8 offsetInFloats=-2 channelCount=2
  //
  // Nothing noticed, because reading the redzone of a heap block yields plausible audio in a release build. An
  // address-sanitized run of the root tree is what reported it:
  //
  //   READ of size 4 ... PolyphaseResamplerStereo.cpp:39 in PolyphaseResamplerStereo::writeFrame(float const *)
  mInputCursor = 0;
}

bool SampleRateConverter::isInputAvailable() {
  // If we have consumed all of the input data then go out and get some more.
  if (mInputCursor >= mNumValidInputFrames) {
    mInputCallCount++;
    mNumValidInputFrames = input->pullData(mInputCallCount, input->getFramesPerBuffer());
    mInputCursor = 0;
  }
  return (mInputCursor < mNumValidInputFrames);
}

const float* SampleRateConverter::getNextInputFrame() {
  const float* inputBuffer = input->getBuffer();
  const float* frame = &inputBuffer[mInputCursor++ * input->getChannelCount()];
  return frame;
}

} // namespace coral
