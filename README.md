<h1 align="center">reGML</h1>

<p align="center">
  <a href="https://github.com/TimGoTheCreator/reGML/issues"><img src="https://img.shields.io/badge/reGML-issues_%2F_compatibility-green"></a>
  <a href="https://discord.gg/RsEypMzs6b"><img src="https://img.shields.io/discord/1563248164725596262?color=5865F2&logo=discord&logoColor=white&label=discord"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C%2B%2B-blue" alt="Language: C++">
  <img src="https://img.shields.io/badge/fluffy%20boys-ralsei-pink" alt="fluffy boys: ralsei">
</p>

A GameMaker bytecode runner and package inspector in C++.

Package inspection reads v14-v17 layouts.

Decoding & disasm cover v15-v17.
v17 is supported, but not every GameMaker feature is implemented yet. DELTARUNE has been tried.

### Screenshots

| Desktop | Wii U (Cemu) |
| --- | --- |
| <img src="https://raw.githubusercontent.com/TimGoTheCreator/timgothecreator.github.io/master/images/regml.png" alt="Undertale on desktop" width="420"> | <img src="https://raw.githubusercontent.com/TimGoTheCreator/timgothecreator.github.io/master/images/wiiu.png" alt="Undertale on Wii U via Cemu" width="420"> |
| **Desktop (Asriel)** | **Deltarune (Cemu / Wii U)** |
| <img src="https://raw.githubusercontent.com/TimGoTheCreator/timgothecreator.github.io/master/images/asriel.png" alt="Asriel on desktop" width="420"> | <img src="https://raw.githubusercontent.com/TimGoTheCreator/timgothecreator.github.io/master/images/king.png" alt="Deltarune King fight on Wii U via Cemu" width="420"> |

---

## Build

Requires C++20, CMake 3.22+, libpng, zlib, libvorbisfile, and SDL2.

### CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

For a headless build, add `-DREGML_SDL2=OFF`.

As well, the optional miniaudio can be tried:

```sh
cmake -S . -B build/miniaudio -DREGML_MINIAUDIO=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/miniaudio --config Release
```

(It only replaces the output device. Decoding and mixing still use audio/SDL. Wii, Wii U, and PS2 builds do not support this.)

OpenAL can also be selected for desktop audio output: Install OpenAL-Soft, then configure with `-DREGML_OPENAL=ON`. 
The decode/mix story is the same as SDL.

### Web / GX.games

Build with the Emscripten SDK installed and activated:

```sh
emcmake cmake -S . -B build/web -DREGML_WEB=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/web
```

Once the build finishes, upload index.html, index.js, & index.wasm from build/web as a web game on GX.games. When it opens, pick the folder that has data.win and the game's external files. (Saves are in the browser!!!)

## PlayStation 4

The PS4 target uses OpenOrbis and its SDL2 port. Install OpenOrbis, clang, lld, and Ninja. Put PS4 builds of libpng 1.6, zlib, libvorbisfile, Vorbis, and Ogg in the folder you set in REGML_PS4_DEPS_ROOT. If the game uses BZ2-QOI textures, add BZip2 there too. Set OO_PS4_TOOLCHAIN to the OpenOrbis folder. REGML_PS4_SDL2_ROOT can point to a separate SDL-PS4 install, and defaults to OO_PS4_TOOLCHAIN.


```powershell
cmake -S . -B build/ps4 -G Ninja `
  --toolchain cmake/toolchains/ps4-openorbis.cmake `
  -DREGML_PS4=ON -DREGML_SDL2=ON `
  -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/ps4
