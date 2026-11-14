# Silent Hill: GameCube, Wii and Wii U ports

Branch `ogc-port`, cut from `xbox360-port`. The 360 branch already makes the shared game code work on a 32-bit big-endian PowerPC: the endian swappers, the type-punned store fixes and the bitfield reversals. All three Nintendo consoles have that CPU model, so they reuse it unchanged and only add a HAL.

| | GameCube | Wii | Wii U (native) |
|---|---|---|---|
| Output | `bin/cube/silenthill.dol` | `bin/wii/silenthill.dol` | `bin/wiiu/silenthill.rpx` |
| Libraries | libogc 3.2 + libfat | libogc 3.2 + libfat | wut 1.9 + SDL2 2.32 |
| HAL | `ogc_port/src` | `ogc_port/src` | `wiiu_port/src` + the platform-neutral files in `ogc_port/src` |
| Defines | `SH_OGC_PORT SH_GC_PORT` | `SH_OGC_PORT SH_WII_PORT` | `SH_WIIU_PORT` |
| Video | 640×480, 4:3 | 640×480 (480p with a component cable); anamorphic 16:9 if the console is set to 16:9 | 1920×1080; Hor+ 16:9 in game, 4:3 screens at 1440×1080 |
| Renderer | GX (`gpu_gx.c`) | GX (`gpu_gx.c`) | native GX2 + CafeGLSL shaders (`gpu_gx2.c`; `WIIU_RENDERER=sdl` builds the SDL2 `gpu_sdl_wiiu.c` fallback) |

All three builds also define `SH_XBOX360_PORT SH_XBOX_PORT SH_PC_PORT`, layered the way the 360 build layers them: the big-endian fixes, the console memory behaviour and the PC game code.

**Status (2026-11-14):** all three build and run in emulators (Dolphin for GameCube/Wii, Cemu for Wii U). None has been tested on hardware.
- **Wii U (GX2):** boots, plays the intro FMV to the end, the title, both attract demos and new-game play. The GamePad features below run in Cemu.
- **GameCube:** title to the Cheryl chase at about 10–16 fps.

## Building

The build runs in WSL (Ubuntu, aarch64), with devkitPPC r51 (gcc 16.2), libogc, libfat-ogc, wut and wiiu-sdl2 under `/opt/devkitpro`.

pkg.devkitpro.org refuses this machine (HTTP 403), so the toolchain came from elsewhere:
- **Packages:** the leseratte10 archive mirror.
- **newlib:** built from sourceware's newlib 4.6.0.20260123 with devkitPro's r51 patch and configure flags.
- **Linker scripts:** devkitppc-crtls v2.1.0 from GitHub. libogc 3 needs these newer scripts; v1.0.0 is too old.

```sh
PLATFORM=wii  bash ogc_port/build_ogc.sh   # compile gate + maps + link + elf2dol
PLATFORM=cube bash ogc_port/build_ogc.sh
PLATFORM=wiiu bash ogc_port/build_ogc.sh   # ... + elf2rpl
PLATFORM=wii  bash ogc_port/ogc_gate.sh    # compile only
SKIP_GATE=1 SKIP_MAPS=1 ...                # relink only
```

Every build compiles with one shared flag set, `ogc_flags.sh`. The gate, the map script and the link all source it.

## Running

Put the disc image (`*.bin`; any name works, the port scans for it) in the first of these folders that exists:

| Console | Folders searched | Loader |
|---|---|---|
| Wii | `sd:/silenthill/`, `usb:/silenthill/`, `sd:/apps/silenthill/`, `sd:/`, `usb:/` | Homebrew Channel (`apps/silenthill/boot.dol`) |
| GameCube | `sd2:/silenthill/` (SD2SP2), `carda:` / `cardb:` (SD Gecko) | Swiss |
| Wii U | `sd:/silenthill/`, `sd:/wiiu/apps/silenthill/`, `sd:/` | Aroma / Homebrew Launcher |

The disc image's folder also holds:
- **Saves:** written to `save/`.
- **Logs:** `silenthill_NNN.log`; the last 20 are kept.
- **Settings:** an optional `silenthill.cfg`.
- **Test picture:** an empty file named `gputest` makes the port show the renderer self-test picture before the game starts.

