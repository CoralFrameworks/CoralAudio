# ==================================================================================================
# Tests
#
# Each test is a standalone executable named tests_<Thing>, registered with CTest.
# ==================================================================================================
add_executable(tests_AudioConverter tests_AudioConverter.cpp)
target_link_libraries(tests_AudioConverter PRIVATE audio)

add_executable(test_AudioThreadPriority test_AudioThreadPriority.cpp)
target_link_libraries(test_AudioThreadPriority PRIVATE audio utils)

# The graph tests render in the manual rendering modes, so they need no audio endpoint
# and can run anywhere. They include the private graph headers, which this directory
# inherits through the parent scope. The file playback test is skipped when the asset is
# not part of the checkout.
add_executable(tests_AudioEngine tests_AudioEngine.cpp)
target_link_libraries(tests_AudioEngine PRIVATE audio)
target_compile_definitions(tests_AudioEngine PRIVATE CORAL_ASSETS_DIR="${CORAL_TEST_DATA_DIR}")

# The player and the recorder drive the real endpoints and the file system, so every section
# of their test is skipped when the endpoint it needs is unavailable.
add_executable(tests_AudioPlayerRecorder tests_AudioPlayerRecorder.cpp)
target_link_libraries(tests_AudioPlayerRecorder PRIVATE audio)
target_compile_definitions(tests_AudioPlayerRecorder PRIVATE CORAL_ASSETS_DIR="${CORAL_TEST_DATA_DIR}")

add_test(NAME tests_AudioConverter COMMAND tests_AudioConverter)
add_test(NAME test_AudioThreadPriority COMMAND test_AudioThreadPriority)
add_test(NAME tests_AudioEngine COMMAND tests_AudioEngine)
add_test(NAME tests_AudioPlayerRecorder COMMAND tests_AudioPlayerRecorder)

# The WASAPI stream is the Windows implementation of AudioStream, so the test that
# drives it against the real endpoints only exists there.
if (WIN32)
  add_executable(tests_AudioStreamWasapi tests_AudioStreamWasapi.cpp)
  target_link_libraries(tests_AudioStreamWasapi PRIVATE audio)
  add_test(NAME tests_AudioStreamWasapi COMMAND tests_AudioStreamWasapi)
endif ()

# The ALSA stream is the Linux implementation. A machine without a PCM skips the
# test; CORAL_ALSA_DEVICE=null runs it against ALSA's null plugin.
if (LINUX)
  add_executable(tests_AudioStreamAlsa tests_AudioStreamAlsa.cpp)
  target_link_libraries(tests_AudioStreamAlsa PRIVATE audio)
  add_test(NAME tests_AudioStreamAlsa COMMAND tests_AudioStreamAlsa)
endif ()

# The Core Audio stream is the macOS implementation. A Mac without an output or an input
# device skips the corresponding sections; CORAL_COREAUDIO_DEVICE selects another device.
if (APPLE)
  add_executable(tests_AudioStreamCoreAudio tests_AudioStreamCoreAudio.cpp)
  target_link_libraries(tests_AudioStreamCoreAudio PRIVATE audio)
  add_test(NAME tests_AudioStreamCoreAudio COMMAND tests_AudioStreamCoreAudio)
endif ()
