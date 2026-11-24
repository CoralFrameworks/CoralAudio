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

#ifndef CORAL_AUDIO_FILE_H
#define CORAL_AUDIO_FILE_H

#include <coral/audio/AudioBase.h>
#include <coral/audio/AudioTypes.h>

#include <memory>
#include <string>

namespace coral {

class AudioFormat;
class AudioPCMBuffer;
class AudioFileBase;

/**
 * An audio file opened for reading or writing.
 *
 * Regardless of the file's actual format, reading and writing the file is done
 * via `AudioPCMBuffer` objects, containing samples in an `AudioSampleFormat`,
 * referred to as the file's "processing format." Conversions are performed to
 * and from the file's actual format.
 *
 * Reads and writes are always sequential, but random access is possible by
 * setting the framePosition property.
 */
class AudioFile final : public AudioNoncopyable {
public:
  explicit AudioFile(std::unique_ptr<AudioFileBase> audioFile) noexcept;

  ~AudioFile() override;

  /**
   * Open a file for reading.
   *
   * @param fileUrl the file to open
   * @param outError on exit, if an error occurs, a description of the error
   * @return an instance of AudioFile, nullptr when any error occurs.
   *
   * This opens the file for reading using the standard format (deinterleaved
   * floating point).
   */
  static std::shared_ptr<AudioFile>
  InitForReading(const std::string& fileUrl, Error** outError);

  /**
   * Open a file for reading, using a specified processing format.
   *
   * @param fileUrl the file to open
   * @param sampleFormat the process data type to use when reading from the file
   * @param interleaved whether to use an interleaved processing format
   * @param outError on exit, if an error occurs, a description of the error
   * @return an instance of AudioFile, nullptr when any error occurs.
   */
  static std::shared_ptr<AudioFile>
  InitForReading(const std::string& fileUrl, AudioSampleFormat sampleFormat, bool interleaved, Error** outError);

  /**
   * Open a file for writing.
   *
   * @param fileUrl the path at which to create the file
   * @param fileType the file type of the url
   * @param format the format of the file to create(See `AudioRecorder`). For
   * linear PCM, only interleaved formats are supported for the saved file, non
   * interleaved setting will be ignored and a warning is shown.
   * @param outError on exit, if an error occurs, a description of the error
   * @return an instance of AudioFile, nullptr when any error occurs.
   *
   * The file type to create can be set through the corresponding settings key.
   * If not set, it will be inferred from the file extension. Will overwrite a
   * file at the specified url if a file exists.
   */
  static std::shared_ptr<AudioFile>
  initForWriting(const std::string& fileUrl,
    const MediaFileType& fileType,
    const AudioFormat& format,
    Error** outError);

  /**
   * Read an entire buffer.
   *
   * @param buffer The buffer into which to read from the file. Its format must
   * match the file's processing format.
   * @param outError on exit, if an error occurs, a description of the error
   * @return True for success
   *
   * Reading sequentially from framePosition, attempts to fill the buffer to its
   * capacity. On return, the buffer's length indicates the number of sample
   * frames successfully read.
   */
  bool readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError);

  /**
   * Read an entire buffer.
   *
   * @param buffer The buffer into which to read from the file. Its format must
   * match the file's processing format.
   * @param frames The number of frames to read.
   * @param outError on exit, if an error occurs, a description of the error
   * @return True for success.
   */
  bool readIntoBuffer(std::unique_ptr<AudioPCMBuffer>& buffer, uint32_t frames, Error** outError);

  /**
   * Write a buffer.
   *
   * @param buffer The buffer from which to write to the file. Its format must
   * match the file's processing format.
   * @param outError on exit, if an error occurs, a description of the error
   * @return True for success.
   *
   * Writes sequentially. The buffer's frameLength signifies how much of the
   * buffer is to be written.
   */
  bool writeFromBuffer(const std::unique_ptr<AudioPCMBuffer>& buffer, Error** outError);

  /**
   * The url the file is reading or writing.
   */
  [[nodiscard]] std::string getFileUrl() const;

  /**
   * The on-disk format of the file.
   */
  [[nodiscard]] std::optional<AudioFormat> getFileFormat() const;

  /**
   * The processing format of the file.
   */
  [[nodiscard]] std::optional<AudioFormat> getProcessingFormat() const;

  /**
   * The number of sample frames in the file.
   *
   * Note: this can be expensive to compute for the first time.
   */
  [[nodiscard]] AudioFramePosition getLength() const;

  /**
   * The position in the file at which the next read or write will occur.
   */
  [[nodiscard]] AudioFramePosition getFramePosition() const;

  /**
   * Sets the position in the file at which the next read or write will occur.
   *
   * Set framePosition to perform a seek before a read or write. A read or write
   * operation advances the frame position by the number of frames read or
   * written.
   */
  void setFramePosition(AudioFramePosition framePosition);

  /**
   * Releases the AudioFile, after calling this method, no longer uses.
   */
  void release();

private:
  AudioFileBase* const impl;
};

} // namespace coral

#endif // CORAL_AUDIO_FILE_H
