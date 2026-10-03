<div align="center">
    <img src="./omnilaunch.png" alt="OmniLaunch" width="380" />
    <p>The definitive multi-layout, highly customizable custom HOME menu replacement for Nintendo Switch.</p>
    <p><i>Unifying the runtime architecture and rich services of SwitchU with the signature 3D carousel and presentation engines of sLaunch.</i></p>
</div>

<p align="center">
  <a rel="LICENSE" href="https://github.com/ncarvalho99/omnilauncher/blob/master/LICENSE">
    <img src="https://img.shields.io/static/v1?label=license&message=GPLV3&labelColor=111111&color=0057da&style=for-the-badge" alt="License">
  </a>
  <a rel="VERSION" href="https://github.com/ncarvalho99/omnilauncher/releases/latest">
    <img src="https://img.shields.io/github/v/release/ncarvalho99/omnilauncher?labelColor=111111&color=06f&style=for-the-badge" alt="Version">
  </a>
  <a rel="BUILD" href="https://github.com/ncarvalho99/omnilauncher/actions">
      <img src="https://img.shields.io/github/actions/workflow/status/ncarvalho99/omnilauncher/switch.yml?branch=master&labelColor=111111&color=06f&style=for-the-badge" alt="Build">
  </a>
</p>

---

## Table of Contents

