# reGML

![Language](https://img.shields.io/badge/Language-C%2B%2B-blue)
![fluffy boys](https://img.shields.io/badge/fluffy%20boys-ralsei-pink)


A GameMaker bytecode runner and package inspector in C++

Package inspection reads v14-v17 layouts

Bytecode decoding and disassembly cover v15-v17
v17 is supported, but not every GameMaker feature is implemented yet (as in deltarune doesn't even show maps, and plays wrong SFX, so experiemental?)

Tested with UNDERTALE on Windows and the Wii U build in Cemu
No full playthrough is verified
PS2 builds are slow

## Build

Requires C++20, CMake 3.22+, libpng, zlib, libvorbisfile and SDL2

### CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

For a headless build, add `-DREGML_SDL2=OFF`

aswell the optional miniaudio can be tried:

```sh
cmake -S . -B build/miniaudio -DREGML_MINIAUDIO=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/miniaudio --config Release
```

(it only replaces the output device. decoding and mixing still uses audio/sdl. wii wii u and ps2 builds do not support this)

OpenAL can also be selected for desktop audio output: Install OpenAl (soft), then config with `-DREGML_OPENAL=ON`. 
the decode/mix story is same as SDL.

### Web / GX.games

Build with the Emscripten SDK installed and activated:

```sh
emcmake cmake -S . -B build/web -DREGML_WEB=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/web
```

Upload `index.html`, `index.js`, and `index.wasm` from `build/web` as a web game on GX.games. When it opens, choose the folder containing `data.win` and the game's external files. Save data is kept in the browser.

## PlayStation 4

The PS4 target uses OpenOrbis and its SDL2 port. Install OpenOrbis, clang, lld, and Ninja. Put PS4 builds of libpng 1.6, zlib, libvorbisfile, Vorbis, and Ogg in `REGML_PS4_DEPS_ROOT`. Add BZip2 there for games that use BZ2-QOI textures. Set `OO_PS4_TOOLCHAIN` to the OpenOrbis folder. `REGML_PS4_SDL2_ROOT` can point to a separate SDL-PS4 install; it defaults to `OO_PS4_TOOLCHAIN`.

```powershell
cmake -S . -B build/ps4 -G Ninja `
  --toolchain cmake/toolchains/ps4-openorbis.cmake `
  -DREGML_PS4=ON -DREGML_SDL2=ON `
  -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/ps4
```

This creates `build/ps4/regml.elf`, `build/ps4/regml.oelf`, and `build/ps4/eboot.bin`. The game package is loaded from `/app0/data.win`; save files go under `/data/reGML/saves`. The build creates the executable payload; it does not create a `.pkg` installer.

## Run

```sh
./build/regml run path/to/data.win
```

On Windows with the Visual Studio CMake generator:

```powershell
.\build\Release\regml.exe run 'C:\path\to\data.win'
```

Keep external files in the same folder as `data.win`
Game files aren't included

For speed:

```powershell
.\build\Release\regml.exe run 'C:\path\to\data.win' --speed 4x
```

`--speed` accepts values from `0.01` to `1000`, with an optional `x` suffix
Hidden and headless runs have no frame delay
Add `--debug-overlay` to show FPS, frame time, room, GML code and bytecode address

## Wii U

install devkitPro and run these commands in its msys2 terminal.

```sh
pacman -Syu
pacman -S --needed cmake ninja wiiu-dev wiiu-sdl2 ppc-libpng ppc-libvorbis

/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S . -B build-wiiu -DCMAKE_BUILD_TYPE=Release -DREGML_WIIU=ON -DBUILD_TESTING=OFF
cmake --build build-wiiu --config Release
```

Outputs: `build-wiiu/regml.rpx` and `build-wiiu/regml.wuhb`

### SD card

Put `data.win` and external audio files in `sd:/reGML/`

- Aroma: copy `regml.wuhb` to `sd:/wiiu/apps/reGML/` & launch with Homebrew on Menu
- Tiramisu: copy `regml.rpx` to `sd:/wiiu/apps/reGML/` & launch with the Homebrew Launcher

Saves: `sd:/wiiu/apps/reGML/saves/`

### Cemu

was tested with cemu v. 2.6

1. Put the `data.win` and external files in `sdcard/reGML/` inside the Cemu dir
2. Config a Wii U GamePad in Input settings
3. Enable TV audio in General settings > Audio
4. Open `build-wiiu/regml.wuhb` through File > Load

| Controller | GML key |
| --- | --- |
| D-pad | Arrow keys |
| A | Z |
| B | X |
| X / Y | C |
| Start | Enter |

The Trace logs and input scripts: [docs/wiiu.md](docs/wiiu.md)

## Wii (experimental)

wii build uses libogc for video, input, & audio, with direct GX render. the vm is shared with desktop and Wii U. sdl2 isn't linked:
wii code is in `src/platform/wii/`

[GX renderer](docs/wii-gx.md) ? [Optional 240p builds](docs/240p.md)

Install devkitPro and run these in its MSYS2 terminal:

```sh
pacman -S --needed cmake ninja wii-dev ppc-libpng ppc-libvorbis ppc-zlib

/opt/devkitpro/portlibs/wii/bin/powerpc-eabi-cmake -G Ninja -S . -B build/wii \
  -DCMAKE_BUILD_TYPE=Release -DREGML_WII=ON -DBUILD_TESTING=OFF
cmake --build build/wii --config Release
```

Outputs: `build/wii/regml.dol`
Copy it to `sd:/apps/reGML/boot.dol` and launch it from the Homebrew Channel. Put `data.win` and external audio files in `sd:/reGML/`.

For the debug HUD, put an empty `regml-debug.enabled` file beside `data.win`

wii hardware and Dolphin have not been tested. rendering still uses the software framebuffer.

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

### Some facts

The idea for reGML came from a simple thought: if GameMaker runs on a VM, then in theory, you can reimplement it and port any GameMaker game to any platform. 

Before this version, I actually tried making other runners (also named reGML, but blindly coded by antigravity). so, it failed miserably. quick lesson that "vibe coding" simply does not work if you don't know what you're doing. The AI gets stuck, generates trash, and leaves you with a broken mess. If you think AI is going to replace developers, you're either lying or you don't know what actual code looks like.

heh, maybe if you just give it a prompt and say "make it work fast no error pls", it might work, well, you know what they say, the [infinite monkey theorem](https://en.wikipedia.org/wiki/Infinite_monkey_theorem)

So hadn't i given up. I started using codex proper with a more "usable" approach, aka, actually nudging it. i still used (and still do) use it to generate code but debug it and atleast try to see what's wrong and why doesn't it launch?
well, thats when it started working.

current reGML isn't the "best" either. I know just enough C++ to be dangerous (who doesn't love a good `std::bad_alloc` on the PS2?), so some architectural decisions are questionable at best, harmful at worst. 
