# Silent Hill — Wii U Port

Native Wii U port of **Silent Hill (PS1)** built on top of the [Silent Hill matching decompilation](https://github.com/Vatuu/silent-hill-decomp) and [SlickAmogus's multi-platform port](https://github.com/SlickAmogus/silent-hill-decomp) (using [PsyCross](https://github.com/OpenDriver2/PsyCross)).

> **Legal notice:** *Silent Hill* is © Konami. This repository contains **no copyrighted game assets**. You must supply your own legally obtained PS1 *Silent Hill* (USA) `.bin` disc image to play.

## Features

- **Native GX2 renderer** (`wiiu_port/src/gpu_gx2.c`) with CafeGLSL shaders, palette-in-shader PSX VRAM textures, perspective-correct texturing (PGXP clip-space W), and 1920×1080 output (Hor+ 16:9 in gameplay, 1440×1080 for 4:3 screens).
- **Wii U GamePad integration** (`wiiu_port/src/gamepad_screen.c`, `gamepad_game.c`):
  - **Live Paper Map:** displays the current area's map with all markings and Harry's position/arrow (zoomable 1×–8× in **Options → GamePad Map Zoom**).
  - **Health & Ammo Strip:** subtle ECG trace at the top of the GamePad (with `CAUTION` / `DANGER` thresholds) plus equipped gun ammo. Tap the strip to hide it for the original hidden-health experience.
  - **Radio Static via GamePad Speaker:** routes the pocket radio's audio directly to the GamePad speaker (`radio_gamepad` in `silenthill.cfg`: `0` = TV, `1` = GamePad [default], `2` = both).
  - **Off-TV Play:** tap **PAD** (top right of the GamePad) to move gameplay to the GamePad and blank the TV; tap **TV** to switch back.
  - **Rumble & Achievement Pop-ups:** drives the GamePad vibration motor (including the low-health heartbeat) and shows achievement toasts on the GamePad.
- **Difficulty & Quality-of-Life Enhancements:** Easy / Modern / Normal / Hard difficulties, staircase/boss autosaves, memory card backups, and key-item glint.

## Running (Wii U / Cemu)

Place your PS1 *Silent Hill* `.bin` disc image (any `*.bin` filename works) in one of these SD card folders (searched in order):

1. `sd:/silenthill/`
2. `sd:/wiiu/apps/silenthill/`
3. `sd:/`

Launch `silenthill_gx2.rpx` via Aroma / Homebrew Launcher (or open it in Cemu).

The disc image's folder also stores:
- **`save/`** — memory card saves and backups.
- **`silenthill_NNN.log`** — rolling debug logs (last 20 kept).
- **`silenthill.cfg`** — optional configuration overrides (e.g., `minimap = 1`, `radio_gamepad = 1`, `gamepad_map_zoom = 4`).
- **`gputest`** — create an empty file with this name to display the renderer self-test image on boot.

### Controls

Buttons are mapped by physical position to match the PlayStation layout:

| PlayStation | Wii U GamePad / Pro Controller |
|---|---|
| Cross / Circle / Square / Triangle | B / A / Y / X |
| L1 / R1 | L / R |
| L2 / R2 | ZL / ZR |
| Select / Start | − / + |
| Movement | Left Stick / D-Pad |
| Camera | Right Stick |

## Building from Source

Requires **devkitPro** (`devkitPPC`, `wut`, `wiiu-sdl2`, `ppc-zlib`, etc.) on Linux or WSL.

```bash
# 1. Clone the wiiu-port branch
git clone -b wiiu-port https://github.com/comaro-sudo/silent-hill-decomp.git
cd silent-hill-decomp

# 2. Download CafeGLSL (once, for the native GX2 renderer)
bash wiiu_port/fetch_cafeglsl.sh

# 3. Build the Wii U .rpx
bash wiiu_port/build.sh
```

Output: `wiiu_port/bin/silenthill_gx2.rpx`

### Useful build options

- `WIIU_RENDERER=sdl bash wiiu_port/build.sh` — build the SDL2 fallback renderer (`wiiu_port/bin/silenthill.rpx`, does not require CafeGLSL).
- `bash wiiu_port/gate.sh` — compile game + HAL objects only.
- `SKIP_GATE=1 SKIP_MAPS=1 bash wiiu_port/build.sh` — relink only.

## Project Layout

| Path | Purpose |
|---|---|
| `wiiu_port/` | Wii U HAL (GX2 & SDL2 renderers, GamePad screen, VPAD/KPAD input, AX/SDL audio, SD filesystem, entry point, and build scripts) |
| `src/`, `include/` | Decompiled Silent Hill (PS1) game code and headers |
| `pc_port/` | Shared PC port runtime, PsyCross compatibility layer, FMV decoder, and extracted map data |
| `xbox_port/`, `xbox360_port/` | Shared console memory/GPU/CD/memory-card layers and 32-bit big-endian PowerPC fixes |

## Status

- Boots and plays through the intro FMV, title screen, attract demos, and gameplay in **Cemu** with full GamePad features.
- **Not yet tested on real Wii U hardware** (in particular, GamePad touch coordinate calibration assumes 1280×720 space and should be verified on console).
