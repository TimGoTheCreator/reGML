# Wii GX renderer

Wii game sessions use GX for the main screen, desktop and Wii U still use the software renderer

Sprites, backgrounds, tiles and font glyphs become textured quads
Texture regions convert to GX RGBA8 on first use and get cached with an 8 MiB budget
Regions over 1024 pixels are split into smaller textures
Cache eviction waits for pending GPU reads before freeing memory

Rectangles, triangles, lines and circles use GX geometry
Circles use 96 segments
Edges and line coverage can differ from the software renderer

The main CPU pixel buffer and fullscreen upload texture are freed when GX takes over
Offscreen GameMaker surfaces still use software canvases
Drawing a surface uploads it as a temporary texture, GX waits before freeing it, this can be slow

`draw_getpixel`, screenshots and frame hashes read the EFB
Readback waits for queued draws, so frequent full-screen captures are slow
The screen uses RGB8, pixel reads report opaque alpha

The fixture demo and headless runs still use software rendering
GX builds for Wii, visual parity and performance still need testing in Dolphin and on hardware
