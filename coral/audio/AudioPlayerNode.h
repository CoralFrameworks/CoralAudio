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

#ifndef CORAL_AUDIO_PLAYER_NODE_H
#define CORAL_AUDIO_PLAYER_NODE_H

#include <coral/audio/AudioFile.h>
#include <coral/audio/AudioMixing.h>
#include <coral/audio/AudioNode.h>

#include <functional>

namespace coral {

/**
 * Play buffers or segments of audio files.
 *
 * Buffers and segments may be scheduled at specific points in time, or to play
 * immediately following preceding segments.
 */
class AudioPlayerNode : public AudioNode, public AudioMixing {
public:
  AudioPlayerNode();

  /**
   * Specifies when the completion handler must be invoked.
   */
  enum class CompletionCallbackType : uint8_t {
    //!< The buffer or file data has been consumed by the player.
    kConsumed = 0,
    //!< The buffer or file data has been rendered by the player.
    kRendered = 1,
    //!< Applicable only when the engine is rendering to/from an audio device.
    //!< The buffer or file has finished playing.
    kPlayedBack = 2,
  };

  /**
   * Buffer or file completion callback handler.
   *
   * @param callbackType Indicates the type of buffer or file completion when the callback is invoked.
   */
  using CompletionHandler = std::function<void(CompletionCallbackType callbackType)>;

  /**
   * Schedule playing samples from an AudioBuffer.
   *
   * @param buffer the buffer to play
   * @param completionHandler called after the buffer has been consumed by the
   * player or the player is stopped. may be nil.
   *
   * @discussion
   *     Schedules the buffer to be played following any previously scheduled
   * commands.
   *
   *     It is possible for the completionHandler to be called before rendering
   * begins or before the buffer is played completely.
   */
  void scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer, CompletionHandler completionHandler);

