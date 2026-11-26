# Silent Hill — Wii U Port

**Language / Idioma:** **English** | [Español](README.es.md)

Native Wii U port of **Silent Hill (PS1)** built on top of the [Silent Hill matching decompilation](https://github.com/Vatuu/silent-hill-decomp) and [SlickAmogus's multi-platform port](https://github.com/SlickAmogus/silent-hill-decomp) (using [PsyCross](https://github.com/OpenDriver2/PsyCross)).

> **Legal notice:** *Silent Hill* is © Konami. This repository contains **no copyrighted game assets**. You must provide your own legally obtained PS1 *Silent Hill* (USA) `.bin` disc image to play.

---

## Features

- **Native GX2 Renderer** (`wiiu_port/src/gpu_gx2.c`): CafeGLSL shaders, palette-in-shader PSX VRAM textures, perspective-correct texturing (PGXP clip-space W), and **1920×1080** output (Hor+ 16:9 in gameplay, 1440×1080 on 4:3 screens).
- **Wii U GamePad Integration** (`wiiu_port/src/gamepad_screen.c`, `gamepad_game.c`):
  - **Live Paper Map:** shows the current area's paper map with all markings and Harry's arrow once found (zoomable 1×–8× via **Options → GamePad Map Zoom**).
  - **Health & Ammo Strip:** subtle ECG trace along the top of the GamePad (`CAUTION` / `DANGER` thresholds) plus equipped gun ammo. Tap the strip to hide it for the original hidden-health experience.
  - **GamePad Speaker Radio:** plays the pocket radio static directly through the GamePad speaker (`radio_gamepad` in `silenthill.cfg`: `0` = TV, `1` = GamePad [default], `2` = both).
  - **Off-TV Play:** tap **PAD** (top right of the GamePad) to move gameplay to the GamePad and blank the TV; tap **TV** to switch back.
  - **Rumble & Achievement Pop-ups:** drives the GamePad rumble motor (including the low-health heartbeat) and shows achievement notifications on the GamePad.
- **Difficulty & Quality-of-Life Enhancements:** Easy / Modern / Normal / Hard difficulties, staircase & boss autosaves, memory card backups, and key-item glint.

---

## Installing & Running on a Wii U

### Prerequisites
- A modded Wii U running **Aroma** (or **Tiramisu / Homebrew Launcher**).
- Your own legal PS1 *Silent Hill* (USA, `SLUS-00707`) `.bin` disc image (any `*.bin` filename works — the port automatically scans for it).
- The compiled `silenthill_gx2.rpx` (from Releases or built from source below).

### SD Card Setup
Copy the files onto your Wii U's FAT32 SD card using this layout:

```text
sd:/
├── wiiu/
│   └── apps/
│       └── silenthill/
│           └── silenthill_gx2.rpx
└── silenthill/
    └── Silent Hill (USA).bin       <-- your own PS1 .bin disc image (any .bin name)
```

*(You can also place the `.bin` directly inside `sd:/wiiu/apps/silenthill/` or `sd:/`. The port searches `sd:/silenthill/`, `sd:/wiiu/apps/silenthill/`, and `sd:/` in that order.)*

### Launching
- **Aroma:** Open **Homebrew** from the Wii U Menu and launch `silenthill_gx2.rpx` (or place it in `sd:/wiiu/apps/` as a `.rpx` app).
- **Tiramisu / Homebrew Launcher:** Open the **Homebrew Launcher** and select `silenthill`.

### Files Created on the SD Card
Inside the same folder as your `.bin` disc image, the port reads/writes:
- **`save/`** — PS1 memory card saves and automatic backups.
- **`silenthill_NNN.log`** — rolling debug logs (the last 20 are kept).
- **`silenthill.cfg`** — optional settings file (see [Configuration](#configuration-silenthillcfg)).
- **`gamepad_nohealth`** — created automatically when you tap the GamePad health strip to hide it.
- **`gputest`** — create an empty file with this name to display the renderer self-test picture on boot.

---

## Running on Cemu (PC Emulator)

1. Open Cemu and locate its virtual SD card folder (`sdcard/` inside your Cemu data directory, e.g., `<Cemu>/sdcard/`).
2. Create `sdcard/silenthill/` and copy your PS1 *Silent Hill* `.bin` disc image into it.
3. In Cemu, go to **File → Load...** and select `silenthill_gx2.rpx`.
4. To view the GamePad screen (live map, health strip, Off-TV toggle), enable **Options → Separate GamePad View** (or hold **Tab**).

---

## Controls

Buttons are mapped by physical position to match the PlayStation layout:

| PlayStation | Wii U GamePad / Pro Controller |
|---|---|
| Cross / Circle / Square / Triangle | B / A / Y / X |
| L1 / R1 | L / R |
| L2 / R2 | ZL / ZR |
| Select / Start | − / + |
| Movement | Left Stick / D-Pad |
| Camera | Right Stick |

---

## Configuration (`silenthill.cfg`)

Create an optional `silenthill.cfg` next to your `.bin` file to customize settings:

```ini
widescreen_mode = 1      # 1 = 16:9 Hor+ (1920x1080), 0 = 4:3 (1440x1080)
minimap = 0              # 0 = off on TV (uses GamePad map), 1 = also show TV minimap
radio_gamepad = 1        # 0 = TV only, 1 = GamePad speaker (default), 2 = both
gamepad_map_zoom = 4     # 1 to 8 (default 4)
preload_chunks = 1       # preload map chunks into MEM2
```

---

## Building from Source

Requires **Linux** or **WSL (Ubuntu)** with **devkitPro** installed under `/opt/devkitpro`.

### 1. Install the devkitPro Wii U Toolchain
Follow the [devkitPro pacman setup](https://devkitpro.org/wiki/devkitPro_pacman), then install the Wii U packages:

```bash
sudo dkp-pacman -S wiiu-dev wiiu-sdl2 ppc-zlib
```

### 2. Clone the Repository
```bash
git clone -b wiiu-port https://github.com/comaro-sudo/silent-hill-decomp.git
cd silent-hill-decomp
```

### 3. Download CafeGLSL (once, for the GX2 Renderer)
```bash
bash wiiu_port/fetch_cafeglsl.sh
```
This downloads CafeGLSL into `wiiu_port/cafeglsl/` (statically linked into the `.rpx`, so no extra `.rpl` is needed on the SD card).

### 4. Build the `.rpx`
```bash
bash wiiu_port/build.sh
```
The compiled executable is written to:
- `wiiu_port/bin/silenthill_gx2.rpx`

### Useful Build Options
- **SDL2 fallback renderer** (no CafeGLSL required):
  ```bash
  WIIU_RENDERER=sdl bash wiiu_port/build.sh
  ```
  Outputs `wiiu_port/bin/silenthill.rpx`.
- **Compile game & HAL objects only:** `bash wiiu_port/gate.sh`
- **Compile map overlays only:** `bash wiiu_port/maps.sh`
- **Relink only:** `SKIP_GATE=1 SKIP_MAPS=1 bash wiiu_port/build.sh`

---

## Project Layout

| Path | Purpose |
|---|---|
| `wiiu_port/` | Wii U HAL (GX2 & SDL2 renderers, GamePad screen, VPAD/KPAD input, AX/SDL audio, SD filesystem, entry point, and build scripts) |
| `src/`, `include/` | Decompiled Silent Hill (PS1) game code and headers |
| `pc_port/` | Shared PC port runtime, vendored PsyCross (with big-endian GTE fixes), FMV decoder, and extracted map data |
| `xbox_port/`, `xbox360_port/` | Shared console memory/GPU/CD/memory-card layers and 32-bit big-endian PowerPC fixes |

---

## Status

- Boots and plays through the intro FMV, title screen, attract demos, and gameplay in **Cemu** with full GamePad features.
- **Not yet tested on real Wii U hardware** — in particular, GamePad touch coordinate calibration assumes 1280×720 space and should be verified on console. Feedback and hardware test reports are welcome!