The Wii build also runs on a Wii U in Wii mode. The native Wii U build exists for 1080p output and the GamePad.

### GameCube disc (Dolphin)

Dolphin cannot emulate an SD adapter, so the GameCube build can also read the PS1 `.bin` from the DVD it booted from (used when no SD volume holds one; the SD path stays first). `ogc_port/src/dvd_gc.c` parses the disc FST, finds the largest `*.bin` and exposes it as a `FILE*` (`funopen` over `DVD_ReadPrio`), so `cd_xbox360.c` is unchanged apart from one hook. With no writable device the log goes to the OSReport UART (`SYS_Report`), visible in Dolphin's log with the OSREPORT channel on.

```sh
bash ogc_port/tools/build_apploader.sh                       # own apploader, no Nintendo code (WSL)
python3 ogc_port/tools/mkgcm.py --dol ogc_port/bin/cube/silenthill.dol     --bin "Silent Hill (v1.1).bin" --apploader ogc_port/tools/apploader.bin --out silenthill.gcm
dolphin -b -e silenthill.gcm
```

`apploader_stub.c` implements only the apploader contract (init/main/close): it streams the DOL sections and returns the entry point. Dolphin boots it. Never commit the resulting image (it contains the game).

GameCube memory: Dolphin's 24 MB leaves about 8 MB of heap after the 9.6 MB BSS. Under `SH_GC_PORT` the heap budget is: voice PCM cut to 16K samples, PSX-VRAM caches at the N64 sizes (4 index pages, ARGB cache floor of 2 slots), 384 KB packet arenas (x2) instead of 2 MB, a 16K-entry PGXP shadow table (384 KB, not 3 MB), 3 RGBA8 + 12 CI8 GX pages, at most 10 owned IPD chunk slots, and movies (FMV) are skipped because their decoder buffers (1.3 MB, never freed) no longer fit. With no writable volume the memory card is RAM-backed (a card that reports "not connected" wedges the KCET-logo card check forever), so saves last for the session only. Build with `OGC_ALLOC_TRACE=1` to log every 32 KB+ allocation and every failed one (`[ALLOC]` lines).

Unattended Dolphin runs: `mkgcm.py --extra autoplay.txt=<file>` puts a script in the disc root, and `autoplay.c` reads it from the DVD when there is no SD volume.

### Controls

The buttons are mapped by position, so each one sits where the PlayStation's would.

| PSX | GameCube pad | Classic / Pro Controller, GamePad | Wii Remote + Nunchuk |
|---|---|---|---|
| Cross / Circle / Square / Triangle | A / B / X / Y | B / A / Y / X | A / 2 / C / 1 |
| L1 / R1 | L / R, full click | L / R | Z / B |
| L2 / R2 | L / R, half press | ZL / ZR | – |
| Select / Start | Z / Start | − / + | − / + |
| Movement | stick, d-pad | left stick, d-pad | Nunchuk stick, d-pad |
| Camera | C stick | right stick | – |

## How the renderer maps the 360 shaders

The PS1 primitive decoder and VRAM decoder (`xbox_port/src/gpu_xbox.c`, `psx_vram.c`) are shared and unchanged. Each console supplies only the `GpuNv2a_*` backend.

- **GX:** two TEV stages: texture × vertex colour, plus the fog colour carried as a second vertex colour.
  - **Alpha:** ABR0 takes the texture's alpha; every other mode outputs a constant where the texture's alpha is above 0, using a TEV compare.
  - **Alpha test:** kills the transparent texels.
  - **Perspective:** a perspective projection fed `(x·w, y·w, −w)` reproduces the 360 vertex shader exactly.
  - **Textures:** RGBA8 tiles in a cache of 40 pages on Wii and 12 on GameCube.
- **Wii U:** `SDL_RenderGeometry`.
  - **Pages:** each page is uploaded in an "STP" form (alpha as the game decoded it) and a "solid" form (any visible texel fully opaque), so alpha blending also does the alpha test.
  - **Blend modes:** B−F uses reverse-subtract blending, B+F and B+F/4 use additive blending.
  - **Fog:** a second additive pass, on opaque primitives only.