```

That creates build/ps4/regml.elf, build/ps4/regml.oelf, and build/ps4/eboot.bin. The game pkg loads from /app0/data.win and saves go under /data/reGML/saves. This build only makes the executable payload. However, it doesn't create a .pkg installer.


## Run

```sh
./build/regml run path/to/data.win
```

On Windows with the Visual Studio CMake generator:

```powershell
.\build\Release\regml.exe run 'C:\path\to\data.win'
```

Keep external files in the same folder as `data.win`.
Game files aren't included.

For speed:

```powershell
.\build\Release\regml.exe run 'C:\path\to\data.win' --speed 4x
```

`--speed` accepts values from `0.01` to `1000`, with an optional `x` suffix.
Hidden and headless runs have no frame delay.
Add `--debug-overlay` to show FPS, frame time, room, GML code, and bytecode address.

## Wii U

Install devkitPro and run these commands in its MSYS2 terminal.

```sh
pacman -Syu
pacman -S --needed cmake ninja wiiu-dev wiiu-sdl2 ppc-libpng ppc-libvorbis

/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S . -B build-wiiu -DCMAKE_BUILD_TYPE=Release -DREGML_WIIU=ON -DBUILD_TESTING=OFF
cmake --build build-wiiu --config Release
```

Outputs: `build-wiiu/regml.rpx` and `build-wiiu/regml.wuhb`

### SD card

Put `data.win` and external audio files in `sd:/reGML/`.

- Aroma: copy `regml.wuhb` to `sd:/wiiu/apps/reGML/` & launch with Homebrew on Menu.
- Tiramisu: copy `regml.rpx` to `sd:/wiiu/apps/reGML/` & launch with the Homebrew Launcher.

Saves: `sd:/wiiu/apps/reGML/saves/`

### Cemu

Was tested with Cemu v2.6.

1. Put the `data.win` and external files in `sdcard/reGML/` inside the Cemu dir.
2. Configure a Wii U GamePad in Input settings.
3. Enable TV audio in General settings > Audio.
4. Open `build-wiiu/regml.wuhb` through File > Load.

| Controller | GML key |
| --- | --- |
| D-pad | Arrow keys |
| A | Z |
| B | X |
| X / Y | C |
| Start | Enter |

The Trace logs and input scripts: [docs/wiiu.md](docs/wiiu.md)

## Wii

The Wii build uses libogc for video, input, & audio, with direct GX render. The VM is shared with desktop and Wii U. SDL2 isn't linked:
Wii code is in `src/platform/wii/`.

[GX renderer](docs/wii-gx.md) ? [Optional 240p builds](docs/240p.md)

Install devkitPro and run these in its MSYS2 terminal:

```sh
pacman -S --needed cmake ninja wii-dev ppc-libpng ppc-libvorbis ppc-zlib

/opt/devkitpro/portlibs/wii/bin/powerpc-eabi-cmake -G Ninja -S . -B build/wii \
  -DCMAKE_BUILD_TYPE=Release -DREGML_WII=ON -DBUILD_TESTING=OFF
cmake --build build/wii --config Release
```

Outputs: `build/wii/regml.dol`.
Copy it to `sd:/apps/reGML/boot.dol` and launch it from the Homebrew Channel. Put `data.win` and external audio files in `sd:/reGML/`.

For the debug HUD, put an empty `regml-debug.enabled` file beside `data.win`.

Wii hardware and Dolphin have not been tested. Rendering still uses the software framebuffer.

## Tools

```powershell
.\build\Release\regml.exe inspect 'C:\path\to\data.win' --code --strings
.\build\Release\regml.exe audit 'C:\path\to\data.win'
.\build\Release\regml.exe disasm 'C:\path\to\data.win' 0 --linked
```

## Tests

Undertale regression checks:

```powershell
.\tools\test-undertale.ps1 -DataFile 'C:\path\to\data.win'
```

Audio decoding, mixing, and collision mask checks:

```powershell
cmake -S . -B build/audio-test -DREGML_UNDERTALE_DATA='C:\path\to\data.win'
cmake --build build/audio-test --config Release
ctest --test-dir build/audio-test --output-on-failure -C Release
```

Audio tests do not check playback through a physical output device.

## Contributing

Include the game version, reproduction steps, and error output in bug reports.
Keep runtime fixes general to GameMaker.
Build and run the relevant tests before submitting changes.

Do not commit game files, extracted assets, saves, or screenshots.
