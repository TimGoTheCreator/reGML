# Wii GX renderer

Wii game sessions use GX directly for the main screen. Desktop and Wii U still use the software renderer.

Sprites, backgrounds, tiles, and font glyphs become textured quads. Texture regions are converted to GX RGBA8 on first use and cached with an 8 MiB budget. Regions larger than 1024 pixels are split into smaller textures. Cache eviction waits for pending GPU reads before freeing memory.

Rectangles, triangles, lines, and circles use GX geometry. Circles currently use 96 segments. Their edges and line coverage can differ from the software rasterizer.

The main CPU pixel buffer and fullscreen upload texture are freed when GX takes over. Offscreen GameMaker surfaces remain software canvases; drawing a surface uploads it as a temporary texture. This path waits for GX before releasing that texture and can still be expensive.

`draw_getpixel`, screenshots, and frame hashes read the EFB. Readback waits for queued draws and is unsuitable for frequent full-screen captures. The screen uses RGB8; pixel reads report opaque alpha.

The fixture demo and headless runs still use software rendering. This initial GX implementation has been compiled for Wii, but visual parity and performance need testing in Dolphin and on hardware.
