# Silent Hill — Windows ARM64 PC Port

Native **Windows 11 ARM64** (Snapdragon / `CLANGARM64`) PC port of **Silent Hill (PS1)**, built on top of the [Silent Hill matching decompilation](https://github.com/Vatuu/silent-hill-decomp) and [SlickAmogus's PC port](https://github.com/SlickAmogus/silent-hill-decomp) (using [PsyCross](https://github.com/OpenDriver2/PsyCross)).

> **Legal notice:** *Silent Hill* is © Konami. This repository contains **no copyrighted game assets**. You must provide your own legally obtained PS1 *Silent Hill* (USA) `.bin` disc image to play.

---

## Features

- **Native Windows ARM64 binary** (`SilentHillPC.exe` + per-map DLLs) compiled with MSYS2 `CLANGARM64` (`clang` / `clang++`), including DPI awareness and early OpenGL-call guards for Snapdragon Adreno drivers.
- **Modern Difficulty & Quality-of-Life:**
  - **Easy / Modern / Normal / Hard** difficulty selection (Modern includes gradual health regen, 1.5× ammo, enemy ammo drops beside bodies, and subtle door-guide shine when stuck).
  - **Staircase & Boss Autosaves** + **Memory Card Backups** (`save/backup/`).
  - **Key-Item Glint:** subtle shine on key items across the game.
  - **Local Achievements:** offline achievement tracking and pop-up badges (`pc_port/assets/gamedata/achievements/`).
- **Modern PC Defaults:** 1920×1200 borderless window (configurable in `pc_port/config.cfg`), PGXP perspective correction enabled, and WASD control layout.

---

## Running

1. Place your PS1 *Silent Hill* (USA, `SLUS-00707`) `.bin` disc image inside `pc_port/build/gamedata/` (for example `pc_port/build/gamedata/Silent Hill (USA).bin`).
2. Copy `pc_port/config.cfg` into `pc_port/build/` (optional — edit resolution, fullscreen/borderless, controls, and difficulty/QoL options there).
3. Run `pc_port/build/SilentHillPC.exe`.

---

## Building from Source (Windows ARM64)

### 1. Install MSYS2 (`CLANGARM64`)
Install [MSYS2](https://www.msys2.org/) on Windows 11 ARM64, open the **MSYS2 CLANGARM64** shell, and install the toolchain & dependencies:

```bash
pacman -Syu
pacman -S --needed \
    mingw-w64-clang-aarch64-clang \
    mingw-w64-clang-aarch64-cmake \
    mingw-w64-clang-aarch64-ninja \
    mingw-w64-clang-aarch64-SDL2 \
    mingw-w64-clang-aarch64-openal \
    mingw-w64-clang-aarch64-pkgconf
```

### 2. Clone the Repository
```bash
git clone --recursive -b pc-arm64 https://github.com/comaro-sudo/silent-hill-decomp.git
cd silent-hill-decomp
```
*(Patched `pc_port/PsyCross` with Windows ARM64 & memory-card backup fixes is vendored directly in this branch; `--recursive` pulls `rcheevos` for RetroAchievements support.)*

### 3. Build
From the **MSYS2 CLANGARM64** shell:

```bash
bash pc_port/build-arm64.sh
```

Or from PowerShell:
```powershell
$env:MSYSTEM='CLANGARM64'
C:\msys64\usr\bin\bash.exe -lc 'cd /c/path/to/silent-hill-decomp && bash pc_port/build-arm64.sh'
```

Output: `pc_port/build/SilentHillPC.exe` and map DLLs in `pc_port/build/maps/`.

---

## Project Layout

| Path | Purpose |
|---|---|
| `pc_port/` | PC port runtime, CMake build, `build-arm64.sh`, `config.cfg`, vendored `PsyCross`, and achievement badge assets |
| `src/`, `include/` | Decompiled Silent Hill (PS1) game code and headers |
| `configs/`, `lib/` | Decompilation symbol configs and shared library headers |