- [Overview](#overview)
- [Key Features](#key-features)
  - [8 Selectable Launcher View Modes](#8-selectable-launcher-view-modes)
  - [Hardware NVDEC Video Wallpapers](#hardware-nvdec-video-wallpapers)
  - [Integrated USB MTP File Transfer](#integrated-usb-mtp-file-transfer)
  - [Full SteamGridDB Artwork Integration & Game Dossier](#full-steamgriddb-artwork-integration--game-dossier)
  - [WaraWara Plaza & Miiverse Experience](#warawara-plaza--miiverse-experience)
  - [Audio & Multimedia Center](#audio--multimedia-center)
  - [Folders, Custom Widgets & Live Tiles](#folders-custom-widgets--live-tiles)
  - [Built-In System Settings & On-Screen Keyboard](#built-in-system-settings--on-screen-keyboard)
  - [Activity Log & Playtime Tracker](#activity-log--playtime-tracker)
  - [Cheats & Mods Manager](#cheats--mods-manager)
  - [Auto-Update & Self-Uninstall Engine](#auto-update--self-uninstall-engine)
  - [Accessibility & Multi-Language Support](#accessibility--multi-language-support)
- [Screenshots](#screenshots)
- [Installation](#installation)
- [Building from Source](#building-from-source)
- [Credits & Acknowledgements](#credits--acknowledgements)
- [License](#license)

---

## Overview

**OmniLaunch** is a complete, feature-rich replacement for the stock Nintendo Switch `qlaunch` HOME Menu. It merges the rock-solid daemon lifecycle, application handoff authority, folder hierarchy, widgets, and offline/online services of the **SwitchU** ecosystem with the presentation prowess, hardware-accelerated 3D carousel views, and aesthetic minimalism of **sLaunch**.

Games and applets launch through a persistent, low-overhead daemon (`0100000000001000`), meaning HOME returns to it seamlessly, suspended titles can be resumed instantly, and sleep, shutdown, and reboot are natively managed.

---

## Key Features

### 8 Selectable Launcher View Modes
Cycle between 8 distinct layout modes dynamically on **Minus (−)** or via the Settings / Theme Shop menu:
1. **Grid Mode:** Traditional Wii U-style layout with configurable columns (3–8) and rows (2–5), page transitions, drag-and-drop reordering, and expandable tiles.
2. **Dynamic Line:** Fast horizontal carousel with a single focused hero tile and proximity-scaled neighbors.
3. **Flow (3D Coverflow):** Authentic 3D coverflow carousel with watertight case geometry, dynamic perspective tilt, real-time ground reflection, proximity lighting, and 2:3 vertical cover art.
4. **Shelf:** 3D perspective shelf view featuring vertical spine and front case art.
5. **Deck:** Clean angled card deck with physical depth cues.
6. **Cover:** Focused large-format single cover presentation.
7. **XMB:** PlayStation-inspired Cross-Media Bar featuring vertical categorical navigation (Users, Settings, Games, Media, Homebrew).
8. **Metro (Live Tiles):** Windows Phone / Xbox-inspired Live Tiles grid with customizable 1×1, 2×1 (wide), and 2×2 (expanded) live animated tiles, SteamGridDB hero banners, high-resolution vector glyphs, and a dedicated top-bar control cluster.
9. **List (Niagara):** High-speed vertical carousel with dynamic cursor easing, proximity pill highlighting, and a dedicated hero detail card displaying synopsis, play records, publisher, and size badges.

### Hardware NVDEC Video Wallpapers
- Supports native video playback (`.mp4`, `.mkv`, `.webm`, `.mov`) directly as animated theme wallpapers using the Tegra X1 **NVDEC hardware video accelerator** (`nvtegra` via FFmpeg).
- Operates on a dedicated Horizon OS worker thread with zero frame-drops or UI stutter.
- Includes a resolution and rate guard: safely blocks clips exceeding 1080p60 to protect hardware performance.

### Integrated USB MTP File Transfer
- Built-in background USB MTP responder (adapted from Atmosphère's `haze` and sLaunch).
- Plug the console directly into Windows, macOS, or Linux (KDE Dolphin / GNOME Files) to drag and drop files directly onto the microSD card without rebooting into payload mode or removing the card.
- Native Linux / KDE Solid compatibility via Vendor-Specific class declaration (`0xFF`).

### Full SteamGridDB Artwork Integration & Game Dossier
- Automatic and manual SteamGridDB artwork search for all installed titles and homebrew applications.
- In-menu artwork picker with live candidate preview: apply high-res 600×900 vertical covers, 920×430 wide grid banners, 460×215 hero banners, and transparent PNG logos.
- Rich Game Dossier screen: view playtime records, game version, launch counts, publisher, synopsis, and local screenshot gallery.

### WaraWara Plaza & Miiverse Experience
- Full-screen interactive WaraWara Plaza populated by console user accounts and bundled high-resolution guest Mii avatars.
- Dialogue engine with game tips, community news, and classic Miiverse-style speech bubbles.
- Procedural **Animalese voice synthesizer** powered by eSpeak NG.

### Audio & Multimedia Center
- Dedicated Multimedia Center for local MP3, OGG, and WAV soundtrack playback.
- Background BGM playback during menu navigation with shuffle and volume controls.
- Integrated YouTube-DL audio streaming bridge client (`ytdl.nclabs.dev`).

### Folders, Custom Widgets & Live Tiles
- Full folder hierarchy support: group games into folders with customizable color tint, live 9-item mini-previews, and smooth opening animations.
- Interactive widgets: Live Digital Clock, Console & Controller Battery rings (Joy-Con L/R/Pro), Recently Played game hero card, Recent Playtime, Random Screenshot, and Image Pin widgets (supporting static pictures and animated GIFs).

### Built-In System Settings & On-Screen Keyboard
- **System Settings:** Firmware info, Atmosphère version, EmuNAND status, timezone, clock sync, and language selector.
- **Hardware Controls:** Bluetooth pairing and connection manager, display brightness, audio output, sleep timers, and storage manager.
- **Controller Test:** Live visual diagnostics for Joy-Cons, Pro Controllers, stick deadzones, buttons, and touch screen calibration.
- **Custom Touch & Controller Virtual Keyboard:** Full on-screen keyboard with accented letters, symbols, and word prediction.

### Activity Log & Playtime Tracker
- Accurate per-title playtime tracker logging total hours, minutes, launch counts, and first/last played timestamps.
- Daily, monthly, and yearly activity bar charts.

### Cheats & Mods Manager
- Built-in game cheats toggle reading Atmosphere cheat databases directly from the SD card.
- RomFS LayeredFS mod toggles per game.

### Auto-Update & Self-Uninstall Engine
- Self-updater fetching stable releases and assets from GitHub API (`ncarvalho99/omnilauncher`).
- Safe two-phase staging: updates are verified, unpacked, and applied at boot by the daemon before the menu executable loads.
- **Clean Self-Uninstall:** Permanently purges OmniLaunch and SwitchU files from `atmosphere/contents/`, `switch/`, and `config/`, rebooting cleanly into the original stock Nintendo HOME Menu.

### Accessibility & Multi-Language Support
- Full text-to-speech voice guidance via eSpeak NG with adjustable speed.
- Native localization in 8 languages: English, Portuguese (Brasil), Spanish, French, German, Italian, Dutch, and Russian.

---

## Screenshots

<div align="center">
  <img src="./screenshots/2026100222051600-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  <img src="./screenshots/2026100222110700-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
</div>
<div align="center">
  <img src="./screenshots/2026100222112300-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  <img src="./screenshots/2026100222112800-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
</div>
<div align="center">
  <img src="./screenshots/2026100222113700-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  <img src="./screenshots/2026100222114300-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
</div>
<div align="center">
  <img src="./screenshots/2026100222115100-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  <img src="./screenshots/2026100222115600-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
</div>
<div align="center">
  <img src="./screenshots/2026100222123900-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
</div>

<details>
  <summary><b>View More Screenshots</b></summary>
  <br>
  <div align="center">
    <img src="./screenshots/2026100222052600-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222053000-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222053400-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222053800-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222054000-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222054300-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222054800-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222061300-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222061800-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222064000-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222064300-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222065200-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222065800-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222070100-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222072800-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222073200-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222074100-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222074400-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222074700-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222083300-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
  <div align="center">
    <img src="./screenshots/2026100222084100-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
    <img src="./screenshots/2026100222104600-DB1426D1DFD034027CECDE9C2DD914B8.jpg" width="48%" />
  </div>
</details>


---

## Installation

1. Download the latest `OmniLaunch-sysmodule-release.zip` from the [Releases](https://github.com/ncarvalho99/omnilauncher/releases) page.
2. Extract the archive directly to the root of your Nintendo Switch microSD card:
   - `atmosphere/contents/0100000000001000/exefs.nsp`
   - `switch/OmniLaunch/`
   - `switch/OmniLaunch-Manager/OmniLaunch-Manager.nro`
3. If upgrading from **SwitchU**, you do not need to do anything: on first boot, OmniLaunch will automatically detect and migrate your existing `config/SwitchU/` and `switch/SwitchU/` data to `config/OmniLaunch/` with all settings, themes, and saves intact.
4. Reboot your console.

---

## Building from Source

### Prerequisites
- [devkitPro](https://devkitpro.org/) with `devkitA64` toolchain and Switch portlibs.
- [xmake](https://xmake.io/) build utility.

### Production Sysmodule Build
```bash
xmake f -p cross -a aarch64 --toolchain=devkita64 -m release --homebrew=n --backend=deko3d
xmake -j$(nproc)
```

### Reproducible Build with Docker
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-image.ps1 -PullBase
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-local.ps1 -Mode release -Variant sysmodule -SkipConsoleDeploy
```
The compiled output package will be placed under `artifacts/OmniLaunch-sysmodule-release.zip`.

---

## Credits & Acknowledgements

- **[PoloNX](https://github.com/PoloNX):** Creator of [SwitchU](https://github.com/PoloNX/SwitchU).
- **[etonedemid](https://github.com/etonedemid):** Creator of [sLaunch](https://github.com/etonedemid/slaunch).
- **[Xortroll](https://github.com/Xortroll):** Creator of [uLaunch](https://github.com/Xortroll/uLaunch).
- **[ncarvalho99](https://github.com/ncarvalho99):** Architecture, integration engineering, optimization, and maintenance of OmniLaunch.

---

## License

OmniLaunch is licensed under the [GNU General Public License v3.0](https://github.com/ncarvalho99/omnilauncher/blob/master/LICENSE).
