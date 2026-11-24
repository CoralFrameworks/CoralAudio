# ==================================================================================================
# CoralAudio examples
# ==================================================================================================
# The framework's standalone demo and verification program. `audio_engine_demo` plays an audio file
# through the realtime graph:
#
#     AudioFile -> AudioPlayerNode -> mainMixerNode -> AudioOutputNode -> device
#
# and verifies the render as it plays: the player's node time must follow the wall clock, the frames
# it rendered must match the segment, and the tap on the main mixer must be delivered from the
# engine's IO thread. It is the manual check that the platform stream actually renders on the host.
#
# Two ways to build it:
#
#   standalone, as its own project
#     cmake -S examples -B build/examples -DCMAKE_BUILD_TYPE=Release
#     cmake --build build/examples
#     ./build/examples/audio_engine_demo <file> [seconds]
#
#   added by the framework root
#     A top-level configure of the repository adds this directory (see the root CMakeLists), so the
#     standalone build and the repository build share one target. A project that consumes CoralAudio
#     as a submodule does not get this executable.
#
# The demo needs an audio file. It takes one on the command line, and otherwise falls back to
# assets/music/music.wav in the checkout, which is compiled in as CORAL_ASSETS_DIR. A checkout
# without that directory still builds it; pass a file when running it.
#
# The submodules have to be initialized first (`git submodule update --init --recursive`), and the
# CoralMedia third-party submodules with `update = none` fetched explicitly with a per-submodule
# strategy override; see the header of the repository README.

set(CORALAUDIO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Standalone. Resolve the dependencies and add the framework the same way the application root does:
  # the dependency graph first, then the framework, then the examples.
  cmake_minimum_required(VERSION 3.19)
  project(CoralAudioExamples LANGUAGES CXX)

  # CoralCore and CoralMedia carry Objective-C++ on Apple; the language has to be enabled before the
  # framework adds them.
  if (APPLE)
    enable_language(OBJCXX)
  endif ()

  set(IS_HOST_PLATFORM TRUE)

  # The examples link the framework's libraries; they do not build or run its unit suites, which is
  # why this project does not need GoogleTest even when the third-party tree carries it.
  set(CORALAUDIO_BUILD_TESTS OFF CACHE BOOL "Build the CoralAudio tests" FORCE)
  set(CORAL_BUILD_TESTS OFF CACHE BOOL "Build the CoralAudio tests" FORCE)

  add_subdirectory("${CORALAUDIO_ROOT}" "${CMAKE_BINARY_DIR}/coral-audio")

  # The framework's CMakeLists defines this function. Calling it here gives the demo the same C++
  # standard and runtime flags as the libraries it links; the examples project is the parent of the
  # framework in this configure, so it does not inherit them through directory scope.
  coralaudio_apply_build_settings()
endif ()

# The demo plays an audio file from the assets directory when the command line does not name one, so
# that path is compiled in. A CoralAudio checkout keeps its fixtures in assets/ at the root of the
# repository; an application tree keeps them in assets/ two directories above it, and older
# checkouts used testdata/.
if (NOT DEFINED CORAL_TEST_DATA_DIR)
  if (EXISTS "${CORALAUDIO_ROOT}/assets")
    set(CORAL_TEST_DATA_DIR "${CORALAUDIO_ROOT}/assets")
  elseif (EXISTS "${CORALAUDIO_ROOT}/../../assets")
    set(CORAL_TEST_DATA_DIR "${CORALAUDIO_ROOT}/../../assets")
  elseif (EXISTS "${CORALAUDIO_ROOT}/testdata")
    set(CORAL_TEST_DATA_DIR "${CORALAUDIO_ROOT}/testdata")
  else ()
    set(CORAL_TEST_DATA_DIR "${CORALAUDIO_ROOT}/assets")
  endif ()
endif ()

add_executable(audio_engine_demo audio_engine_demo.cpp)
target_link_libraries(audio_engine_demo PRIVATE audio minilog)
set_target_properties(audio_engine_demo PROPERTIES FOLDER Examples)
target_compile_definitions(audio_engine_demo PRIVATE CORAL_ASSETS_DIR="${CORAL_TEST_DATA_DIR}")
