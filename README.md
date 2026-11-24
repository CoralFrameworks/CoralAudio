# CoralAudio

The audio runtime framework. It plays and records audio, exposes a realtime node graph with the
`AVAudioEngine` shape, and implements the effect units (`AVAudioUnitDelay`, `AVAudioUnitReverb`,
`AVAudioUnitDistortion`, `AVAudioUnitEQ`, `AVAudioUnitTimePitch`, `AVAudioUnitVarispeed`) on top of
the public API. It is the counterpart of AVFAudio; composition and export live in CoralCompose.

CoralAudio depends on CoralCore (`core`, `event`, `utils`, `minilog`) and CoralMedia (`codec`,
`media`, `resampler`) for codecs, media data and sample-rate conversion.

## Layout

```
coral/audio/            public headers, the API a client includes (<coral/audio/...>)
private/audio/          AudioStream, AudioStreamBuilder and the callback interfaces
private/graph/          the node-graph implementation behind the public node classes
private/common/         AudioRingBuffer and AudioThreadPriority
resampler/              the bundled sample-rate converter
src/
  Audio*.cpp            the public classes (graph, player, recorder, file, format, units)
  common/               the platform-independent half of AudioStream
  details/              AudioPlayerImpl, AudioRecorderImpl, file reading/writing
  graph/                AudioEngineImpl and the node implementations
  flowgraph/            the internal render graph nodes
  platform/
    android/            AAudio, OpenSL ES
    darwin/             Core Audio (AUHAL)
    linux/              ALSA
    win32/              WASAPI
tests/                  the tests, one executable per area plus the platform stream test
examples/               the standalone demo project (not built by a consumer)
assets/                 the fixtures of the tests and the demo
scripts/                the bootstrap tools that fetch the dependency repositories
```

`AudioStream` is the platform boundary. The graph, the player and the recorder never touch a device
API; they go through `AudioStreamBuilder::openStream()`, which validates the request, creates the
platform stream and opens it. Every backend implements the same contract:

- one direction (`Render` or `Capture`) and either a data callback (the HAL thread calls
  `AudioStreamDataCallback::onAudioReady`) or a blocking ring buffer (`write()`/`read()`);
- the explicit state machine `Open -> Starting -> Started -> Pausing/Paused -> Stopping/Stopped`,
  with `Flushing`, `Closed` and `Disconnected` as the other states;
- format negotiation: the caller asks for a sample rate, channel count and sample format, and reads
  back what the device accepted through `getSampleRate()`, `getChannelCount()` and `getFormat()`;
- `getFramesRead()`/`getFramesWritten()`, `getTimestamp()`, `calculateLatencyMillis()`,
  `getXRunCount()` and the error-callback path.

The backends differ in how they own the device, not in that contract:

| Platform | Device API | IO ownership |
| --- | --- | --- |
| Android | AAudio, OpenSL ES fallback | AAudio callback / OpenSL ES buffer queue |
| macOS | Core Audio AUHAL | Core Audio HAL thread, one control thread for start/stop |
| Linux | ALSA | one worker thread owns the PCM |
| Windows | WASAPI | an `EventThread` pumps and applies commands |

## Building

CoralAudio is a standalone CMake project. Its dependency repositories are declared in `.gitmodules`
but are not part of this checkout, so fetch them first with the bootstrap tool for the host:

macOS / Linux:

```sh
./scripts/bootstrap.sh
```

Windows PowerShell:

```powershell
.\scripts\bootstrap.ps1
```

If the PowerShell execution policy blocks local scripts, run it through
`powershell -ExecutionPolicy Bypass -File .\scripts\bootstrap.ps1`.

The tool reads `.gitmodules` and, for every entry, clones the repository from its default remote
branch into the declared path. An existing checkout is updated with a fast-forward pull, and a
detached submodule checkout is switched back to the remote default branch. When a parent repository
needs nested repositories, the tool initializes those too (Minilog for CoralCore, zlib for libz and
GoogleTest for libgtest). Nested repositories that the dependencies mark as `update = none` are
skipped by their own recursive update, because this project supplies those targets itself. The tool
is idempotent and stops with an error instead of discarding local changes.

Because the checkouts follow the remote branches rather than the commits recorded by this repository,
`git status` can report the dependency directories as modified or untracked; that is expected.

The repositories it manages are:

| Path | Contents |
| --- | --- |
| `external/CoralCore` | `utils`, `core`, `event` and Minilog |
| `external/CoralMedia` | codec, media, extractor and muxer |
| `third_party/libz` | zlib, used by the CoralMedia codec |
| `third_party/libgtest` | GoogleTest for the test suites |
| `third_party/libyuv` | only used with `-DCORAL_SUPPORTS_FFMPEG=ON` |

