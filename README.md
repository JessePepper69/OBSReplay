# OBSReplay

Instant replay for Windows, built on OBS Studio. Press a key and the last few minutes of your game land in a folder. Then watch, trim and export the clip without leaving the app.

![Capture page with the live replay buffer and the latest clips](docs/images/capture.png)

> [!NOTE]
> Claude wrote the code. The UI/UX, user flow, onboarding and feature set come from me and from public recommendations. If you still hand-write code in 2026, when a model can program and diagnose in 30 seconds what would take you weeks, you are a caveman.

## Contents

- [About](#about)
- [Features](#features)
- [Install](#install)
- [Hotkeys](#hotkeys)
- [Where your files go](#where-your-files-go)
- [What it changes on your PC](#what-it-changes-on-your-pc)
- [Good to know](#good-to-know)
- [Build from source](#build-from-source)
- [Project layout](#project-layout)
- [Documentation](#documentation)
- [Credits](#credits)
- [License](#license)

## About

OBS Studio has a good replay buffer and a setup most players never finish. OBSReplay does the setup for you and stays out of the way afterwards.

OBS does the capture and the encoding. OBSReplay installs OBS when it is missing, writes its own OBS profile, starts OBS in the tray, registers the hotkeys and keeps a library of what you saved. Your existing OBS profiles and scenes stay as they are.

The app is one executable of about 3 MB. The interface is plain HTML, CSS and JavaScript inside WebView2, with a C++23 host.

## Features

- **Guided setup.** Three steps: install or locate OBS, choose what to record, start. OBS closes and reopens on its own.
- **One-key save.** A global hotkey saves the replay buffer while you play. A sound confirms the save once OBS reports the file.
- **Three capture modes.** Game or display, fullscreen games only, or one whole display.
- **Clips library.** Thumbnails, clip length and file size, a filter per game, 24 clips a page, separate tabs for replays and edited exports.
- **Replay viewer.** Scrub a filmstrip and waveform timeline, step by frame, zoom with the wheel.
- **Clip editor.** Split, remove and restore sections, undo and redo, per-track volume, then export. Quality 10 keeps the original video and cuts on the exact frame. Lower levels re-encode to a smaller file.
- **Hardware encoding.** NVIDIA, AMD and Intel encoders with H.264, HEVC or AV1, picked from what your GPU supports.
- **Separate audio tracks.** Desktop and microphone record to their own tracks. The microphone is off until you turn it on.
- **Seven themes.** Original, Mono, Light, Rose, Forest, Amethyst and Ember.
- **Tray app.** Closing the window keeps the hotkeys alive. Start with Windows is optional.

![Clip editor with a removed section](docs/images/editor.png)

![Theme picker in Settings](docs/images/themes.png)

## Install

Requirements:

- Windows 10 or 11, 64-bit
- [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (included with Windows 11)
- OBS Studio 31 or newer. Setup installs it for you.
- FFmpeg, only for exporting edited clips. The editor installs it for you.

Steps:

1. Get `OBSReplay.exe` from [Releases](https://github.com/JessePepper69/OBSReplay/releases), or [build it](#build-from-source).
2. Put it in a folder you plan to keep, then run it.
3. Follow the three setup steps. If OBS is missing, select **Install OBS Studio**. Windows asks for permission once.
4. Select **Set up and start capture**.

There is no installer. To remove the app, see [What it changes on your PC](#what-it-changes-on-your-pc).

## Hotkeys

| Action | Default | Scope |
| --- | --- | --- |
| Save replay | `Alt+Z` | Global |
| Show or hide the window | `Alt+X` | Global |
| Play or pause | `Space` or `K` | Viewer and editor |
| Move 1 second (5 with `Shift`) | `←` `→` | Viewer and editor |
| Previous or next frame | `,` `.` | Viewer and editor |
| Split at the playhead | `S` | Editor |
| Remove or restore a section | `Delete` | Editor |
| Remove everything before or after the playhead | `I` `O` | Editor |
| Undo, redo | `Ctrl+Z` `Ctrl+Y` | Editor |
| Export | `Ctrl+E` | Editor |

Change the two global hotkeys in **Settings**, **App**. `Alt+Z` is also the default for the NVIDIA overlay. If both are active, OBSReplay reports the conflict and offers **Change shortcut**.

## Where your files go

| What | Location |
| --- | --- |
| Saved replays | `Videos\OBSReplay`, in one folder per game. Change it in **Settings**, **Capture**. |
| Edited exports | An `Edited Exports` folder next to the original clip |
| Settings, clip history, editor preferences | `%LOCALAPPDATA%\OBSReplay` |
| OBS profile and scene collection | `%APPDATA%\obs-studio`, both named `OBSReplay` |

## What it changes on your PC

- Installs or updates OBS Studio through `winget`, only when you ask it to.
- Installs FFmpeg through `winget` when you select **Install FFmpeg** in the editor.
- Adds an `OBSReplay` profile and scene collection to OBS. It does not edit your other profiles.
- Enables the OBS WebSocket server so the app can talk to OBS on this PC.
- Marks the OBS first-run wizard as done, so the wizard does not open over a hidden session.
- Adds an `OBSReplay` entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` while **Start with Windows** is on.

To undo it: open **Help**, select **Reset integration**, turn off **Start with Windows**, then delete the executable and `%LOCALAPPDATA%\OBSReplay`. Your clips stay where they are.

## Good to know

- **Game or display records your desktop between games.** Other windows and notifications end up in the buffer. Choose **Fullscreen games only** to avoid that.
- **Stopping capture discards the buffer.** Save first.
- **Changing capture or audio settings restarts OBS.** OBSReplay saves the current buffer before it does.
- **Quitting OBSReplay leaves OBS running.** Capture continues. The hotkeys stop until you reopen the app.
- **Replay length is an upper limit.** OBS holds the buffer in memory.
- **A stream or full recording in OBS blocks setup.** OBSReplay asks you to stop it and never force-closes OBS.
- **Game folders are named after the app in front** when you save. That is a best guess at the game.
- **Some games block capture.** Anti-cheat and protected video can leave a black frame. That comes from OBS game capture.

## Build from source

You need Windows x64, Visual Studio with the C++ desktop workload (MSVC 19.44 or newer), CMake 3.25 or newer, and Git.

```powershell
./build.ps1
```

The first build downloads the pinned dependencies. The result is `dist/OBSReplay.exe`, with the interface, icon, font, notices and runtime compiled in. There is no npm step.

To run a second copy with its own data folder, set `OBSREPLAY_DATA_DIR` to an absolute path before you start it. [docs/validation.md](docs/validation.md) covers the headless mode used for automated checks.

## Project layout

```text
OBSReplay/
├── src/            C++ host: window, tray, OBS control, settings, media, editor export
├── frontend/       Interface: HTML, CSS, JavaScript, bundled Inter font
├── resources/      Icon, manifest, version resource, save sound
├── docs/           Product, scope, architecture, design system, validation, research
├── CMakeLists.txt  Build definition and pinned dependencies
└── build.ps1       Configure, build and copy the executable to dist/
```

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

Third-party notices are compiled into the executable. Open them from **Help**, **Software notices**.

OBSReplay is not affiliated with the OBS Project.

## License

[MIT](LICENSE).
