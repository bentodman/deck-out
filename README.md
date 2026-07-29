# Deck Out

An OBS Studio filter that sends a **scene or source** straight to a **Blackmagic DeckLink** output.

By [Ben Todman](https://github.com/bentodman) · [flashbang.media](https://www.flashbang.media)

Inspired by [DistroAV](https://github.com/DistroAV/DistroAV)'s dedicated-output filter. Built on the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) scaffold.

## Features

- **DeckLink Output** filter on scenes and sources
- Start / Stop and Auto Start
- Audio routing: master, source/scene, selected source, or mute (video only), with pre/post-fader tap
- Device conflict detection (Tools → DeckLink Output, other filters, or DeckLink input on the same device)
- Status line in the filter properties
- It does not reimplement DeckLink support. Instead it drives OBS’s existing built-in `decklink_output`, so device discovery, modes, keyer, and SDR options stay native to OBS.

## Requirements

- [OBS Studio](https://obsproject.com/) with DeckLink support enabled
- A Blackmagic DeckLink device and [Desktop Video](https://www.blackmagicdesign.com/support) drivers

| Platform | Status |
|----------|--------|
| macOS (Apple Silicon) | Tested |
| macOS (Intel) | Not tested |
| Windows | Tested |
| Linux | Not tested |

## Install

1. Download a [release](https://github.com/bentodman/deck-out/releases) for your platform (or [build from source](#build-from-source)).
2. Install the package, or copy the plugin into OBS’s user plugins folder:

| Platform | Package | Manual path |
|----------|---------|-------------|
| macOS | `.pkg` installer | `~/Library/Application Support/obs-studio/plugins/` |
| Windows | `.exe` installer or `.zip` | `%PROGRAMDATA%\obs-studio\plugins\` |
| Linux | `.deb` (`sudo dpkg -i …`) | `~/.config/obs-studio/plugins/` |

3. Restart OBS.

## Usage

1. Right-click a **scene** or **source** → **Filters** → add **DeckLink Output**.
2. Choose the DeckLink device and mode.
3. Set audio routing if needed.
4. Click **Start Output** (or enable **Auto Start**).

Stop the output before changing device/mode settings. Only one filter (or Tools → DeckLink Output) can use a given device at a time.

## Build from source

You need CMake **3.30.5+** and a C/C++ toolchain. On first configure, macOS/Windows download OBS build deps into `.deps/`; Linux uses system OBS packages.

After building, use the **package** steps below to produce installable artifacts under `release/`.

### macOS

The `macos` preset builds a **universal** binary (`arm64` + `x86_64`). That works when OBS/deps are universal (the first configure downloads them into `.deps/`).

If you only have **Apple Silicon** OBS (arm64-only — typical for `/Applications/OBS.app`), build arm64 only or the x86_64 link will fail:

```bash
cmake --preset macos -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build --preset macos --parallel
```

Universal build (downloaded `.deps`):

```bash
cmake --preset macos
cmake --build --preset macos --parallel
```

**Package** (creates the `.pkg` installer and plugin bundle):

```bash
cmake --install build_macos --config RelWithDebInfo --prefix release/RelWithDebInfo
```

Artifacts:

- `release/RelWithDebInfo/deck-out-*-macos-*-Installer.pkg` — installer
- `release/RelWithDebInfo/deck-out.plugin` — plugin bundle (manual install)

Optional: put machine-specific overrides (local OBS paths, `CMAKE_OSX_ARCHITECTURES`, etc.) in a gitignored `CMakeUserPresets.json`.

### Windows

Requires [Inno Setup 6](https://jrsoftware.org/isinfo.php) to package.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
```

**Package** (creates the installer and zip):

```powershell
cmake --install build_x64 --config RelWithDebInfo --prefix release/RelWithDebInfo
.\.github\scripts\Package-Windows.ps1 -Target x64 -Configuration RelWithDebInfo
```

Artifacts:

- `release/deck-out-*-windows-x64-Installer.exe` — installer (requires admin; installs to `%PROGRAMDATA%`)
- `release/deck-out-*-windows-x64.zip` — manual install into `%PROGRAMDATA%\obs-studio\plugins\`

### Linux (Ubuntu)

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config obs-studio
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 --parallel
```

**Package** (creates a `.deb` via CPack):

```bash
cmake --build build_x86_64 --target package
```

Artifact: `release/deck-out-*-x86_64.deb` (exact name includes version and architecture). Install with:

```bash
sudo dpkg -i release/deck-out-*.deb
```

## License

Copyright (C) 2026 Ben Todman.

GPL-2.0-or-later — see [LICENSE](LICENSE) and source file headers.

- Website: [www.flashbang.media](https://www.flashbang.media)
- GitHub: [github.com/bentodman](https://github.com/bentodman)