  /**
   * Schedule playing samples from an AudioBuffer.
   *
   * @param buffer the buffer to play
   * @param callbackType option to specify when the completion handler must be
   * called
   * @param completionHandler called after the buffer has been consumed by the
   * player or the player is stopped. may be nil.
   *
   * @discussion
   *     Schedules the buffer to be played following any previously scheduled
   * commands.
   */
  void scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer,
    CompletionCallbackType callbackType,
    CompletionHandler completionHandler);

  /**
   * Options controlling buffer scheduling.
   */
  enum class BufferOptions : uint8_t {
    //!< The buffer loops indefinitely.
    kBufferLoops = 1,
    //!< The buffer interrupts any buffer already playing.
    kBufferInterrupts = 2,
    //!< The buffer interrupts any buffer already playing, at its loop point.
    kBufferInterruptsAtLoop = 4,
  };

  /**
   * Schedule playing samples from an AudioBuffer.
   *
   * @param buffer the buffer to play
   * @param when the time at which to play the buffer. see the discussion of
   * timestamps, above.
   * @param options options for looping, interrupting other buffers, etc.
   * @param completionHandler called after the buffer has been consumed by the
   * player or the player is stopped. may be nil.
   *
   * @discussion
   *     It is possible for the completionHandler to be called before rendering
   * begins or before the buffer is played completely.
   */
  void scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer,
    const std::optional<AudioTime>& when,
    BufferOptions options,
    CompletionHandler completionHandler);

  /**
   * Schedule playing samples from an AudioBuffer.
   *
   * @param buffer the buffer to play
   * @param when the time at which to play the buffer. see the discussion of
   * timestamps, above.
   * @param options options for looping, interrupting other buffers, etc.
   * @param callbackType option to specify when the completion handler must be
   * called
   * @param completionHandler called after the buffer has been consumed by the
   * player or the player is stopped. may be nil.
   */
  void scheduleBuffer(std::unique_ptr<AudioPCMBuffer> buffer,
    const std::optional<AudioTime>& when,
    BufferOptions options,
    CompletionCallbackType callbackType,
    CompletionHandler completionHandler);

  /**
   * Schedule playing of an entire audio file.
   *
   * @param file the file to play
   * @param when the time at which to play the file. see the discussion of
   * timestamps, above.
   * @param completionHandler called after the buffer has been consumed by the
   * player or the player is stopped. may be nil.
   *
   * @discussion
   *     It is possible for the completionHandler to be called before rendering
   * begins or before the file is played completely.
   */
  void scheduleFile(const std::shared_ptr<AudioFile>& file,
    const std::optional<AudioTime>& when,
    CompletionHandler completionHandler);

  /**
   * Schedule playing of an entire audio file.
   *
   * @param file the file to play
   * @param when the time at which to play the file. see the discussion of
   * timestamps, above.
   * @param callbackType option to specify when the completion handler must be
   * called
   * @param completionHandler called after the buffer has been consumed by the
   * player or the player is stopped. may be nil.
   *
   * @discussion
   *     It is possible for the completionHandler to be called before rendering
   * begins or before the file is played completely.
   */
  void scheduleFile(const std::shared_ptr<AudioFile>& file,
    const std::optional<AudioTime>& when,
    CompletionCallbackType callbackType,
    CompletionHandler completionHandler);

  /**
   * Schedule playing a segment of an audio file.
   *
   * @param file the file to play
   * @param startFrame the starting frame position in the stream
   * @param numberFrames the number of frames to play
   * @param when the time at which to play the region. see the discussion of
   * timestamps, above.
   * @param completionHandler called after the buffer has been consumed by the
   * player or the player is stopped. may be nil.
   *
   * @discussion
   *     It is possible for the completionHandler to be called before rendering
   * begins or before the file is played completely.
   */
  void scheduleSegment(const std::shared_ptr<AudioFile>& file,
    AudioFramePosition startFrame,
    AudioFrameCount numberFrames,
    const std::optional<AudioTime>& when,
    CompletionHandler completionHandler);

  /**
   * Schedule playing a segment of an audio file.
   *
   * @param file the file to play
   * @param startFrame the starting frame position in the stream
   * @param numberFrames the number of frames to play
   * @param when the time at which to play the region. see the discussion of
   * timestamps, above.
   * @param callbackType option to specify when the completion handler must be
   * called
   * @param completionHandler called after the buffer has been consumed by the
   * player or the player is stopped. may be nil.
   *
   * @discussion
   *     It is possible for the completionHandler to be called before rendering
   * begins or before the file is played completely.
   */
  void scheduleSegment(const std::shared_ptr<AudioFile>& file,
    AudioFramePosition startFrame,
    AudioFrameCount numberFrames,
    const std::optional<AudioTime>& when,
    CompletionCallbackType callbackType,
    CompletionHandler completionHandler);

  /**
   * Prepares previously scheduled file regions or buffers for playback.
   *
   * @param frameCount The number of sample frames of data to be prepared before returning.
   */
  void prepare(uint32_t frameCount);

  /**
   * Start or resume playback immediately.
   */
  void play();

  /**
   * Start or resume playback at a specific time.
   *
   * @param timeUs the node time at which to start or resume playback. 0
   * signifies "now".
   */
  void playAtTime(uint64_t timeUs);

  /**
   * Pause playback.
   *
   * The player's sample time does not advance while the node is paused.
   *
   * Note that pausing or stopping all the players connected to an engine does
   * not pause or stop the engine or the underlying hardware. The engine must be
   * explicitly paused or stopped for the hardware to stop.
   */
  void pause();

  /**
   * Clear all of the node's previously scheduled events and stop playback.
   *
   * All of the node's previously scheduled events are cleared, including any
   * that are in the middle of playing. The node's sample time (and therefore
   * the times to which new events are to be scheduled) is reset to 0, and will
   * not proceed until the node is started again (via play or playAtTime).
   *
   * Note that pausing or stopping all the players connected to an engine does
   * not pause or stop the engine or the underlying hardware. The engine must be
   * explicitly paused or stopped for the hardware to stop.
   */
  void stop();

  /**
   * Convert from player time to node time.
   *
   * @param playerTime a time relative to the player's start time
   * @return a node time
   *
   * This method and its inverse `playerTimeForNodeTime:` are discussed in the
   * introduction to this class.
   *
   * If the player is not playing when this method is called, nil is returned.
   */
  std::optional<AudioTime>
  nodeTimeForPlayerTime(const std::optional<AudioTime>& playerTime) const;

  /**
   * Convert from node time to player time.
   *
   * @param nodeTime a node time
   * @return a time relative to the player's start time
   *
   * This method and its inverse `nodeTimeForPlayerTime:` are discussed in the
   * introduction to this class.
   *
   * If the player is not playing when this method is called, nullptr is
   * returned.
   */
  std::optional<AudioTime>
  playerTimeForNodeTime(const std::optional<AudioTime>& nodeTime) const;

  /**
   * Indicates whether or not the player is playing
   */
  [[nodiscard]] bool isPlaying() const;

  std::shared_ptr<AudioMixingDestination>
  destinationForMixer(const std::shared_ptr<AudioNode>& mixer, AudioNodeBus bus) override;
};

} // namespace coral

#endif // CORAL_AUDIO_PLAYER_NODE_H
