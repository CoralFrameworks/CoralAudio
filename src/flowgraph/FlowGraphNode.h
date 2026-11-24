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

#ifndef CORAL_AUDIO_FLOW_GRAPH_NODE_H
#define CORAL_AUDIO_FLOW_GRAPH_NODE_H

#include <coral/audio/AudioBase.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coral {

class AudioGraphPort;
class AudioGraphPortFloatInput;

class FlowGraph : public AudioNoncopyable {
protected:
  /**
   * Default block size that can be overridden when the AudioGraphPortFloat is created.
   * If it is too small then we will have too much overhead from switching between nodes.
   * If it is too high then we will thrash the caches.
   */
  static constexpr int kDefaultBufferSize = 8;
};

/**
 * Bass class for all graph nodes to process audio
 */
class FlowGraphNode : public FlowGraph {
public:
  /**
   * Read from the input point, generate multiple frames of data then write the
   * results to the output ports.
   *
   * @param numFrames maximum number of frames requested for processing
   * @return number of frames actually processed
   */
  virtual int32_t onProcess(int32_t numFrames) = 0;

  /**
   * If the callCount is at or after the previous callCount then call
   * pullData on all the upstreamNodes.
   *
   * @param numFrames
   * @param callCount
   * @return number of frames valid
   */
  int32_t pullData(int32_t numFrames, int64_t callCount);

  /**
   * Recursively reset all the nodes in the graph, starting from a Sink.
   *
   * This must not be called at the same time as pullData!
   */
  void pullReset();

  /**
   * Reset framePosition counters.
   */
  virtual void reset();

  /**
   * Only use for the input of the AudioGraphPort
   * @param inputPort the AudioGraphPort
   */
  void addInputPort(AudioGraphPort& inputPort) {
    mInputPorts.emplace_back(inputPort);
  }

  [[nodiscard]] bool isDataPulledAutomatically() const {
    return mDataPulledAutomatically;
  }

  /**
   * Set true if you want the data pulled through the graph automatically.
   * This is the default.
   *
   * Set false if you want to pull the data from the input ports in the
   * onProcess() method. You might do this, for example, in a sample rate
   * converting node.
   *
   * @param automatic
   */
  void setDataPulledAutomatically(const bool automatic) {
    mDataPulledAutomatically = automatic;
  }

  [[nodiscard]] virtual std::string getName() const {
    return "FlowGraphNode";
  }

  [[nodiscard]] int64_t getLastCallCount() const {
    return mLastCallCount;
  }

  /**
   * Returns the pointer of the input port.
   *
   * @return the pointer of the AudioGraphPortFloatInput, or nullptr.
   *
   * When the AudioGraphNode supports only one input port, the bus value is only
   * available when 0, otherwise will return nullptr.
   *
   * The AudioGraphNode only supports one input port by default.
   * The AudioGraphSource has no input port.
   * The AudioGraphMixer supports many-to-one connections, so it has multi input
   * port.
   */
  virtual AudioGraphPortFloatInput* getInput(uint32_t bus) const = 0;

protected:
  static constexpr int64_t kInitialCallCount = -1;
  int64_t mLastCallCount = kInitialCallCount;

  std::vector<std::reference_wrapper<AudioGraphPort>> mInputPorts;

private:
  bool mDataPulledAutomatically = true;
  bool mBlockRecursion = false;
  int32_t mLastFrameCount = 0;
};

//--------------------------------------------------------------------------------------------------

/**
 * This is a connector that allows data to flow between modules.
 *
 * The connections are the primary means of interacting with a module.
 * So they are generally declared as public.
 */
class AudioGraphPort : public FlowGraph {
public:
  explicit AudioGraphPort(FlowGraphNode& parent, int32_t samplesPerFrame) noexcept
    : mContainingNode(parent),
      mChannelCount(samplesPerFrame) {
  }

  ~AudioGraphPort() override = default;

  [[nodiscard]] int32_t getChannelCount() const {
    return mChannelCount;
  }

  virtual int32_t pullData(int64_t framePosition, int32_t numFrames) = 0;

