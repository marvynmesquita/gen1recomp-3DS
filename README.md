# Gen1Recomp — Nintendo 3DS Port

A native LÖVE2D recreation of Pokemon Red, Blue, and Yellow for the Nintendo 3DS. The engine and map behavior are hand-written Lua; game data and graphics are decoded from a ROM supplied by the player.
> **This project was developed through vibecoding** — an AI-assisted, conversational development workflow.

<p align="center"><img src="https://raw.githubusercontent.com/bryanthaboi/gen1recomp/refs/heads/dev/assets/logo/logo.png"></p>

## Features

- Play Pokemon Red, Blue, and Yellow natively on Nintendo 3DS
- Stereoscopic 3D support (with performance considerations — see below)
- Full battle system with animations
- Overworld exploration with NPC interactions
- Save/load game support
- Mod support via Lua scripts
- Custom HUD and battle interface

## Installation

### Requirements

- A Nintendo 3DS with custom firmware (Luma3DS recommended)
- [FBI](https://github.com/Steveice10/FBI) installed on your 3DS
- A Pokemon Red, Blue, or Yellow ROM file (`.gb` or `.gbc`)

### Steps

1. Download the latest release `.cia` file from the [Releases](../../releases) page
2. Copy `gen1recomp3ds.cia` to your SD card (anywhere)
3. Launch **FBI** on your 3DS
4. Navigate to the `.cia` file and select **Install and delete** (or just Install)
5. Exit FBI — Gen1Recomp will appear on your Home Menu
6. Place your Pokemon ROM files in the `roms/` folder on your SD card
7. Launch Gen1Recomp and select your ROM

### Directory Structure

```
SD:/
├── 3ds/
│   └── gen1recomp3ds/
│       ├── gen1recomp3ds.cia
│       ├── options.lua
│       ├── roms/
│       │   └── (your .gb/.gbc ROM files here)
│       └── mods/
│           └── (optional Lua mods here)
```

## Performance Notes

### Stereoscopic 3D

> [!WARNING]
> **Stereoscopic 3D may cause framerate drops.** The engine renders the scene twice (once per eye) for 3D depth effect, which roughly doubles the GPU workload. If you experience slowdowns, disable stereoscopic 3D in your 3DS system settings or use the in-game slider to reduce 3D depth.

### Recommended Settings

- **3D Slider:** Reduce or disable for better performance
- **Battery:** Use AC adapter for extended play sessions
- **CPU Clock:** The New 3DS/2DS XL models provide better performance than the original models

### Performance Tips

- Overworld areas with many NPCs may experience slight slowdowns — this is normal
- Battle animations are the most CPU-intensive sequences
- If framerate is consistently low, try reducing the 3D slider to 0

## Building from Source

### Requirements

- [devkitPro](https://devkitpro.org/) with devkitARM
- Python 3 (for build scripts)

### Build Commands

```bash
# Build the .3dsx (for HBL)
make clean && make

# Build the .cia (for FBI installation)
make clean && make cia
```

### CIA Build Notes

The CIA build requires:
- `makerom` (included in devkitPro)
- A valid banner and logo in `assets/` (pre-built `banner.bin` is included)

## Troubleshooting

| Issue | Solution |
|-------|----------|
| "SD Card was removed" error | Ensure `assets/banner.bin` and `assets/logo.bin` exist |
| Game crashes on load | Verify your ROM file is valid and not corrupted |
| Low framerate | Reduce 3D slider; use New 3DS/2DS XL |
| No sound | Check that `ndsp` is initialized (required for audio) |
| Save data issues | Ensure SD card has sufficient free space |

## Credits

- **Engine:** Gen1Recomp project
- **Original Pokemon:** Nintendo/Game Freak
- **3DS Port:** [marvynmesquita](https://github.com/marvynmesquita)
- **Build System:** devkitPro toolchain

## License

This project is a fan-made recreation and is not affiliated with Nintendo or The Pokemon Company. Pokemon is a trademark of Nintendo/Game Freak/Creatures Inc.

---

**Note:** This is a passion project built with love for the original games. If you enjoy it, consider sharing it with fellow Pokemon fans!

A native **Nintendo 3DS** port of [gen1recomp](https://github.com/bryanthaboi/gen1recomp), a Pokemon Red/Blue/Yellow decompilation and recompilation project. This port adapts the game to run on 3DS homebrew via [LovePotion](https://github.com/lovebrew/LovePotion) and LÖVE2D.

> **This project was developed through vibecoding** — an AI-assisted, conversational development workflow.

## Status

| Feature | Status |
|---------|--------|
| Pokemon Red | ✅ Supported |
| Pokemon Blue | ✅ Supported |
| Pokemon Yellow | ✅ Supported |
| Pokemon Gold | ❌ Not yet (WIP) |
| Pokemon Silver | ❌ Not yet |
| Pokemon Crystal | ❌ Not yet |
| Mods | ⚠️ WIP — not yet compatible |

Only **Generation 1** Pokemon games are supported at this time.

## Downloads

Grab the latest `.cia` from [Releases](../../releases). Install it using FBI on your 3DS.

### SD Card Structure

```
sdmc:/
└── 3ds/
    └── gen1recomp3ds/
    ├── gen1recomp3ds.cia     ← install this via FBI
        ├── options.lua           ← settings (auto-created on first run)
        ├── roms/
        │   └── *.gb              ← place your ROMs here
        └── mods/
            └── <mod folders>     ← optional mods (WIP)
```

### Controls

| Button | Action |
|--------|--------|
| D-Pad | Move / Navigate |
| A | Confirm |
| B | Back / Cancel |
| START | Open launcher menu (in-game) |
| L + R + START | Return to launcher |
| Touch screen | Tap buttons and menu items |

## Building from Source

### Prerequisites

- [devkitPro](https://devkitpro.org/) with `devkitARM` and `libctru`
- A 3DS with [Luma3DS](https://github.com/LumaTeam/Luma3DS) custom firmware (for testing)

### Setup

<details>
<summary><b>macOS</b></summary>

```bash
# Install devkitPro via the official installer
# Download from: https://devkitpro.org/wiki/Getting_Started

# Or via Homebrew (if available):
# brew install devkitpro-pacman

# Install required packages
sudo dkp-pacman -S 3ds-dev

# Clone the repository
git clone https://github.com/marvynmesquita/gen1recomp-3DS.git
cd gen1recomp-3DS

# Build
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH="$DEVKITARM/bin:$PATH"
make -j$(sysctl -n hw.ncpu)
```
</details>

<details>
<summary><b>Linux (x86_64)</b></summary>

```bash
# Install devkitPro (Debian/Ubuntu)
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x install-devkitpro-pacman
sudo ./install-devkitpro-pacman

# Install required packages
sudo dkp-pacman -S 3ds-dev

# Clone the repository
git clone https://github.com/marvynmesquita/gen1recomp-3DS.git
cd gen1recomp-3DS

# Build
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH="$DEVKITARM/bin:$PATH"
make -j$(nproc)
```
</details>

<details>
<summary><b>Linux (ARM SBC — Raspberry Pi, etc.)</b></summary>

```bash
# Same steps as x86_64 Linux above — devkitPro has ARM packages.
# If building on-device, ensure at least 1 GB free RAM for linking.

sudo dkp-pacman -S 3ds-dev

git clone https://github.com/marvynmesquita/gen1recomp-3DS.git
cd gen1recomp-3DS

export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH="$DEVKITARM/bin:$PATH"
make -j$(nproc)
```
</details>

<details>
<summary><b>Windows</b></summary>

```powershell
# Install devkitPro via the Windows installer:
# https://github.com/devkitPro/installer/releases

# Or use MSYS2 / pacman:
# pacman -S devkitARM-pacman
# dkp-pacman -S 3ds-dev

# Clone the repository
git clone https://github.com/marvynmesquita/gen1recomp-3DS.git
cd gen1recomp-3DS

# Build (in MSYS2 or Git Bash)
export DEVKITPRO=/c/devkitPro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH="$DEVKITARM/bin:$PATH"
make -j$(nproc)
```
</details>

The output `gen1recomp3ds.3dsx` will be in the project root.

## Project Structure

```
source/          C++ source (LÖVE2D 3DS bindings, GPU, input, audio)
romfs/           RomFS content packed into the .3dsx
  main.lua         Entry point
  conf.lua         LÖVE2D configuration
  src/             Game logic (Lua)
    launcher3ds.lua  Launcher / settings menu
    core/            Core engine (GameVersion, SaveIO, etc.)
    world/           Overworld / map rendering
    battle/          Battle system
    ui/              UI components
    audio/           Audio engine
include/         C++ headers
```

## Credits

This project builds upon the work of:

- **[gen1recomp](https://github.com/bryanthaboi/gen1recomp)** — the original Pokemon Red/Blue/Yellow recompilation project
- **[LovePotion](https://github.com/lovebrew/LovePotion)** — LÖVE2D framework for Nintendo 3DS
- **[libctru](https://github.com/devkitPro/libctru)** — 3DS homebrew library
- **[devkitPro](https://devkitpro.org/)** — ARM toolchain for 3DS homebrew development

## License

See individual source files and dependencies for their respective licenses.
