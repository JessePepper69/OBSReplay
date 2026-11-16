<div align="center">

<h1><img src="resources/logo/logo.svg" width="40" height="40" align="top" alt=""> OBSReplay</h1> 

**Instant replay for Windows, built on OBS Studio.**

Alternative to Shadowplay & OBS Replay buffer to capture clips. Then watch, trim and export the clip without leaving the app.

[![Latest release](https://img.shields.io/github/v/release/JessePepper69/OBSReplay?label=release&color=6d5ef5)](https://github.com/JessePepper69/OBSReplay/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/JessePepper69/OBSReplay/total?color=6d5ef5)](https://github.com/JessePepper69/OBSReplay/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-6d5ef5)](LICENSE)
![Windows 10 and 11](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D6)

[![Download](https://img.shields.io/badge/Download-OBSReplay-6d5ef5?style=for-the-badge&logo=github&logoColor=white)](https://github.com/JessePepper69/OBSReplay/releases/latest)

</div>

![Capture page: the live replay buffer and the latest clips](docs/images/capture.png)

> [!NOTE]
> Claude wrote the code. The UI/UX, user flow, onboarding and feature set come from me and from public recommendations. If you still hand-write code in 2026, when a model can program and diagnose in 30 seconds what would take you weeks, you are a caveman.

## Contents

- [Why OBSReplay](#why-obsreplay)
- [Features](#features)
- [Install](#install)
- [Where your files go](#where-your-files-go)
- [What it changes on your PC](#what-it-changes-on-your-pc)
- [Build from source](#build-from-source)
- [Documentation](#documentation)
- [Credits](#credits)
- [License](#license)

## Why OBSReplay

OBSReplay pairs the OBS capture engine with the convenience of ShadowPlay.

- **OBS Studio alone** has no player or library for your clips, uses encoder terms that confuse new users, and takes manual work to start capture at every Windows boot.
- **ShadowPlay alone** draws common complaints: heavy resource use, corrupted video or audio tracks, recording that turns itself off, large files, few controls and weak microphone noise suppression.
- **OBSReplay** sets up OBS for you, starts capture with Windows, and adds a clip player, a library filtered by game, trim and export, separate desktop and microphone tracks, and quality settings in plain words.

OBS does the capture and the encoding. OBSReplay installs OBS when it is missing, writes its own OBS profile, starts OBS in the tray and registers the hotkeys. Your existing OBS profiles and scenes stay as they are.

## Features

- **Guided setup:** Installs OBS Studio and FFmpeg if they are missing, then starts capture. Two clicks.
- **Self-install:** Run the exe once. It installs itself, adds a Start menu shortcut and can be uninstalled from Windows or from Settings.
- **Save hotkey:** Press `Alt+Z` in any game to save the last few minutes. A sound confirms the save.
- **Capture modes:** Record fullscreen games only, one whole display, or a hybrid that switches between the two.
- **Clips library:** Browse every clip with a thumbnail, its length and its size. Filter by game. Delete a clip from its card.
- **Replay viewer:** Watch a clip in the app with a timeline, a waveform and frame-by-frame steps.
- **Clip editor:** Cut sections out of a clip and export the result. You pick the export quality and file name.
- **Quality presets:** Choose from Very low to Very high. No bitrate numbers to learn.
- **Hardware encoding:** Uses your NVIDIA, AMD or Intel GPU with H.264, HEVC or AV1.
- **Separate audio tracks:** Game audio and your microphone are saved as two tracks, so you can mute either one later.
- **Microphone noise suppression:** Optional RNNoise or Speex filter on your microphone.
- **Start with Windows:** Capture is running after you sign in. Nothing to open.
- **Tray menu:** Save a replay, start or stop capture, or close the app from the tray.
- **Debug tab:** See how much CPU, memory and GPU OBSReplay and OBS use. Export a log of the session for a bug report.
- **Themes:** Navy, Mono, Light, Rose, Amethyst and Ember.

![Clip editor: a clip with one section removed](docs/images/editor.png)

![Theme picker in Settings](docs/images/themes.png)

## Install

Requirements:

- Windows 10 or 11, 64-bit
- [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (included with Windows 11)
- OBS Studio 31 or newer. Setup installs it for you.
- FFmpeg, only for exporting edited clips. Setup installs it for you.

Steps:

1. Get `OBSReplay.exe` from [Releases](https://github.com/JessePepper69/OBSReplay/releases), or [build it](#build-from-source).
2. Run it. It installs itself in about a second, adds a Start menu shortcut and opens. You can delete the download afterwards.
3. Follow the three setup steps. If OBS Studio or FFmpeg is missing, select **Install OBS Studio and FFmpeg**. Windows asks for permission once.
4. Select **Set up and start capture**.

To update, download the new `OBSReplay.exe` and run it. To remove the app, open **Settings**, **App** in OBSReplay and select **Uninstall**, or use Windows **Installed apps**.

To run it without installing, start it with `OBSReplay.exe --portable`.

## Where your files go

| What | Location |
| --- | --- |
| Saved replays | `Videos\OBSReplay`, in one folder per game. Change it in **Settings**, **Capture**. |
| Edited exports | An `Edited Exports` folder next to the original clip |
| The app | `%LOCALAPPDATA%\Programs\OBSReplay` |
| Settings, clip history, editor preferences | `%LOCALAPPDATA%\OBSReplay` |
| Session logs (the last 3 runs) | `%LOCALAPPDATA%\OBSReplay\logs` |
| OBS profile and scene collection | `%APPDATA%\obs-studio`, both named `OBSReplay` |

## What it changes on your PC

- Copies itself to `%LOCALAPPDATA%\Programs\OBSReplay`, adds a Start menu shortcut and an entry in **Installed apps**. None of this needs administrator rights.
- Installs or updates OBS Studio through `winget`, only when you ask it to.
- Installs FFmpeg through `winget` when you ask it to, in setup or in the editor.
- Adds an `OBSReplay` profile and scene collection to OBS. It does not edit your other profiles.
- Enables the OBS WebSocket server so the app can talk to OBS on this PC.
- Marks the OBS first-run wizard as done, so the wizard does not open over a hidden session.
- Adds an `OBSReplay` entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` while **Start with Windows** is on.

Uninstalling removes the app, the shortcut, the startup entry and the OBS profile. It asks whether to delete your settings. Your clips, OBS Studio and FFmpeg stay.

## Build from source

You need Windows x64, Visual Studio with the C++ desktop workload (MSVC 19.44 or newer), CMake 3.25 or newer, and Git.

```powershell
./build.ps1
```

The first build downloads the pinned dependencies. The result is `dist/OBSReplay.exe`, with the interface, icon, font, notices and runtime compiled in. There is no npm step.

To run a second copy with its own data folder, set `OBSREPLAY_DATA_DIR` to an absolute path before you start it. [docs/validation.md](docs/validation.md) covers the headless mode used for automated checks.

## Documentation

| Document | Covers |
| --- | --- |
| [docs/product.md](docs/product.md) | What the app is for and the rules it keeps |
| [docs/scope.md](docs/scope.md) | Features in and out of scope |
| [docs/architecture.md](docs/architecture.md) | Source map, native bridge, OBS integration, settings, export pipeline |
| [docs/design.md](docs/design.md) | Design system: tokens, themes, layout, components, motion |
| [docs/validation.md](docs/validation.md) | What has been tested, and how to run the checks |
| [docs/research/standalone-obs.md](docs/research/standalone-obs.md) | Research on shipping without a separate OBS install |

## Credits

- [OBS Studio](https://obsproject.com) does the capture and encoding.
- [saucer](https://github.com/saucer/saucer) hosts the WebView2 interface.
- [Inter](https://rsms.me/inter/) is the typeface, under the SIL Open Font License.
- [FFmpeg](https://ffmpeg.org) exports edited clips.
- [ShadBuffer/ShadOBS](https://github.com/seandotnet/ShadBuffer) Original inspiration for this type of project.

Third-party notices are compiled into the executable. Open them from **Help**, **Software notices**.

OBSReplay is not affiliated with the OBS Project.

## License

[MIT](LICENSE).