  virtual void pullReset() {
  }

protected:
  FlowGraphNode& mContainingNode;

private:
  const int32_t mChannelCount = 1;
};

//--------------------------------------------------------------------------------------------------

/**
 * This connection contains a 32-bit float buffer that can contain several
 * frames of data. Processing the data in a block improves performance.
 *
 * The size is (framesPerBuffer * samplesPerFrame).
 */
class AudioGraphPortFloat : public AudioGraphPort {
public:
  AudioGraphPortFloat(FlowGraphNode& parent, const int32_t samplesPerFrame)
    : AudioGraphPortFloat(parent, samplesPerFrame, kDefaultBufferSize) {
  }

  explicit AudioGraphPortFloat(FlowGraphNode& parent, int32_t samplesPerFrame, int32_t framesPerBuffer) noexcept;

  [[nodiscard]] int32_t getFramesPerBuffer() const {
    return mFramesPerBuffer;
  }

protected:
  virtual float* getBuffer() {
    return mBuffer.get();
  }

private:
  const int32_t mFramesPerBuffer = 1;
  std::unique_ptr<float[]> mBuffer; // allocate in constructor
};

//--------------------------------------------------------------------------------------------------

/**
 * The results of a node's processing are stored in the buffers of the output
 * connections.
 */
class AudioGraphPortFloatOutput final : public AudioGraphPortFloat {
public:
  explicit AudioGraphPortFloatOutput(FlowGraphNode& parent, const int32_t samplesPerFrames) noexcept
    : AudioGraphPortFloat(parent, samplesPerFrames) {
  }

  using AudioGraphPortFloat::getBuffer;

  /**
   * Connect to the input of another module.
   * An input port can only have one connection.
   * An output port can have multiple connections.
   * If you connect a second output port to an input port
   * then it overwrites the previous connection.
   *
   * This not thread-safe. Do not modify the graph topology from another thread
   * while running. Also do not delete a module while it is connected to another
   * port if the graph is running.
   */
  void connect(AudioGraphPortFloatInput* port);

  /**
   * Disconnect from the input of another module.
   * This not thread-safe.
   */
  void disconnect(AudioGraphPortFloatInput* port);

  /**
   * Call the parent module's onProcess() method.
   * That may pull data from its inputs and recursively
   * process the entire graph.
   * @return number of frames actually pulled
   */
  int32_t pullData(int64_t framePosition, int32_t numFrames) override;

  void pullReset() override;
};

//--------------------------------------------------------------------------------------------------

/**
 * An input connection for streaming audio data.
 * You can set a value that will be used for processing.
 * If you connect an output port to this port then its value will be used
 * instead.
 */
class AudioGraphPortFloatInput final : public AudioGraphPortFloat {
public:
  explicit AudioGraphPortFloatInput(FlowGraphNode& parent, const int32_t samplesPerFrame) noexcept
    : AudioGraphPortFloat(parent, samplesPerFrame) {
    parent.addInputPort(*this);
  }

  /**
     * If connected to an output connection then this will return
     * that output ports buffers.
     * If not connected then it returns the input ports own buffer
     * which can be loaded using setValue().
     */
  float* getBuffer() override;

  /**
   * Write every value of the float buffer.
   * This value will be ignored if an output connection is connected
   * to this port.
   */
  void setValue(const float value) {
    const auto numFloats = kDefaultBufferSize * getChannelCount();
    float* buffer = getBuffer();
    for (int i = 0; i < numFloats; i++) {
      *buffer++ = value;
    }
  }

  /**
   * Connect to the output of another module.
   */
  void connect(AudioGraphPortFloatOutput* output) {
    assert(getChannelCount() == output->getChannelCount());
    mConnected = output;
  }

  void disconnect(const AudioGraphPortFloatOutput* output) {
    assert(mConnected == output);
    (void) output;
    mConnected = nullptr;
  }

  void disconnect() {
    mConnected = nullptr;
  }

  /**
   * Pull data from any output port that is connected.
   */
  int32_t pullData(int64_t framePosition, int32_t numFrames) override;

