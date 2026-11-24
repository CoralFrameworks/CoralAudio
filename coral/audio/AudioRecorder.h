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

#ifndef CORAL_AUDIO_RECORDER_H
#define CORAL_AUDIO_RECORDER_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioFormat.h>

#include <atomic>
#include <memory>

namespace coral {

/**
 * An object that records audio data to a file.
 */
class AudioRecorder : public AudioNoncopyable {
public:
  class Delegate {
  public:
    virtual ~Delegate() = default;

    /**
     * Called when a recording has been finished or stopped. This method is NOT called if
     * the recorder is stopped due to an interruption.
     */
    virtual void onFinishRecording(AudioRecorder* recorder, bool successfully) = 0;

    /**
     * If an error occurs while encoding it will be reported to the delegate.
     */
    virtual void onEncodeErrorOccur(AudioRecorder* recorder, const Error* error) = 0;
  };

  /**
   * Creates an AudioRecorder
   *
   * @param url      the output url for recording
   * @param fileType the output file type for recording
   * @param format   the audio format for recording, only support pcm_16bit or float data to record
   *
   * @return the new instance of the AudioRecorder or nullptr when any error occurs.
   */
  static std::shared_ptr<AudioRecorder>
  Make(const std::string& url, MediaFileType fileType, const AudioFormat& format);

  /**
   * Sets the delegate of the AudioRecorder.
   *
   * @param delegate the specified delegate
   */
  void setDelegate(std::unique_ptr<Delegate> delegate);

  /**
   * Creates the file and gets ready to record. Happens automatically on record.
   */
  bool prepareToRecord();

  void prepareAsync();

  /**
   * Starts or resumes recording to file.
   */
  bool record();

  /**
   * Starts recording at specified time in the future.
   *
   * @param timeUs Time is an absolute time based on and greater than
   * deviceCurrentTimeUs().
   * @return Returns true on success and false on failure.
   */
  bool recordAtTime(int64_t timeUs);

  /**
   * Records for a specified duration.
   * <p>
   * The recorder will stop when it has recorded this length of audio.
   *
   * @param durationUs The specified duration to record in microseconds.
   * @return Returns true on success and false on failure.
   */
  bool recordForDuration(int64_t durationUs);

  /**
   * Record for a specified duration at a specified time in the future.
   *
   * @param timeUs     Time is an absolute time based on and greater than
   * deviceCurrentTimeUs()
   * @param durationUs The specified duration to record in microseconds.
   * @return Returns true on success and false on failure.
   */
  bool recordAtTimeForDuration(int64_t timeUs, int64_t durationUs);

  /**
   * Pauses recording
   */
  void pause();

  /**
   * Stops recording. closes the file.
   */
  void stop();

  /**
   * Deletes the recorded file. recorder must be stopped.
   *
   * @return returns false on failure.
   */
  bool deleteRecording();

  /**
   * Whether is it recording or not?
   */
  virtual bool isRecording() const {
    return mStatus == Status::kRecording;
  }

  /**
   * Returns the url of the recorded file
   */
  [[nodiscard]] std::string getUrl() const {
    return mUrl;
  }

  /**
   * Returns the content type of the recorded file.
   */
  [[nodiscard]] MediaFileType getFileType() const {
    return mFileType;
  }

  /**
   * Returns the current time(us) of the recording. Only valid while recording
   */
  [[nodiscard]] int64_t getCurrentTimeUs() const {
    return mCurrentTimeUs;
  }

  /**
   * Gets the device current time - always valid
   */
  static int64_t getDeviceCurrentTimeUs();

  static std::string getName() {
    return "AudioRecorder";
  }

protected:
  explicit AudioRecorder(std::string url, MediaFileType fileType, AudioFormat format) noexcept;

  virtual void onPrepare() {
  }

  virtual void onPrepareAsync() {
  }

  virtual void onStartRecording(int64_t timeUs, int64_t durationUs) {
  }

  virtual void onStop() {
  }

  bool deleteFile();

  enum class Status : uint8_t {
    kIdle = 0,
    kPrepared = 1,
    kRecording = 2,
    kPaused = 3,
    kStopped = 4,
    kCompleted = 5,
    kFailed = 6
  };

  std::atomic<Status> mStatus;

  std::string mUrl;
  MediaFileType mFileType;
  AudioFormat mAudioFormat;

  std::unique_ptr<Delegate> mDelegate;

  std::atomic<int64_t> mCurrentTimeUs{};
  std::atomic<int64_t> mMaxRecordDurationUs{};
};

} // namespace coral

#endif // CORAL_AUDIO_RECORDER_H
