# Pokemon Gen1 Recomp — Nintendo 3DS Port

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

Grab the latest `.3dsx` from [Releases](../../releases). Extract the zip to the root of your 3DS SD card.

### SD Card Structure

```
sdmc:/
└── 3ds/
    └── gen1recomp3ds/
        ├── gen1recomp3ds.3dsx    ← launch this
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
