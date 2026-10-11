# reGML

reGML is a C++20 implementation of a GameMaker bytecode runner. It loads a GameMaker package such as `data.win` and executes its bytecode. Compatibility is incomplete; Undertale is the current test target, and a full playthrough has not been verified.

## Requirements

- C++20 compiler
- libpng
- libvorbisfile
- pkg-config for CMake dependency discovery
- SDL2 for windowed builds
- CMake 3.24 or newer for CMake builds

On Windows, `tools/build.ps1` expects the compiler, PNG, and Vorbis libraries in a compatible toolchain prefix. SDL2 is expected there by default; use `-SdlPrefix` to specify another SDL2 prefix. Use `-NoVideo` for a headless build.

## Build

Windows PowerShell:

```powershell
.\tools\build.ps1 -Configuration Release -Test
```

For a CMake build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake requires SDL2 by default. Configure with `-DREGML_SDL2=OFF` for a headless build.

## Run

Pass the path to a GameMaker package to the runner:

```powershell
.\build\direct\regml.exe run 'C:\path\to\data.win'
```

To change real-time playback speed, pass any multiplier from `0.01x` to `1000x`:

```powershell
.\build\direct\regml.exe run 'C:\path\to\data.win' --speed 4x
```

Examples include `--speed 0.5x`, `--speed 1.5x`, `--speed 3x`, and `--speed 10x`; the `x` is optional. It changes the simulation rate. Audio sample rate is limited to the device-friendly 8–192 kHz range. Hidden and headless runs already run as fast as possible; `--frames` remains a count of simulation steps.

The CMake executable is `build/regml` (or `build/Release/regml.exe` with a multi-configuration generator).

Useful inspection commands:

```powershell
.\build\direct\regml.exe inspect 'C:\path\to\data.win' --code --strings
.\build\direct\regml.exe audit 'C:\path\to\data.win'
.\build\direct\regml.exe disasm 'C:\path\to\data.win' 0 --linked
```

The optional Undertale integration check requires a built test executable and a locally supplied game package:

```powershell
.\tools\test-undertale.ps1 -DataFile 'C:\path\to\data.win'
```

To check Undertale audio decoding and mixing with CTest, configure with the local `data.win` path:

```powershell
cmake -S . -B build/audio-test -DREGML_UNDERTALE_DATA='C:\path\to\data.win'
cmake --build build/audio-test --config Release
ctest --test-dir build/audio-test --output-on-failure -C Release
```

This audio test checks the intro track and dialogue sound in the decoder/mixer. It does not verify SDL output or speaker playback.

Do not put game packages, extracted assets, saves, or screenshots in source control.

## Contributing

- Open an issue for a bug or proposed change that needs discussion.
- Keep runtime behavior general to GameMaker where possible; avoid game-specific workarounds.
- Add or update a regression check for behavior changes.
- Run `tools/build.ps1 -Configuration Release -Test` or the CMake build and CTest commands before submitting.
- Include reproduction steps and relevant runner diagnostics in bug reports. Do not attach copyrighted game data or extracted assets.

## Current scope

The project includes package inspection, bytecode decoding and linking, a VM, a software renderer, an SDL2 host, and an experimental GameMaker runtime. Bytecode versions and built-ins are only partially supported. The runner reports unsupported operations with diagnostics. See the source and tests for the implemented behavior.
