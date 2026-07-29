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
| Windows | Not tested |
| Linux | Not tested |

## Install

1. Download a release for your platform (or [build from source](#build-from-source)).
2. Install the plugin into OBS’s user plugins folder:

| Platform | Path |
|----------|------|
| macOS | `~/Library/Application Support/obs-studio/plugins/` |
| Windows | `%APPDATA%\obs-studio\plugins\` |
| Linux | `~/.config/obs-studio/plugins/` |

3. Restart OBS.

## Usage

1. Right-click a **scene** or **source** → **Filters** → add **DeckLink Output**.
2. Choose the DeckLink device and mode.
3. Set audio routing if needed.
4. Click **Start Output** (or enable **Auto Start**).

Stop the output before changing device/mode settings. Only one filter (or Tools → DeckLink Output) can use a given device at a time.

## Build from source

You need CMake **3.30.5+** and a C/C++ toolchain. On first configure, macOS/Windows download OBS build deps into `.deps/`; Linux uses system OBS packages.

### macOS

```bash
cmake --preset macos
cmake --build --preset macos --parallel
cp -R build_macos/RelWithDebInfo/deck-out.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/
```

### Windows

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
```

Copy `build_x64\RelWithDebInfo\deck-out.dll` into `%APPDATA%\obs-studio\plugins\deck-out\bin\64bit\`.

### Linux (Ubuntu)

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config obs-studio
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64 --parallel
```

Install the `.so` under `~/.config/obs-studio/plugins/deck-out/bin/64bit/`.

## License

Copyright (C) 2026 Ben Todman.

GPL-2.0-or-later — see [LICENSE](LICENSE) and source file headers.

- Website: [www.flashbang.media](https://www.flashbang.media)
- GitHub: [github.com/bentodman](https://github.com/bentodman)