## Wii U GX2 renderer (`wiiu_port/src/gpu_gx2.c`)

- Setup: `bash wiiu_port/fetch_cafeglsl.sh` once (CafeGLSL 1.0.0-rc1 static library, compiled into the .rpx; no .rpl on the SD card), then `PLATFORM=wiiu bash ogc_port/build_ogc.sh` -> `silenthill_gx2.rpx`. `WIIU_RENDERER=sdl` -> `silenthill.rpx`.
- One pixel shader: `rgb = tex*col + spec`, ABR0 alpha 0/0.5/1, discard on transparent texels; clip-space W from PGXP for perspective. About 110-230 draws a frame in town (SDL: 2,500-5,000), flushed only by blend/scissor changes or a new oversize texture.
- Textures: PSX pages as an R8 index atlas plus a palette texture, looked up in the shader (one 64 KB upload per page, any CLUT free), instead of an ARGB atlas copy per page+CLUT pair and alpha variant.
- Both screens are GX2 (no SDL video; SDL stays for audio). Vertex attributes are all float: 8-bit UNORM attributes made the ARM64 Cemu (Adreno Vulkan) reject every pipeline (Error -13).
- Debug: `gx2_vs.glsl` / `gx2_ps.glsl` next to the disc image override the shaders.

## Known gaps

- No hardware test on any of the three.
- **GameCube memory:** runs in the 24 MB with 576 KB packet arenas and the top 64 KB of MEM1 kept out of the heap; chunks stream like the PSX. FMV is skipped and saves are RAM-only.
- **Wii U SDL renderer** (`WIIU_RENDERER=sdl`, the fallback): affine texturing only, fog on opaque primitives only, no pause freeze-frame. The GX2 build has none of these limits.

## Wii U GamePad

The TV and the GamePad are separate SDL windows (`SDL_WINDOW_WIIU_TV_ONLY` and `SDL_WINDOW_WIIU_GAMEPAD_ONLY`). The code is `wiiu_port/src/gamepad_screen.c`; the game values it reads come from `gamepad_game.c`.

- **Health strip** along the top during play, deliberately quiet: a thin, dim ECG trace whose beat speeds up as health drops, and a small CAUTION or DANGER label at the inventory's thresholds (`Gfx_Inventory_HealthStatusDraw`). No word when healthy and no number, as in the original.
  - **Tap the strip to hide it**, for the original hidden-health experience. The choice is saved as an empty `gamepad_nohealth` file next to the disc image.
  - **Ammo** of the equipped gun on the right, loaded/magazine (`14/15`). Not shown for melee weapons or the Hyper Blaster.
- **Live map** below the strip: the area's paper map with every marking the map screen would show (the game's own marking tables and event flags, composed in `pc_minimap.c`), and Harry's arrow. It follows Harry and appears once he has found that area's map.
  - Zoom: **Options > GamePad Map Zoom**, 1×–8×, default 4× (`gamepad_map_zoom`). The zoom keeps the map's 4:3 proportions; it scrolls on whichever axes no longer fit.
  - The TV minimap is off by default on Wii U for this reason; `minimap = 1` in the cfg turns it back on.
  - Areas without a live map fall back to a snapshot taken while the map screen is open.
- **Achievement pop-ups** show on the GamePad instead of the TV. In Off-TV play they show over the game as on other ports.
- **Radio static** plays through the GamePad speaker only (`radio_gamepad`: 0 = TV, 1 = GamePad (default), 2 = both). The radio's two SPU voices are split off the software mix into an AX voice routed to the GamePad.
- **Off-TV play:** tap **PAD** (top right) to move the game to the GamePad and black out the TV; tap **TV** to move it back.
  - The page texture cache is dropped on each switch, since textures belong to one renderer.
  - Vsync moves to the screen that shows the game.
- **Rumble:** the game's own vibration calls drive the GamePad motor, including the PS1's low-health heartbeat. Strength becomes the density of the motor's on/off pattern.

**Uncertain:** the touch coordinates assume wut's calibrated point is in 1280×720 space. Check this on hardware.
