# Wii U build

This is an experimental homebrew target. It builds a Wii U RPX and WUHB bundle; it does not make
the Windows executable run on Wii U. The game runner reuses the current SDL2
host and expects the SDL2 port from devkitPro.

Install devkitPro, then run these in its MSYS2 terminal (or an existing MSYS2
with the devkitPro package repositories enabled):

```sh
pacman -Syu
pacman -S --needed cmake ninja wiiu-dev wiiu-sdl2 ppc-libpng ppc-libvorbis
```

From the repository root, configure and build with devkitPro's cross-CMake:

```sh
/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S . -B build-wiiu -DCMAKE_BUILD_TYPE=Release -DREGML_WIIU=ON -DBUILD_TESTING=OFF
cmake --build build-wiiu --config Release
```

The build produces `regml.rpx` and `regml.wuhb`. To launch it from the Wii U
Menu, use Aroma with the Homebrew on Menu plugin and copy `regml.wuhb` to
`sd:/wiiu/apps/reGML/`. Put the game package's `data.win` and external audio
files in `sd:/reGML/`; the Wii U build starts `sd:/reGML/data.win` automatically.
Saves go to `sd:/wiiu/apps/reGML/saves/`.

With Tiramisu, copy `regml.rpx` to `sd:/wiiu/apps/reGML/` and launch it through
the Homebrew Launcher instead. The WUHB/menu route requires Aroma.

Cemu 2.6 has been tested with Undertale's opening sequence and title-to-menu
transition. Real hardware and full game progression remain untested.
Performance is still poor in some scenes, especially particle-heavy effects.

For a runtime trace, create an empty `sd:/reGML/regml-trace.enabled` file.
The runner writes frame counts, VM instruction counts, room names, and frame-loop
stages to `sd:/reGML/regml-trace.log`. Remove the marker to disable tracing.

For scripted input, put `regml-input.txt` beside `data.win`. Each line contains a
frame number followed by held GML keyboard codes, using the same format as the
desktop `--input-script` option. A line with just a frame number releases all keys.
Remove the input file to return to controller input.
