# CoralAudio examples

The framework's standalone demo and verification project. `audio_engine_demo` plays an audio file
through the realtime graph and checks the render while it plays:

```
AudioFile -> AudioPlayerNode -> mainMixerNode -> AudioOutputNode -> device
```

| Check | What it proves |
| --- | --- |
| node time vs. wall clock | the graph is pulled by the device at the right rate, not too fast or too slow |
| frames rendered vs. segment | no block was dropped or duplicated on the way to the output |
| wall-clock duration vs. audio duration | the segment reached the output in the time the audio takes |
| tap on the main mixer | the graph really rendered, and the tap ran on the engine's IO thread |
| device format | the stream reported the format the graph renders in |

The demo prints the file format, the graph format and the device format, then plays the requested
number of seconds (the whole file when the argument is `0`) and prints a verdict. A wrong rate, a
lost block or a tap that never left the installing thread makes the process exit with code 1.

## Running

```sh
./audio_engine_demo [file] [seconds]
```

`file` defaults to `assets/music/music.wav` in the repository root, which is the directory compiled
in as `CORAL_ASSETS_DIR`; pass a file to play another one. `seconds` defaults to 20.

## Building

As part of the framework, a top-level configure builds it with the repository:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/examples/audio_engine_demo assets/music/music.wav 5
```

On its own, which is what an IDE opens when working on the demo:

```sh
cmake -S examples -B build/examples -DCMAKE_BUILD_TYPE=Release
cmake --build build/examples
./build/examples/audio_engine_demo assets/music/music.wav 5
```

The standalone configure resolves CoralCore and CoralMedia from `external/` and adds the framework
the same way the application root does, so the submodules have to be initialized first (see the
header of the repository README).

The demo needs a real output device. `CORAL_COREAUDIO_DEVICE` selects one on macOS; the other
platform backends use their default device.
