# Wii U

Experimental, tested in Cemu 2.6 with Undertale
Real hardware and a full playthrough are untested
Particle effects are still slow

## Build

Requires devkitPro with Wii U support
In its MSYS2 terminal:

```sh
pacman -Syu
pacman -S --needed cmake ninja wiiu-dev wiiu-sdl2 ppc-libpng ppc-libvorbis
```

From the repository root:

```sh
/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S . -B build-wiiu -DCMAKE_BUILD_TYPE=Release -DREGML_WIIU=ON -DBUILD_TESTING=OFF
cmake --build build-wiiu --config Release
```

Outputs: `build-wiiu/regml.rpx` and `build-wiiu/regml.wuhb`

## Files

| File | SD card path |
| --- | --- |
| `data.win` and external audio | `sd:/reGML/` |
| `regml.wuhb` or `regml.rpx` | `sd:/wiiu/apps/reGML/` |
| Saves | `sd:/wiiu/apps/reGML/saves/` |

The default game path is `sd:/reGML/data.win`

Aroma: launch the WUHB through Homebrew on Menu

Tiramisu: launch the RPX through the Homebrew Launcher

## Cemu

Put the game files in `sdcard/reGML/` inside the Cemu directory
Configure a Wii U GamePad in Input settings and enable TV audio in General settings > Audio
Load `build-wiiu/regml.wuhb` through File > Load

D-pad maps to arrow keys, A to Z, B to X, X/Y to C, Start to Enter

## Debugging

Create an empty `regml-trace.enabled` beside `data.win` to enable logging
The output is `regml-trace.log` in the same directory, with frame counts, instruction counts, room names and loop stages
Remove the marker when finished; logging flushes to disk at each stage

For an input replay, put `regml-input.txt` beside `data.win`
Each line is a frame number followed by held GML key codes

```text
30 90
31
120 90
121
```

This holds Z on frames 30 and 120, then releases it on the following frames
A frame number with no keys releases everything
The format is the same as desktop `--input-script`
Remove the file to use controller input again
