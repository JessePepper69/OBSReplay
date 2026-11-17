# Product

This file covers what OBSReplay is, who uses it, the product rules every change must keep, and the assumptions and non-goals.

## Contents

- [What it is](#what-it-is)
- [Who it is for](#who-it-is-for)
- [Daily use](#daily-use)
- [Product rules](#product-rules)
- [Interface rules](#interface-rules)
- [Working rules for contributors](#working-rules-for-contributors)
- [Assumptions](#assumptions)
- [Non-goals](#non-goals)

## What it is

- OBSReplay is a Windows instant-replay controller for an installed OBS Studio.
- OBS Studio captures and encodes. OBSReplay sets OBS up, starts the replay buffer, saves replays, lists clips, plays them and edits them.
- The distributable is one `OBSReplay.exe` that contains the UI and all app dependencies. It installs itself per user on first run and removes itself through Windows **Installed apps**.
- OBS Studio 31 or newer and the Microsoft Edge WebView2 Runtime are installed prerequisites. Guided setup installs OBS when it is missing.
- The clip editor exports through an installed FFmpeg. FFmpeg is not bundled.

## Who it is for

- A Windows gamer who wants the last few minutes of play saved with one global hotkey.
- A user who does not want to learn OBS. OBSReplay writes the OBS configuration.
- A user who already runs OBS for streaming or recording. OBSReplay leaves that setup alone.

## Daily use

1. OBSReplay starts with Windows in the tray and launches OBS minimized to the tray.
2. The replay buffer runs in OBS.
3. The user presses the save hotkey (default `Alt+Z`). OBS writes the clip.
4. The user opens the window (default `Alt+X`, or the tray icon) to watch, trim or delete clips.

## Product rules

| Rule | Detail |
| --- | --- |
| Dedicated OBS profile | Setup creates a profile and a scene collection, both named `OBSReplay`. OBSReplay never edits another profile or collection. |
| Respect existing OBS sessions | OBSReplay asks OBS to close through its main window. It never terminates an OBS process. A stream or a full recording blocks setup and Apply. |
| Ownership checks | OBSReplay controls capture only while OBS runs the `OBSReplay` profile and scene collection. |
| Microphone is on by default | `mic_enabled` defaults to `true`. Guided setup shows the **Record microphone** switch before capture starts, and Help states that the voice is recorded while the switch is on. |
| Desktop capture is explicit | **Game or display (Hybrid)** (default) is labelled as recording the display between games. **Fullscreen games only** never falls back to the desktop. **Entire display** records the selected monitor at all times. |
| Start with Windows is on by default | `start_at_login` defaults to `true`. Setup shows the switch, and the user can turn it off. The login entry changes only through a successful setup or **Apply changes**. **Reset integration** and uninstall remove it. |
| Capture starts by itself | `replay_enabled` (**Start replay capture automatically**) defaults to `true`. Only the user changes it, through **Apply changes** or setup. **Start capture**, **Stop capture** and **Reset integration** leave it alone. One exception: the first launch after an update from 1.0.1 turns it on once, because 1.0.1 overwrote it on every start and stop. |
| Profile updates need no click | When a new app version needs the OBS profile rewritten, the app does it at launch while OBS is closed. It never restarts a running OBS for this. |
| Guided first run | First run is a three-step setup: **Install**, **Capture**, **Start**. It installs OBS and FFmpeg when needed, closes and reopens OBS, and starts capture in one step. |
| Tray behavior | Closing the window hides it to the tray. Hotkeys keep working. **Quit OBSReplay** in Help and **Close OBSReplay** in the tray menu exit the controller and leave OBS and any active capture running. The UI states both facts. **Close OBSReplay and OBS** in the tray menu also closes the OBSReplay session of OBS. |
| Stop discards the buffer | Stopping capture discards unsaved replay history. The UI says so on the tray menu and in Help. |
| One Apply model | Settings change only through **Apply changes**. Capture and audio changes restart the dedicated OBS session. App settings do not. |
| Confirmed results only | A clip appears in the list after OBS confirms the save and the file exists. The UI never invents clips, counters or success states. |
| Honest replay length | The UI says "up to" the configured length. It shows no buffer fill meter. |
| Originals stay intact | The editor writes a new file. It never modifies the source replay. The source is deleted only when the user ticks **Delete original clip after exporting**, and only after the export is saved. |
| User keeps control of Windows and OBS | OBSReplay does not change Windows capture settings, Game Bar settings or OBS crash recovery. It needs no admin rights. |
| Local only | OBSReplay talks to OBS on `127.0.0.1`. Clips stay on local disk. Help links open from a fixed allowlist. The one outside connection is the update check: it reads GitHub Releases once a day, and the user can turn it off. |
| Every saved clip stays listed | The clip list has no size limit. A clip leaves it only when its file is gone: deleted in the app, or deleted, moved or renamed outside it. |

## Interface rules

- The main window opens at 1040 × 760 logical pixels with a minimum of 840 × 620.
- The navigation rail holds **Capture** and **Clips** at the top, **Settings** and **Help** at the bottom.
- Settings has four groups: **Capture**, **Audio**, **App** and **Debug**.
- The **Animations** setting (`animations_enabled`) controls app transitions. It does not follow the Windows animation setting.
- Six color themes ship: Original, Mono, Light, Rose, Amethyst, Ember.
- See [design](design.md) for the visual contract.

## Working rules for contributors

- Keep application code at the repository root (`src/`, `frontend/`, `resources/`).
- Avoid new frameworks, abstraction layers and system changes.
- Verify changes with isolated tools: set `OBSREPLAY_DATA_DIR` and `OBSREPLAY_HEADLESS`. Do not drive the user's desktop with mouse or keyboard input.
- See [architecture](architecture.md) for the code and [scope](scope.md) for feature status.

## Assumptions

- Windows x64.
- OBS Studio 31 or newer.
- One local OBS connection.
- Local files only.
- The app folder name on a clip comes from the foreground window at save time. It is a best-effort label and does not prove that OBS hooked that game.

## Non-goals

- Capturing or encoding video inside OBSReplay.
- Replacing OBS for streaming or full-length recording.
- Bundling OBS, WebView2 or FFmpeg in the exe.
- A separate installer package, an admin task or a background service.
- Cloud upload, accounts or remote control.
- Platforms other than Windows x64.
