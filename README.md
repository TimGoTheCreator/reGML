# reGML

GameMaker VM runner in C++20

Currently tested against Undertale, with incomplete compatibility and no verified full playthrough

Bring your own `data.win` and game assets

<img src="https://raw.githubusercontent.com/TimGoTheCreator/timgothecreator.github.io/master/images/regml.png" alt="Undertale running in reGML" width="680">

Undertale running in reGML

## Requirements

- C++20 compiler
- libpng
- libvorbisfile
- SDL2 for windowed builds
- CMake 3.24 or newer for CMake builds

The Windows script expects dependencies in the compiler's toolchain prefix

- `-SdlPrefix` to use SDL2 from another prefix
- `-DependencyPrefix` to use PNG, Vorbis, and zlib from another prefix
- `-NoVideo` for a headless build

## Build

Windows PowerShell:

```powershell
.\tools\build.ps1 -Configuration Release -Test
```

CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The experimental Wii U homebrew target is documented in [docs/wiiu.md](docs/wiiu.md).

Headless: `-DREGML_SDL2=OFF`

## Run


```powershell
.\build\direct\regml.exe run 'C:\path\to\data.win'
```

4x speed:

```powershell
.\build\direct\regml.exe run 'C:\path\to\data.win' --speed 4x
```

`--speed`: `0.01`–`1000`, fractions allowed, optional `x` suffix

Hidden and headless runs use no real-time pacing

Keep the game's external audio files alongside `data.win`

CMake output: `build/regml`, `build/regml.exe`, or `build/Release/regml.exe`, depending on platform and generator

## Inspect

```powershell
.\build\direct\regml.exe inspect 'C:\path\to\data.win' --code --strings
.\build\direct\regml.exe audit 'C:\path\to\data.win'
.\build\direct\regml.exe disasm 'C:\path\to\data.win' 0 --linked
```

## Test

Undertale checks:

```powershell
.\tools\test-undertale.ps1 -DataFile 'C:\path\to\data.win'
```

Audio and collision mask tests:

```powershell
cmake -S . -B build/audio-test -DREGML_UNDERTALE_DATA='C:\path\to\data.win'
cmake --build build/audio-test --config Release
ctest --test-dir build/audio-test --output-on-failure -C Release
```

Audio coverage: decoding and mixing only

## Contributing

- Include the game version, reproduction steps, and runner error in bug reports
- Keep fixes general to GameMaker
- Add regression tests where practical
- Build and run tests before submitting a PR
- No game files, extracted assets, saves, or screenshots in commits
