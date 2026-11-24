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

#ifndef CORAL_AUDIO_CONNECTION_POINT_H
#define CORAL_AUDIO_CONNECTION_POINT_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioTypes.h>

#include <memory>

namespace coral {

class AudioNode;

/**
 * A representation of either a source or destination connection point in AudioEngine.
 * AudioConnectionPoint describes either a source or destination connection point (node, bus)
 * in AudioEngine's graph.
 */
class AudioConnectionPoint final : AudioCopyable {
public:
  AudioConnectionPoint(const std::shared_ptr<AudioNode>& node, const AudioNodeBus bus)
    : mNode(node),
      mBus(bus) {
  }

  /**
   * Returns the node in the connection point.
   */
  [[nodiscard]] std::shared_ptr<AudioNode> node() const {
    return mNode.lock();
  }

  /**
   * Returns the bus on the node in the connection point.
   */
  [[nodiscard]] AudioNodeBus bus() const {
    return mBus;
  }

  [[nodiscard]] std::string toString() const;

  friend bool operator==(const AudioConnectionPoint& left, const AudioConnectionPoint& right) {
    return !left.mNode.owner_before(right.mNode) &&
           !right.mNode.owner_before(left.mNode) &&
           left.mBus == right.mBus;
  }

  friend std::ostream& operator<<(std::ostream& os, const AudioConnectionPoint& point) {
    return os << point.toString();
  }

private:
  std::weak_ptr<AudioNode> mNode;
  AudioNodeBus mBus;
};

} // namespace coral

#endif // CORAL_AUDIO_CONNECTION_POINT_H
