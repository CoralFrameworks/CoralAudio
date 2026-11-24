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

#ifndef CORAL_AUDIO_MIXING_H
#define CORAL_AUDIO_MIXING_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioConnectionPoint.h>
#include <coral/audio/AudioTypes.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>

namespace coral {

class AudioMixingDestination;
class AudioNode;

/**
 * Protocol that defines properties applicable to the input bus of a mixer node
 *
 * @discussion
 * Nodes that conform to the AudioMixing protocol can talk to a mixer node downstream,
 * specifically of type AudioMixerNode or AudioEnvironmentNode. The properties defined
 * by this protocol apply to the respective input bus of the mixer node that the source
 * node is connected to. Note that effect nodes cannot talk to their downstream mixer.
 *
 * Properties can be set either on the source node, or directly on individual mixer connections.
 * Source node properties are:
 * - applied to all existing mixer connections when set
 * - applied to new mixer connections
 * - preserved upon disconnection from mixers
 * - not affected by connections/disconnections to/from mixers
 * - not affected by any direct changes to properties on individual mixer connections
 *
 * Individual mixer connection properties, when set, will override any values previously derived
 * from the corresponding source node properties. However, if a source node property is
 * subsequently set, it will override the corresponding property value of all individual mixer
 * connections.
 * Unlike source node properties, individual mixer connection properties are not preserved upon
 * disconnection (see `AudioMixing(destinationForMixer:bus:)` and `AudioMixingDestination`).
 *
 * Source nodes that are connected to a mixer downstream can be disconnected from
 * one mixer and connected to another mixer with source node's mixing settings intact.
 * For example, an AudioPlayerNode that is being used in a gaming scenario can set up its
 * 3D mixing settings and then move from one environment to another.
 */
class AudioMixing : public AudioNoncopyable {
public:
  /**
   * Returns the AudioMixingDestination object corresponding to specified mixer node and
   * its input bus
   *
   * @param mixer The mixer to get destination details for.
   * @param bus   The input bus.
   * @return Returns self if the specified mixer or input bus matches its connection point.
   *         If the mixer or input bus doesn't match its connection point, or if the source
   *         node isn’t in a connected state to the mixer or input bus, the method returns
   *         nullptr.
   *
   * @discussion
   * When a source node is connected to multiple mixers downstream, setting AudioMixing
   * properties directly on the source node will apply the change to all the mixers downstream.
   * If you want to set/get properties on a specific mixer, use this method to get the
   * corresponding AudioMixingDestination and set/get properties on it.
   *
   * Note:
   * - Properties set on individual AudioMixingDestination instances will not reflect at the
   *   source node level.
   *
   * - AudioMixingDestination reference returned by this method could become invalid when
   *   there is any disconnection between the source and the mixer node. Hence this reference
   *   should not be retained and should be fetched every time you want to set/get properties
   *   on a specific mixer.
   *
   * If the source node is not connected to the specified mixer/input bus, this method
   * returns nullptr.
   * Calling this on an AudioMixingDestination instance returns self if the specified
   * mixer/input bus matches its connection point, otherwise, it returns nil.
   */
  virtual std::shared_ptr<AudioMixingDestination>
  destinationForMixer(const std::shared_ptr<AudioNode>& mixer, AudioNodeBus bus);

  /**
   * Sets the bus's input volume
   *
   * @param newVolume the specified volume, in range {0.0, 1.0}
   *
   * @discussion
   * A volume outside the range is clamped, and a NaN does not change the property. The setter
   * is safe to call while the graph renders: the mixer reads the value on the render thread.
   */
  virtual void setVolume(const float newVolume) {
    if (std::isnan(newVolume)) {
      return;
    }
    volume.store(std::clamp(newVolume, 0.0f, 1.0f), std::memory_order_relaxed);
  }

  /**
   * Returns the bus's input volume, in range {0.0, 1.0}
   */
  [[nodiscard]] virtual float getVolume() const {
    const auto current = volume.load(std::memory_order_relaxed);
    if (std::isnan(current)) {
      return 1.0f;
    }
    return std::clamp(current, 0.0f, 1.0f);
  }

private:
  //!< Read by the mixer that the node feeds, written by the client thread.
  std::atomic<float> volume{ 1.0f };
};

/**
 * An object representing a connection to a mixer node from a node that conforms to AudioMixing protocol
 *
 * @discussion
 * A standalone instance of AudioMixingDestination cannot be created.
 * Only an instance vended by a source node (e.g. AudioPlayerNode) can be used (see `AudioMixing`).
 */
class AudioMixingDestination final : public AudioMixing,
                                     public std::enable_shared_from_this<AudioMixingDestination> {
public:
  explicit AudioMixingDestination(const std::optional<AudioConnectionPoint>& connection) noexcept
    : connectionPoint(connection) {
  }

  /**
   * Returns the underlying mixer connection point
   */
  const std::optional<AudioConnectionPoint>& getConnectionPoint() const {
    return connectionPoint;
  }

  std::shared_ptr<AudioMixingDestination>
  destinationForMixer(const std::shared_ptr<AudioNode>& mixer, AudioNodeBus bus) override;

private:
  //!< the connection point contains the mixer node and it's input bus.
  std::optional<AudioConnectionPoint> connectionPoint;
};

} // namespace coral

#endif // CORAL_AUDIO_MIXING_H
