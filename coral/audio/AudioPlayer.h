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

#ifndef CORAL_AUDIO_PLAYER_H
#define CORAL_AUDIO_PLAYER_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioTypes.h>

#include <atomic>
#include <memory>
#include <string>

namespace coral {

/**
 * The audio player
 */
class AudioPlayer : public AudioNoncopyable {
public:
  class Delegate {
  public:
    virtual ~Delegate() = default;

    /**
     * Called when a sound has finished playing.
     */
    virtual void onStartPlaying(AudioPlayer* player, int64_t durationUs) = 0;

    /**
     * Called when a sound has finished playing.
     * This method is NOT called if the player is stopped due to an interruption.
     */
    virtual void onFinishPlaying(AudioPlayer* player, bool successfully) = 0;

    /**
     * If an error occurs while decoding it will be reported to the delegate.
     */
    virtual void onDecodeErrorOccur(AudioPlayer* player, const Error* error) = 0;
  };

  /**
   * Creates a new AudioPlayer instance with specific url
   */
  static std::shared_ptr<AudioPlayer> Create(const std::string& url);

  /**
   * Sets the delegate of the AudioPlayer
   */
  void setDelegate(std::unique_ptr<Delegate> delegate);

  /**
   * Get ready to play the sound. This happens automatically on play.
   * @return true on success and false on failure.
   */
  bool prepareToPlay();

  /**
   * This method starts the audio hardware synchronously (if not already
   * running), and triggers the sound playback which is streamed asynchronously.
   */
  bool play();

  /**
   * This method starts the audio hardware synchronously (if not already
   * running), and triggers the sound playback which is streamed asynchronously
   * at the specified time in the future. Time is an absolute time based on and
   * greater than deviceCurrentTime.
   */
  bool playAtTime(int64_t timeUs);

  /**
   * Pauses playback, but remains ready to play.
   */
  void pause();

  /**
   * Synchronously stops playback, no longer ready to play.
   * NOTE:
   * This will block while releasing the audio hardware that was acquired upon
   * calling play() or prepareToPlay()
   */
  void stop();

  /**
   * Whether is it playing or not?
   */
  [[nodiscard]] bool isPlaying() const {
    return mStatus == Status::kPlaying;
  }

  /**
   * Seeks to a specified time. If the time is larger than the duration, the
   * playback finished callback is called immediately.
   * @param timeUs the specified time to seek.
   */
  void seekTo(int64_t timeUs);

  /**
   * Returns the current time playing.
   *
   * If the sound is playing, currentTime is the offset into the sound of the
   * current playback position. If the sound is not playing, currentTime is the
   * offset into the sound where playing would start.
   */
  int64_t getCurrentTimeUs() const {
    return mCurrentTimeUs;
  }

  /**
   * Returns the duration(us) of the player
   */
  int64_t getDurationUs() const {
    return mDurationUs;
  }

  /**
   * Returns the number of channel
   */
  [[nodiscard]] int getNumberOfChannels() const {
    return mNumberOfChannels;
  }

  /**
   * Returns the url to playback
   */
  [[nodiscard]] std::string getUrl() const {
    return mUrl;
  }

  /**
   * Returns the volume for the sound.
   * The nominal range is from 0.0 to 1.0.
   */
  [[nodiscard]] float getVolume() const {
    return mVolume.load();
  }

  /**
   * Sets the volume for the sound.
   *
   * @param volume The specified volume value. The nominal range is from 0.0 to 1.0.
   */
  void setVolume(float volume);

  /**
   * You must set enableRate to true for the rate property to take effect.
   * You must set this before calling prepareToPlay.
   */
  [[nodiscard]] bool isEnableRate() const {
    return mEnableRate;
  }

  /**
   * Sets whether enable to playback at specified rate
   */
  void setEnableRate(bool enableRate);

  /**
   * Returns the rate of the playback.
   * The playback rate for the sound. 1.0 is normal, 0.5 is half speed, 2.0 is
   * double speed.
   */
  [[nodiscard]] float getRate() const {
    return mPlaybackRate;
  }

  /**
   * Sets the rate of the playback.
   * @param rate  The playback rate for the sound. 1.0 is normal, 0.5 is half
   * speed, 2.0 is double speed.
   */
  void setRate(float rate);

  /**
   * Returns the number of times that the sound will return to the beginning
   * upon reaching the end. A value of zero means to play the sound just once.
   * A value of one will result in playing the sound twice, and so on..
   * Any negative number will loop indefinitely until stopped.
   */
  int getNumberOfLoops() const {
    return mNumberOfLoops;
  }

  /**
   * Sets the loop times for playback
   *
   * A value of zero means to play the sound just once.
   * A value of one will result in playing the sound twice, and so on..
   * Any negative number will loop indefinitely until stopped.
   */
  void setNumberOfLoops(int loopCount);

protected:
  explicit AudioPlayer(std::string url) noexcept;

  virtual bool onPrepare() = 0;

  virtual bool onPlayDelay(int64_t delayMs) = 0;

  virtual void onPause() = 0;

  virtual void onStop() = 0;

  virtual void onPlaybackRateUpdate() = 0;

  virtual void onSeekUpdate(int64_t timeUs) = 0;

  virtual void onVolumeUpdate() = 0;

  enum class Status : uint8_t {
    // clang-format off
    kIdle = 0,      //!< when player is idle.
    kPrepared = 1,  //!< When player is prepared and ready to play
    kPlaying = 2,   //!< When player is playing.
    kPaused = 3,    //!< When player is paused.
    kStopped = 4,   //!< When player has stopped.
    kCompleted = 5, //!< When player has playback completed
    kFailed = 6,    //!< When player has something error occurs.
    // clang-format on
  };

  std::atomic<Status> mStatus;

  std::string mUrl;
  std::unique_ptr<Delegate> mDelegate;

  uint32_t mNumberOfChannels;
  std::atomic<float> mVolume;

  std::atomic<int64_t> mCurrentTimeUs;
  std::atomic<int64_t> mDurationUs;

  std::atomic_int mNumberOfLoops;

  std::atomic_bool mEnableRate;
  std::atomic<float> mPlaybackRate;
};

} // namespace coral

#endif // CORAL_AUDIO_PLAYER_H
