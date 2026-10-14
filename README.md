# reGML

GameMaker VM runner in C++20

Bytecode 15 and 16, no YYC support

Tested with Undertale on Windows and with the Wii U build in Cemu 2.6
Linux builds are checked in CI

Compatibility is incomplete, no full playthrough verified
Wii U hardware is untested and particle effects are slow

## Build

Requires C++20, CMake 3.24+, libpng, zlib, libvorbisfile and SDL2

### Windows

```powershell
.\tools\build.ps1 -Configuration Release -Test
```

Dependencies default to the compiler's toolchain prefix
Use `-DependencyPrefix <path>` for PNG, Vorbis and zlib, and `-SdlPrefix <path>` for SDL2
Use `-NoVideo` for a headless build

### CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

For headless builds, add `-DREGML_SDL2=OFF` when configuring

The optional miniaudio output backend can be tried in a separate desktop build:

```sh
cmake -S . -B build/miniaudio -DREGML_MINIAUDIO=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/miniaudio --config Release
```

It only replaces the output device. Decoding and mixing still use `audio/sdl`; the default SDL output is unchanged. Wii and Wii U builds do not support this option.

OpenAL can also be selected for desktop audio output. Install OpenAL Soft, then configure with `-DREGML_OPENAL=ON`. It uses the same decoder and mixer as the default SDL output.

## Run

```powershell
.\build\direct\regml.exe run 'C:\path\to\data.win'
```

Keep external audio files in the same directory as `data.win`
Game files are not included

The PowerShell build outputs `build/direct/regml.exe`
CMake outputs `build/regml`, `build/regml.exe` or `build/Release/regml.exe`

For 4x speed:

```powershell
.\build\direct\regml.exe run 'C:\path\to\data.win' --speed 4x
```

`--speed` accepts values from `0.01` to `1000`, with an optional `x` suffix
Hidden and headless runs have no frame delay
Add `--debug-overlay` to show FPS, frame time, room, GML code and bytecode address

## Wii U

Install devkitPro and run these in its MSYS2 terminal
Run the build commands from the repository root

```sh
pacman -Syu
pacman -S --needed cmake ninja wiiu-dev wiiu-sdl2 ppc-libpng ppc-libvorbis

/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S . -B build-wiiu -DCMAKE_BUILD_TYPE=Release -DREGML_WIIU=ON -DBUILD_TESTING=OFF
cmake --build build-wiiu --config Release
```

Outputs: `build-wiiu/regml.rpx` and `build-wiiu/regml.wuhb`

### SD card

Put `data.win` and external audio files in `sd:/reGML/`

- Aroma: copy `regml.wuhb` to `sd:/wiiu/apps/reGML/` and launch with Homebrew on Menu
- Tiramisu: copy `regml.rpx` to `sd:/wiiu/apps/reGML/` and launch with the Homebrew Launcher

Saves: `sd:/wiiu/apps/reGML/saves/`

### Cemu

Tested with Cemu 2.6

1. Put `data.win` and external audio files in `sdcard/reGML/` inside the Cemu directory
2. Configure a Wii U GamePad in Input settings
3. Enable TV audio in General settings > Audio
4. Open `build-wiiu/regml.wuhb` through File > Load

| Controller | GML key |
| --- | --- |
| D-pad | Arrow keys |
| A | Z |
| B | X |
| X / Y | C |
| Start | Enter |

Trace logs and input scripts: [docs/wiiu.md](docs/wiiu.md)

## Wii (experimental)

The Wii build uses libogc for video, input, and audio, with direct GX rendering. The VM is shared with desktop and Wii U. SDL2 is not linked. Wii code is in `src/platform/wii/`.

[GX renderer](docs/wii-gx.md) ? [Optional 240p builds](docs/240p.md)

Install devkitPro and run these in its MSYS2 terminal:

```sh
pacman -S --needed cmake ninja wii-dev ppc-libpng ppc-libvorbis ppc-zlib

/opt/devkitpro/portlibs/wii/bin/powerpc-eabi-cmake -G Ninja -S . -B build/wii \
  -DCMAKE_BUILD_TYPE=Release -DREGML_WII=ON -DBUILD_TESTING=OFF
cmake --build build/wii --config Release
```

This produces `build/wii/regml.dol`. Copy it to `sd:/apps/reGML/boot.dol` and launch it from the Homebrew Channel. Put `data.win` and external audio files in `sd:/reGML/`.

For the debug HUD, put an empty `regml-debug.enabled` file beside `data.win`.

The DOL cross-builds; Wii hardware and Dolphin have not been tested. Rendering still uses the software framebuffer.

## Tools

```powershell
.\build\direct\regml.exe inspect 'C:\path\to\data.win' --code --strings
.\build\direct\regml.exe audit 'C:\path\to\data.win'
.\build\direct\regml.exe disasm 'C:\path\to\data.win' 0 --linked
```

## Tests

Undertale regression checks:

```powershell
.\tools\test-undertale.ps1 -DataFile 'C:\path\to\data.win'
```

Audio decoding, mixing and collision mask checks:

```powershell
cmake -S . -B build/audio-test -DREGML_UNDERTALE_DATA='C:\path\to\data.win'
cmake --build build/audio-test --config Release
ctest --test-dir build/audio-test --output-on-failure -C Release
```

Audio tests do not check playback through a physical output device

## Contributing

Include the game version, reproduction steps and error output in bug reports
Keep runtime fixes general to GameMaker
Build and run the relevant tests before submitting changes

Do not commit game files, extracted assets, saves or screenshots

## Screenshots

Undertale on desktop

<img src="https://raw.githubusercontent.com/TimGoTheCreator/timgothecreator.github.io/master/images/regml.png" alt="Undertale running in reGML on desktop" width="680">

Asriel encounter running in reGML on desktop

<img src="https://raw.githubusercontent.com/TimGoTheCreator/timgothecreator.github.io/master/images/asriel.png" alt="Asriel encounter running in reGML on desktop" width="680">

Undertale with the Wii U build in Cemu

<img src="https://raw.githubusercontent.com/TimGoTheCreator/timgothecreator.github.io/master/images/wiiu.png" alt="Undertale running in reGML's Wii U build in Cemu" width="680">
