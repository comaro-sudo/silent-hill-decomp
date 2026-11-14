# Silent Hill: GameCube, Wii and Wii U ports

Branch `ogc-port`, cut from `xbox360-port`. The 360 branch already makes the shared game code work on a 32-bit big-endian PowerPC: the endian swappers, the type-punned store fixes and the bitfield reversals. All three Nintendo consoles have that CPU model, so they reuse it unchanged and only add a HAL.

| | GameCube | Wii | Wii U (native) |
|---|---|---|---|
| Output | `bin/cube/silenthill.dol` | `bin/wii/silenthill.dol` | `bin/wiiu/silenthill.rpx` |
| Libraries | libogc 3.2 + libfat | libogc 3.2 + libfat | wut 1.9 + SDL2 2.32 |
| HAL | `ogc_port/src` | `ogc_port/src` | `wiiu_port/src` + the platform-neutral files in `ogc_port/src` |
| Defines | `SH_OGC_PORT SH_GC_PORT` | `SH_OGC_PORT SH_WII_PORT` | `SH_WIIU_PORT` |
| Video | 640×480, 4:3 | 640×480 (480p with a component cable); anamorphic 16:9 if the console is set to 16:9 | 1920×1080; Hor+ 16:9 in game, 4:3 screens at 1440×1080 |
| Renderer | GX (`gpu_gx.c`) | GX (`gpu_gx.c`) | SDL2 `SDL_RenderGeometry` on GX2 (`gpu_sdl_wiiu.c`) |

All three builds also define `SH_XBOX360_PORT SH_XBOX_PORT SH_PC_PORT`, layered the way the 360 build layers them: the big-endian fixes, the console memory behaviour and the PC game code.

**Status (2026-11-11):** all three compile with no errors (210 files plus 42 map archives) and link. **None has been run yet**: no Dolphin or Cemu on the build machine, and no hardware test.

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

GameCube memory: Dolphin's 24 MB leaves about 8 MB of heap after the 9.6 MB BSS (voice PCM cut to 16K samples, PSX-VRAM caches cut to the N64 sizes under `SH_GC_PORT`). There is no memory card in a disc-only setup, so saves are unavailable.

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

## Known gaps

- Nothing has been run. Before anything else, boot each build in Dolphin or Cemu and read the log on the SD card.
- **GameCube memory:** the static image is about 17 MB of the 24 MB. Whether the game's runtime allocations fit in the remaining ~7 MB is untested.
  - Candidates to trim: the voice PCM cache (3 MB, `audio_xbox.c`), the frame readback buffer (573 KB, unused on GX) and the unwind tables.
- **Wii U renderer:**
  - Affine texturing only (SDL vertices are 2D).
  - Fog is added only to opaque primitives.
  - No freeze-frame capture (the pause background).
  - Native GX2 with real shaders needs a shader toolchain, which isn't installed.
- **Video playback:** FMV playback (`fmv_xbox.c`) and XA audio are compiled in as on the 360 XDK build, but not tested here.
- **Wii U GamePad:** see the next section. Two plan items are missing: radio static through the GamePad speaker, and a live map.

## Wii U GamePad

The TV and the GamePad are separate SDL windows (`SDL_WINDOW_WIIU_TV_ONLY` and `SDL_WINDOW_WIIU_GAMEPAD_ONLY`). The code is `wiiu_port/src/gamepad_screen.c`; the game values it reads come from `gamepad_game.c`.

- **Health strip** along the top during play.
  - Uses the inventory status display's thresholds and colours (`Gfx_Inventory_HealthStatusDraw`): DANGER below 10 (red), CAUTION below 50 (orange), FINE (yellow-green to green).
  - An ECG trace whose beat speeds up as health drops. No number, as in the original.
  - **Tap the strip to hide it**, for the original hidden-health experience. The choice is saved as an empty `gamepad_nohealth` file next to the disc image.
- **Last-viewed map:**
  - While the paper map screen is open, the TV image is copied to the GamePad every half second.
  - The GamePad then keeps the map as last seen, with its markings and Harry's marker.
  - It's a snapshot, not a live map: the game draws the map only inside the map screen.
- **Off-TV play:** tap **PAD** (top right) to move the game to the GamePad and black out the TV; tap **TV** to move it back.
  - The page texture cache is dropped on each switch, since textures belong to one renderer.
  - Vsync moves to the screen that shows the game.
- **Rumble:** the game's own vibration calls drive the GamePad motor, including the PS1's low-health heartbeat. Strength becomes the density of the motor's on/off pattern.

**Not done:**
- **Radio static through the GamePad speaker.** The software SPU mixes everything into one stream. This needs the radio's SPU voices told apart from the rest, plus an AX voice routed to the GamePad only.
- **Live map.** The paper map would have to be drawn outside the map screen.
- **Uncertain:** the touch coordinates assume wut's calibrated point is in 1280×720 space. Check this on hardware.
