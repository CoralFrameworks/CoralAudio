# ==================================================================================================
# CoralAudio
# ==================================================================================================
# The audio runtime framework: the audio graph, player/recorder, AudioUnit effects and audio file
# APIs. It is the counterpart of AVFAudio, not of the composition/export core in CoralCompose.
#
# The repository is both a standalone project and the source of the audio libraries an application
# links. The public headers live in coral/ at this root, the implementation in src/, the private
# headers in private/, and the bundled resampler in resampler/.
#
# CoralAudio depends on CoralCore and CoralMedia (codec, resampler and media data). CoralPlayback and
# CoralCompose both depend on CoralAudio; they remain independent of each other.

cmake_minimum_required(VERSION 3.19)
project(CoralAudio LANGUAGES CXX)

# CoralCore and CoralMedia contain Objective-C++ platform code on Apple, and the audio module may
# add more of it over time. The language has to be enabled here, before the dependencies are added,
# so a subproject's `enable_language(OBJCXX)` does not end up in a child directory scope only.
if (APPLE)
  enable_language(OBJCXX)
endif ()


# ==================================================================================================
# Build settings
# ==================================================================================================
# These settings apply to CoralAudio and, through directory scope, to the dependencies added below.
# The standalone examples project is the exception: it is the parent directory there, so it calls
# `coralaudio_apply_build_settings()` after adding this repository.
#
# Keep the block free of paths and third-party discovery: those are the concerns of the sections
# below, while this part is the C++ standard, the MSVC runtime and the warning baseline.
function(coralaudio_apply_build_settings)
  option(USE_STATIC_CRT "Link against the static runtime libraries." ON)

  if (MSVC)
    if (USE_STATIC_CRT)
      add_compile_options(
              $<$<CONFIG:>:/MT>
              $<$<CONFIG:Debug>:/MTd>
              $<$<CONFIG:Release>:/MT>
      )
    else ()
      add_compile_options(
              $<$<CONFIG:>:/MD>
              $<$<CONFIG:Debug>:/MDd>
              $<$<CONFIG:Release>:/MD>
      )
    endif ()
  endif ()

  # Windows: generate the .def that makes a DLL import library. CL.EXE does not produce one from
  # __declspec(dllexport) alone in every configuration.
  if (WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON PARENT_SCOPE)
  endif ()

  set(CMAKE_CXX_STANDARD 20 PARENT_SCOPE)
  set(CMAKE_CXX_STANDARD_REQUIRED ON PARENT_SCOPE)

  if (MSVC)
    # The project builds with C++20 (set above); keep the historical /W0 and the __cplusplus value
    # that libraries such as nlohmann/json check.
    set(CMAKE_CXX_FLAGS
            "${CMAKE_CXX_FLAGS} /W0 /Zc:__cplusplus"
            PARENT_SCOPE)
  else ()
    set(CMAKE_CXX_FLAGS
            "${CMAKE_CXX_FLAGS} -fstrict-aliasing -Wno-unknown-pragmas -Wno-unused-function -Wno-deprecated-declarations"
            PARENT_SCOPE)
    if (APPLE)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nullability-extension" PARENT_SCOPE)
      set(CMAKE_OBJCXX_FLAGS "${CMAKE_CXX_FLAGS}" PARENT_SCOPE)
    endif ()
  endif ()

  if (WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_USE_MATH_DEFINES=1" PARENT_SCOPE)
  endif ()
endfunction()

coralaudio_apply_build_settings()

# `audio` is one target; this switch decides whether it is libcoral-audio.a or libcoral-audio.so
# (coral-audio.dll on Windows). A shared CoralAudio links the framework's static dependencies, so
# they have to be position independent as well; the setting is applied before they are added.
option(BUILD_SHARED_LIBS "Build the CoralAudio library as a shared library" OFF)
if (BUILD_SHARED_LIBS)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif ()

# Indicates whether CoralAudio is standalone project.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CORAL_AUDIO_TOP_LEVEL ON)
else ()
  set(CORAL_AUDIO_TOP_LEVEL OFF)
endif ()

if (CORAL_AUDIO_TOP_LEVEL)
  if (NOT DEFINED IS_HOST_PLATFORM)
    set(IS_HOST_PLATFORM TRUE)
  endif ()
  if (NOT DEFINED CORAL_BUILD_TESTS)
    set(CORAL_BUILD_TESTS ON CACHE BOOL "Build the CoralAudio tests")
  endif ()
  set(CORAL_AUDIO_BUILD_TESTS ON CACHE BOOL "Build the CoralAudio tests")
else ()
  if (NOT DEFINED CORAL_BUILD_TESTS)
    set(CORAL_BUILD_TESTS OFF CACHE BOOL "Build the CoralAudio tests")
  endif ()
  set(CORAL_AUDIO_BUILD_TESTS ${CORAL_BUILD_TESTS} CACHE BOOL "Build the CoralAudio tests")
endif ()
set(CORAL_BUILD_TESTS ${CORAL_AUDIO_BUILD_TESTS})
if (CORAL_BUILD_TESTS)
  enable_testing()
endif ()

if (CORAL_AUDIO_TOP_LEVEL)
  # Resolve the shared third-party tree. There are three layouts, and the first candidate that has
  # the wrapper this configure needs wins:
  #
  #   third_party/                      a standalone CoralAudio checkout (libgtest and libz are this
  #                                     repository's own submodules, which CLion and a fresh clone
  #                                     see first)
  #   ../../third_party                 the application tree, two levels above frameworks/CoralAudio
  #   external/Coral*/third_party       the copies carried by the CoralCore and CoralMedia submodules
  if (NOT DEFINED CORAL_SHARED_THIRD_PARTY_DIR)
    foreach (_coral_third_party_candidate
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/CoralMedia/third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/CoralCore/third_party")
      if (CORAL_BUILD_TESTS)
        set(_coral_third_party_probe "${_coral_third_party_candidate}/libgtest/coral/CMakeLists.txt")
      else ()
        set(_coral_third_party_probe "${_coral_third_party_candidate}/libz/coral/CMakeLists.txt")
      endif ()
      if (EXISTS "${_coral_third_party_probe}")
        set(CORAL_SHARED_THIRD_PARTY_DIR "${_coral_third_party_candidate}")
        break()
      endif ()
    endforeach ()
  endif ()
  if (DEFINED CORAL_SHARED_THIRD_PARTY_DIR)
    message(STATUS "CoralAudio third-party tree: ${CORAL_SHARED_THIRD_PARTY_DIR}")
  endif ()
  # GoogleTest is a test-only dependency: a checkout or IDE profile that did not initialize it still
  # has to be able to configure and build the library, so the tests are disabled with a warning
  # instead of stopping the configure. The cache value keeps the next configure trying again.
  if (CORAL_BUILD_TESTS AND NOT TARGET gtest)
    if (DEFINED CORAL_SHARED_THIRD_PARTY_DIR AND
        EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libgtest/coral/CMakeLists.txt")
      add_subdirectory("${CORAL_SHARED_THIRD_PARTY_DIR}/libgtest/coral" "${CMAKE_BINARY_DIR}/gtest")
    else ()
      message(WARNING
          "GoogleTest was not found, so the CoralAudio tests are disabled. Initialize the "
          "submodule with\n"
          "  git -c submodule.third_party/libgtest.update=checkout submodule update --init "
          "third_party/libgtest\n"
          "pass -DCORAL_SHARED_THIRD_PARTY_DIR=<third_party>, or configure with "
          "-DCORAL_BUILD_TESTS=OFF.")
      set(CORAL_AUDIO_BUILD_TESTS OFF)
      set(CORAL_BUILD_TESTS OFF)
    endif ()
  endif ()
  # The fixtures of a CoralAudio checkout live in assets/ at the root of the repository; an
  # application tree that carries CoralAudio under frameworks/ keeps them in assets/ two directories
  # above it. The suites skip a fixture that is not present, so neither layout is required.
  if (NOT DEFINED CORAL_TEST_DATA_DIR)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
      set(CORAL_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../assets")
      set(CORAL_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../assets")
    endif ()
  endif ()
  if (NOT TARGET z AND DEFINED CORAL_SHARED_THIRD_PARTY_DIR AND
      EXISTS "${CORAL_SHARED_THIRD_PARTY_DIR}/libz/coral/CMakeLists.txt")
    add_subdirectory("${CORAL_SHARED_THIRD_PARTY_DIR}/libz/coral" "${CMAKE_BINARY_DIR}/libz")
  endif ()
endif ()

# ==================================================================================================
# Dependencies
# ==================================================================================================
if (NOT TARGET utils)
  if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/CoralCore/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/CoralCore" "${CMAKE_BINARY_DIR}/coral-core")
  elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../CoralCore/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../CoralCore" "${CMAKE_BINARY_DIR}/coral-core")
  endif ()
endif ()

if (NOT TARGET codec OR NOT TARGET utils)
  if (DEFINED CORAL_MEDIA_DIR AND EXISTS "${CORAL_MEDIA_DIR}/CMakeLists.txt")
    add_subdirectory("${CORAL_MEDIA_DIR}" "${CMAKE_BINARY_DIR}/coral-media")
  elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/CoralMedia/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/CoralMedia" "${CMAKE_BINARY_DIR}/coral-media")
  elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../CoralMedia/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../CoralMedia" "${CMAKE_BINARY_DIR}/coral-media")
  else ()
    message(FATAL_ERROR
        "CoralAudio needs CoralMedia. Add it before CoralAudio, pass -DCORAL_MEDIA_DIR=<path>, or "
        "keep the sibling ../CoralMedia submodule.")
  endif ()
endif ()

if (NOT TARGET sonic AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sonic/coral/CMakeLists.txt")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/sonic/coral" "${CMAKE_BINARY_DIR}/sonic")
endif ()

# ==================================================================================================
# CoralAudio library
# ==================================================================================================
# The target is named `audio`, which is what the rest of Coral links; the artifact is named
# `coral-audio` so the installed library is libcoral-audio.a or libcoral-audio.so.
add_subdirectory(resampler)

set(CORAL_AUDIO_TARGET audio)
set(CORAL_AUDIO_PUBLIC_HDR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(PUBLIC_HDRS
    coral/audio/AudioBuffer.h
    coral/audio/AudioChannelLayout.h
    coral/audio/AudioConnectionPoint.h
    coral/audio/AudioConverter.h
    coral/audio/AudioEngine.h
    coral/audio/AudioFifo.h
    coral/audio/AudioFile.h
    coral/audio/AudioFormat.h
    coral/audio/AudioIONode.h
    coral/audio/AudioMixerNode.h
    coral/audio/AudioMixing.h
    coral/audio/AudioNode.h
    coral/audio/AudioOutputSink.h
    coral/audio/AudioPlayer.h
    coral/audio/AudioPlayerNode.h
    coral/audio/AudioRecorder.h
    coral/audio/AudioSinkNode.h
    coral/audio/AudioSourceNode.h
    coral/audio/AudioTime.h
    coral/audio/AudioTypes.h
    coral/audio/AudioUnit.h
    coral/audio/AudioUnitDelay.h
    coral/audio/AudioUnitDistortion.h
    coral/audio/AudioUnitEffect.h
    coral/audio/AudioUnitEqualizer.h
    coral/audio/AudioUnitReverb.h
    coral/audio/AudioUnitTimeEffect.h
    coral/audio/AudioUnitTimePitch.h
    coral/audio/AudioUnitVarispeed.h
    coral/audio/AudioUtils.h
    coral/audio/OpResult.h
    coral/audio/Resample.h
)

set(SRCS
    # common
    src/common/AudioConverter.cpp
    src/common/AudioStream.cpp
    src/common/AudioStreamBuilder.cpp
    src/common/AudioStreamFactory.cpp
    src/common/AudioThreadPriority.cpp
    src/common/AudioUtils.cpp
    src/common/ostream.cpp
    # details
    src/details/AudioFileBase.h
    src/details/AudioFileForReading.cpp
    src/details/AudioFileForReading.h
    src/details/AudioFileForWriting.cpp
    src/details/AudioFileForWriting.h
    src/details/AudioOutputSinkImpl.cpp
    src/details/AudioOutputSinkImpl.h
    src/details/AudioPlayerImpl.cpp
    src/details/AudioPlayerImpl.h
    src/details/AudioRecorderImpl.cpp
    src/details/AudioRecorderImpl.h
    src/details/SampleFormatConverter.cpp
    src/details/SampleFormatConverter.h
    # src
    src/AudioBuffer.cpp
    src/AudioConnectionPoint.cpp
    src/AudioEngine.cpp
    src/AudioFifo.cpp
    src/AudioFile.cpp
    src/AudioFormat.cpp
    src/AudioIONode.cpp
    src/AudioMixerNode.cpp
    src/AudioMixing.cpp
    src/AudioNode.cpp
    src/AudioOutputSink.cpp
    src/AudioPlayer.cpp
    src/AudioPlayerNode.cpp
    src/AudioRecorder.cpp
    src/AudioSinkNode.cpp
    src/AudioSourceNode.cpp
    src/AudioUnit.cpp
    src/AudioUnitDelay.cpp
    src/AudioUnitDistortion.cpp
    src/AudioUnitEqualizer.cpp
    src/AudioUnitReverb.cpp
    src/AudioUnitTimePitch.cpp
    src/AudioUnitVarispeed.cpp
    src/GenericResample.cpp
    src/GenericResample.h
    src/Resample.cpp
)

set(PRIVATE_HDRS
    private/audio/AudioStream.h
    private/audio/AudioStreamBase.h
    private/audio/AudioStreamBuilder.h
    private/audio/AudioStreamCallback.h
    private/audio/AudioStreamFactory.h
    private/audio/AudioStreamTypes.h

    private/common/AudioRingBuffer.h
    private/common/AudioThreadPriority.h
)

# --------------------------------- graph implementation -------------------------------------
list(APPEND PRIVATE_HDRS
    private/graph/AudioEngineImpl.h
    private/graph/AudioGraphUtils.h
    private/graph/AudioInputNodeImpl.h
    private/graph/AudioMixerNodeImpl.h
    private/graph/AudioMixingUtils.h
    private/graph/AudioNodeImpl.h
    private/graph/AudioOutputNodeImpl.h
    private/graph/AudioPlayerNodeImpl.h
    private/graph/AudioSinkNodeImpl.h
    private/graph/AudioSourceNodeImpl.h
    private/graph/AudioUnitDelayImpl.h
    private/graph/AudioUnitDistortionImpl.h
    private/graph/AudioUnitEqualizerImpl.h
    private/graph/AudioUnitReverbImpl.h
    private/graph/AudioUnitTimePitchImpl.h
    private/graph/AudioUnitVarispeedImpl.h
)

list(APPEND SRCS
    src/graph/AudioEngineImpl.cpp
    src/graph/AudioGraphUtils.cpp
    src/graph/AudioInputNodeImpl.cpp
    src/graph/AudioMixerNodeImpl.cpp
    src/graph/AudioNodeImpl.cpp
    src/graph/AudioOutputNodeImpl.cpp
    src/graph/AudioPlayerNodeImpl.cpp
    src/graph/AudioSinkNodeImpl.cpp
    src/graph/AudioSourceNodeImpl.cpp
    src/graph/AudioUnitDelayImpl.cpp
    src/graph/AudioUnitDistortionImpl.cpp
    src/graph/AudioUnitEqualizerImpl.cpp
    src/graph/AudioUnitReverbImpl.cpp
    src/graph/AudioUnitTimePitchImpl.cpp
    src/graph/AudioUnitVarispeedImpl.cpp
)

# --------------------------------- flow graph implementation -------------------------------------

list(APPEND SRCS
    src/flowgraph/ChannelCountConverter.cpp
    src/flowgraph/ChannelCountConverter.h
    src/flowgraph/ClipToRange.cpp
    src/flowgraph/ClipToRange.h
    src/flowgraph/FlowGraphNode.cpp
    src/flowgraph/FlowGraphNode.h
    src/flowgraph/ManyToMultiConverter.cpp
    src/flowgraph/ManyToMultiConverter.h
    src/flowgraph/MonoToMultiConverter.cpp
    src/flowgraph/MonoToMultiConverter.h
    src/flowgraph/MultiToManyConverter.cpp
    src/flowgraph/MultiToManyConverter.h
    src/flowgraph/MultiToMonoConverter.cpp
    src/flowgraph/MultiToMonoConverter.h
    src/flowgraph/RampLinear.cpp
    src/flowgraph/RampLinear.h
    src/flowgraph/SampleRateConverter.cpp
    src/flowgraph/SampleRateConverter.h
    src/flowgraph/SinkFloat.cpp
    src/flowgraph/SinkFloat.h
    src/flowgraph/SinkInt16.cpp
    src/flowgraph/SinkInt16.h
    src/flowgraph/SinkInt24.cpp
    src/flowgraph/SinkInt24.h
    src/flowgraph/SinkInt32.cpp
    src/flowgraph/SinkInt32.h
    src/flowgraph/SourceFloat.cpp
    src/flowgraph/SourceFloat.h
    src/flowgraph/SourceInt16.cpp
    src/flowgraph/SourceInt16.h
    src/flowgraph/SourceInt24.cpp
    src/flowgraph/SourceInt24.h
    src/flowgraph/SourceInt32.cpp
    src/flowgraph/SourceInt32.h
)

# ==================================================================================================
# platform code sources
# ==================================================================================================

if (ANDROID)
  list(APPEND SRCS
      # AAudio implementation
      src/platform/android/aaudio/AAudioExtensions.h
      src/platform/android/aaudio/AAudioLoader.cpp
      src/platform/android/aaudio/AAudioLoader.h
      src/platform/android/aaudio/AudioStreamAAudio.cpp
      src/platform/android/aaudio/AudioStreamAAudio.h
      # fifo
      src/platform/android/fifo/FifoBuffer.cpp
      src/platform/android/fifo/FifoBuffer.h
      src/platform/android/fifo/FifoController.cpp
      src/platform/android/fifo/FifoController.h
      src/platform/android/fifo/FifoControllerBase.cpp
      src/platform/android/fifo/FifoControllerBase.h
      src/platform/android/fifo/FifoControllerIndirect.cpp
      src/platform/android/fifo/FifoControllerIndirect.h
      # opensles
      src/platform/android/opensles/AudioStreamBuffered.cpp
      src/platform/android/opensles/AudioStreamBuffered.h
      src/platform/android/opensles/AudioStreamInputOpenSLES.cpp
      src/platform/android/opensles/AudioStreamInputOpenSLES.h
      src/platform/android/opensles/AudioStreamOpenSLES.cpp
      src/platform/android/opensles/AudioStreamOpenSLES.h
      src/platform/android/opensles/AudioStreamOutputOpenSLES.cpp
      src/platform/android/opensles/AudioStreamOutputOpenSLES.h
      src/platform/android/opensles/EngineOpenSLES.cpp
      src/platform/android/opensles/EngineOpenSLES.h
      src/platform/android/opensles/OpenSLESUtilities.cpp
      src/platform/android/opensles/OpenSLESUtilities.h
      src/platform/android/opensles/OutputMixerOpenSLES.cpp
      src/platform/android/opensles/OutputMixerOpenSLES.h
      # android
      src/platform/android/AudioStreamAndroid.cpp
      src/platform/android/AudioStreamAndroid.h
      src/platform/android/AudioStreamBuilder.cpp
      src/platform/android/Definitions.h
      src/platform/android/MonotonicCounter.h
      src/platform/android/QuirksManager.cpp
      src/platform/android/QuirksManager.h
      src/platform/android/Utilities.cpp
      src/platform/android/Utilities.h
  )
endif ()

if (APPLE)
  list(APPEND SRCS
      # The builder is common API backed by a platform hook; the Windows, Linux, Android and macOS
      # builders each live next to their stream.
      src/platform/darwin/AudioStreamBuilder.cpp
      src/platform/darwin/AudioStreamCoreAudio.cpp
      src/platform/darwin/AudioStreamCoreAudio.h
  )
endif ()

if (LINUX)
  # ALSA is the Linux audio device API. Its development package supplies <alsa/asoundlib.h> and the
  # unversioned libasound.so link name; the runtime package alone has neither. The check is here
  # rather than at the first source file so a missing package is a configure error that names it.
  find_package(ALSA REQUIRED)

  list(APPEND SRCS
      # The builder is common API backed by a platform hook; the Windows and Android builders live
      # next to their streams, so Linux needs its own as well.
      src/platform/linux/AudioStreamBuilder.cpp
      src/platform/linux/AudioStreamAlsa.cpp
      src/platform/linux/AudioStreamAlsa.h
  )
endif ()

if (WIN32)
  list(APPEND SRCS
      src/platform/win32/AudioStreamBuilder.cpp
      src/platform/win32/AudioStreamWasapi.cpp
      src/platform/win32/AudioStreamWasapi.h
  )
endif ()

if (CORAL_SUPPORTS_FFMPEG)
  list(APPEND SRCS
      src/FfmpegResample.cpp
      src/FfmpegResample.h
  )
endif ()

# ==================================================================================================
# Include and target definitions
# ==================================================================================================
include_directories(${CORAL_AUDIO_PUBLIC_HDR_DIR})
include_directories(src)

add_library(${CORAL_AUDIO_TARGET} ${PUBLIC_HDRS} ${PRIVATE_HDRS} ${SRCS})
set_target_properties(${CORAL_AUDIO_TARGET} PROPERTIES OUTPUT_NAME coral-audio)

target_include_directories(${CORAL_AUDIO_TARGET} PUBLIC ${CORAL_AUDIO_PUBLIC_HDR_DIR})

# ==================================================================================================
# Dependencies
# ==================================================================================================
target_link_libraries(${CORAL_AUDIO_TARGET} PUBLIC codec)
target_link_libraries(${CORAL_AUDIO_TARGET} PUBLIC core)
target_link_libraries(${CORAL_AUDIO_TARGET} PUBLIC event)
target_link_libraries(${CORAL_AUDIO_TARGET} PRIVATE minilog)
target_link_libraries(${CORAL_AUDIO_TARGET} PUBLIC resampler)
target_link_libraries(${CORAL_AUDIO_TARGET} PUBLIC sonic)

if (ANDROID)
  target_link_libraries(${CORAL_AUDIO_TARGET} PRIVATE OpenSLES)
endif ()

if (CORAL_SUPPORTS_FFMPEG)
  target_link_libraries(${CORAL_AUDIO_TARGET} PUBLIC ffcodec)
endif ()

if (APPLE)
  # AUHAL comes from AudioToolbox; the device and property queries come from CoreAudio. CoreFoundation
  # is used to read the CFString device name/UID that CORAL_COREAUDIO_DEVICE selects.
  target_link_libraries(${CORAL_AUDIO_TARGET} PRIVATE
      "-framework AudioToolbox"
      "-framework CoreAudio"
      "-framework CoreFoundation")
endif ()

if (WIN32)
  target_link_libraries(${CORAL_AUDIO_TARGET} PRIVATE avrt ole32)
endif ()

if (LINUX)
  if (TARGET ALSA::ALSA)
    target_link_libraries(${CORAL_AUDIO_TARGET} PRIVATE ALSA::ALSA)
  else ()
    # Older CMake releases that do not define the imported target still publish these variables.
    target_include_directories(${CORAL_AUDIO_TARGET} PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(${CORAL_AUDIO_TARGET} PRIVATE ${ALSA_LIBRARIES})
  endif ()
endif ()

# ==================================================================================================
# Installation
# ==================================================================================================
# The artifact keeps the `coral-audio` name, while the target stays `audio` for consumers that add
# this repository with add_subdirectory(). A shared build installs the import library next to the
# static libraries and the DLL under bin/.
install(TARGETS ${CORAL_AUDIO_TARGET}
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
# The public headers are included as <coral/audio/...>, so `coral/` is what goes under include/.
install(DIRECTORY ${CORAL_AUDIO_PUBLIC_HDR_DIR}/coral DESTINATION include)

# ==================================================================================================
# Unit Tests
# ==================================================================================================
if (CORAL_BUILD_TESTS)
  add_executable(gtest_${CORAL_AUDIO_TARGET}
      tests/gtest_audio.cpp
      tests/gtest_AudioBuffer.cpp
      tests/gtest_AudioRingBuffer.cpp
  )
  target_link_libraries(gtest_${CORAL_AUDIO_TARGET} PRIVATE ${CORAL_AUDIO_TARGET} gtest)
  set_target_properties(gtest_${CORAL_AUDIO_TARGET} PROPERTIES FOLDER Tests)

  add_subdirectory(tests)
endif ()

# ==================================================================================================
# Examples
# ==================================================================================================
# The demos are a separate project that can be configured on its own (`cmake -S examples ...`); they
# are built as part of this repository only when it is the top-level project. An application that
# adds CoralAudio as a submodule gets the libraries it links and no example executables.
if (CORAL_AUDIO_TOP_LEVEL)
  add_subdirectory(examples)
endif ()