`third_party/sonic` is vendored directly in this repository and needs no download.

Requirements: Git, CMake 3.19 or newer and a C++20 compiler. Linux also needs the ALSA development
package, macOS the Xcode command line tools, and Windows Visual Studio with the C++ workload.

The standalone build is three steps:

```sh
# 1. Configure the project
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 2. Build the library, the tests and the demo
cmake --build build

# 3. Run the tests
ctest --test-dir build --output-on-failure
```

The demo is built with the project and can be run afterwards:

```sh
./build/examples/audio_engine_demo
```

On Windows, the same commands work from a Developer PowerShell for Visual Studio, or the generated
project can be opened in Visual Studio or CLion.

The CMake target is `audio` and the artifact is `coral-audio`: the default build produces
`libcoral-audio.a`, and `-DBUILD_SHARED_LIBS=ON` produces `libcoral-audio.so` (`.dylib` on macOS,
`coral-audio.dll` with an import library on Windows). `cmake --install` writes the library to `lib/`
(the DLL to `bin/`) and the public headers to `include/coral/audio/`, so a client compiles against
`<coral/audio/...>` with `include/` on its header path.

The configure locates the third-party tree itself, in this order, and uses the first candidate that
has the wrapper it needs:

1. `third_party/` — a standalone checkout (and what CLion sees first);
2. `../../third_party` — the application tree, two levels above `frameworks/CoralAudio`;
3. `external/CoralMedia/third_party` and `external/CoralCore/third_party` — the copies the framework
   submodules carry.

`-DCORAL_SHARED_THIRD_PARTY_DIR=<directory>` overrides the search; the directory has to contain
`libgtest/coral` and `libz/coral`. `third_party/sonic` is vendored directly in this repository and
needs no submodule.

When CoralAudio is added to an application as a subproject, the application provides CoralCore,
CoralMedia and the third-party targets first; the `NOT TARGET` guards in the root `CMakeLists.txt`
then skip the submodules and no dependency is configured twice.

### CLion and other IDEs

Run `scripts/bootstrap.sh` (or `scripts/bootstrap.ps1` on Windows) once, then open the repository
root in CLion as a CMake project; its bundled CMake and Ninja need no extra argument. The configure
reports the tree it picked:

```
-- CoralAudio third-party tree: /path/to/CoralAudio/third_party
```

If GoogleTest is missing, the configure does not stop: it prints a warning and builds the library
without the tests, so an incomplete IDE checkout can still be used. zlib is required to build the
codec module, so `third_party/libz` has to be present or `-DCORAL_SHARED_THIRD_PARTY_DIR` has to name
a tree that has it. Use `-DCORAL_BUILD_TESTS=OFF` in the CLion CMake profile when the tests are not
wanted at all.

## macOS

`AudioStreamCoreAudio` is the macOS backend. It uses one `kAudioUnitSubType_HALOutput` unit for both
directions:

- the unit's client side is set to the requested sample rate, channel count and PCM format, and
  Core Audio converts between that and the device format — including sample rate, so a 44.1 kHz
  file plays on a 48 kHz device without the device being reconfigured;
- callback mode runs the application callback directly on Core Audio's real-time HAL thread;
- blocking mode moves frames through the same `AudioRingBuffer` the other backends use;
- start/stop/reset are applied by a small control thread, because calling `AudioOutputUnitStop()`
  from inside the HAL callback can deadlock the HAL;
- a device-is-alive listener reports a removed device through the error callback;
- `CORAL_COREAUDIO_DEVICE=<name or UID>` selects a device instead of the system default.

The device buffer size is left at the value the system chose, so the stream does not change shared
device settings; the frames per burst it reports is that buffer size, and a larger
`FramesPerDataCallback` is split into one callback per buffer.

The platform stream test opens real devices and skips the sections a Mac cannot provide (a Mac mini
without an input device, for example):

```sh
./build/tests/tests_AudioStreamCoreAudio
CORAL_COREAUDIO_DEVICE="Mac mini Speakers" ./build/tests/tests_AudioStreamCoreAudio
```

## Examples

`examples/` is a separate project that links the framework through its public API. It is configured
on its own, or added by a top-level configure of this repository:

```sh
cmake -S examples -B build/examples -DCMAKE_BUILD_TYPE=Release
cmake --build build/examples
./build/examples/audio_engine_demo
```

The demo plays `assets/music/music.wav` from the repository root by default; pass a file and a
number of seconds to override both.

A project that consumes CoralAudio as a submodule does not get the example executable: the root
`CMakeLists.txt` adds `examples/` only when it is the top-level project.

See [examples/README.md](examples/README.md) for what the demo verifies.