  void pullReset() override;

private:
  AudioGraphPortFloatOutput* mConnected = nullptr;
};

//----------------------------------------------------------------------------------------------------------------------

/**
 * Base class for an edge node in a graph that has no upstream nodes.
 * It outputs data but does not consume data.
 * By default, it will read its data from an external buffer.
 */
class FlowGraphSource : public FlowGraphNode {
public:
  explicit FlowGraphSource(int32_t channelCount) noexcept
      : output(*this, channelCount) {}

  AudioGraphPortFloatInput* getInput(uint32_t bus) const override {
    return nullptr;
  }

  AudioGraphPortFloatOutput* getOutput() { return &output; }

protected:
  AudioGraphPortFloatOutput output;
};

/**
 * Base class for an edge node in a graph that has no upstream nodes.
 * It outputs data but does not consume data.
 * By default, it will read its data from an external buffer.
 */
class FlowGraphSourceBuffered : public FlowGraphSource {
public:
  enum class SourceType {
    kSourceFloat,
    kSourceInt16,
    kSourceInt24,
    kSourceInt32
  };

  static std::unique_ptr<FlowGraphSourceBuffered>
  make(int32_t channelCount, SourceType sourceType = SourceType::kSourceFloat);

  explicit FlowGraphSourceBuffered(int32_t channelCount) noexcept
      : FlowGraphSource(channelCount) {}

  void setData(const void* data, int32_t numFrames) {
    mData = data;
    mSizeInFrames = numFrames;
    mFrameIndex = 0;
  }

protected:
  const void* mData = nullptr;
  int32_t mSizeInFrames = 0; // number of frames in mData
  int32_t mFrameIndex = 0;   // index of next frame to be processed
};

//----------------------------------------------------------------------------------------------------------------------
/**
 * Base class for an edge node in a graph that has no downstream nodes.
 * It consumes data but does not output data.
 * This graph will be executed when data is read() from this node
 * by pulling data from upstream nodes.
 */
class FlowGraphSink : public FlowGraphNode {
public:
  enum class SinkType {
    kSinkFloat, kSinkInt16, kSinkInt24, kSinkInt32
  };

  static std::unique_ptr<FlowGraphSink>
  make(uint32_t channelCount, SinkType sinkType = SinkType::kSinkFloat);

  explicit FlowGraphSink(uint32_t channelCount) noexcept;

  /**
   * Do nothing. The work happens in the read() method.
   *
   * @param numFrames maximum number of frames requested for processing
   * @return number of frames actually processed
   */
  int32_t onProcess(int32_t numFrames) override { return numFrames; }

  virtual int32_t read(void* data, int32_t numFrames) = 0;

  /**
   * Returns the input port.
   * The AudioGraphSink is only supported only one input port, so the bus is
   * ignore.
   */
  AudioGraphPortFloatInput* getInput(uint32_t bus) const override;

protected:
  /**
   * Pull data through the graph using this nodes last callCount.
   * @param numFrames
   * @return
   */
  int32_t pullData(int32_t numFrames);

  std::unique_ptr<AudioGraphPortFloatInput> input;
};


//----------------------------------------------------------------------------------------------------------------------

/**
 * Base class for a node that has an input and an output with the same number of
 * channels. This may include traditional filters, eg. FIR, but also include any
 * processing node that converts input to output.
 *
 * The AudioGraphFilter only has one input port.
 */
class FlowGraphFilter : public FlowGraphNode {
public:
  explicit FlowGraphFilter(int32_t channelCount) noexcept;

  const std::string& getManufacturerName() const { return mManufacturerName; }

  int32_t getVersion() const { return mVersion; }

  AudioGraphPortFloatInput* getInput(uint32_t bus) const override;

  AudioGraphPortFloatOutput* getOutput() { return &output; }

protected:
  static constexpr int64_t kInitialCallCount = -1;
  std::string mManufacturerName;
  int32_t mVersion;

  std::unique_ptr<AudioGraphPortFloatInput> input;
  AudioGraphPortFloatOutput output;
};

} // namespace coral

#endif // CORAL_AUDIO_FLOW_GRAPH_NODE_H
