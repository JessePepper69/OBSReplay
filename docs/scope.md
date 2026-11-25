# Scope

This file lists what OBSReplay includes and excludes, feature by feature, with the status of each item in the code.

## Contents

- [Deliverable](#deliverable)
- [Status legend](#status-legend)
- [Setup](#setup)
- [Capture](#capture)
- [Settings](#settings)
- [Clips and viewer](#clips-and-viewer)
- [Clip editor](#clip-editor)
- [Desktop integration](#desktop-integration)
- [Out of scope](#out-of-scope)
- [Constraints](#constraints)
- [Acceptance evidence](#acceptance-evidence)

## Deliverable

- One release `OBSReplay.exe` with compiled-in HTML, CSS and JavaScript and static C++ dependencies.
- Running the exe needs no Node, no frontend runtime, no sidecar UI, no admin task and no separate installer. The exe installs itself for the current user.
- Installed OBS Studio captures. Installed WebView2 renders the UI. Installed FFmpeg exports edits.

## Status legend

| Status | Meaning |
| --- | --- |
| Done | Present in the code. |
| Out | Excluded on purpose. |

Hardware coverage and test evidence live in [validation](validation.md).

## Setup

| Feature | Status | Notes |
| --- | --- | --- |
| Three-step guided setup (**Install**, **Capture**, **Start**) | Done | Opens on first run while no integration exists. |
| Install FFmpeg through winget | Done | `Gyan.FFmpeg`, from setup step 1 or the editor. **Locate ffmpeg.exe…** accepts an existing copy. |
| Detect installed OBS | Done | Registry key, then `Program Files\obs-studio`. Re-checks every 3 s while no supported OBS is known. |
| Install or update OBS through winget | Done | `OBSProject.OBSStudio`. Windows shows its own permission prompt. |
| Download page and **Locate obs64.exe…** | Done | The located file must be `obs64.exe` version 31 or newer. |
| Close an open OBS before setup | Done | Sends `WM_CLOSE` to the OBS main window. Refuses when OBS reports a stream or recording. |
| Dedicated profile and scene collection | Done | Owner markers guard both. Setup refuses unrelated files with the same names. |
| Authenticated local WebSocket control | Done | Changes to shared OBS WebSocket settings are recorded and backed up. |
| Skip the OBS first-run wizard | Done | Writes `FirstRun=true` to `user.ini`. |
| Launch OBS in the tray and start the buffer | Done | One step from the **Start** page. |
| Profile updates from a new app version | Done | Applied at launch while OBS is closed. No button, and no restart of a running OBS. |
| Rollback on failure | Done | Staged file changes restore in reverse order. |
| Reset integration | Done | Needs OBS closed. Removes owned files, restores shared values that still match. Clips stay. |

## Capture

| Feature | Status | Notes |
| --- | --- | --- |
| Start and stop the replay buffer | Done | Stop discards buffered history. |
| Save by button, tray menu or global hotkey | Done | A save completes when OBS reports the file and the file exists. A failed save is not retried. |
| Screenshot hotkey | Done | Default Alt+S. A full-size PNG of what OBS captures, in the clips folder (the app's folder when sorting by app). A black picture is deleted and reported. Listed on the **Screenshots** tab. Toast with **Show in folder**, or a tray balloon while hidden. A switch in Settings turns the shortcut off and frees its keys. |
| Connection and replay state display | Done | Shows connection, session ownership, pending work, errors and the confirmed file. |
| Reconnect and **Open OBS** | Done | Reconnect never repeats a save. |
| Capture readiness checks | Done | Verifies scene, sources, display and audio devices before start and save. |
| Stop capture when the selected display disconnects | Done | Prevents recording another display. |
| Start capture at launch | Done | Runs when **Start replay capture automatically** (`replay_enabled`) is on and the integration exists. Stopping capture does not turn the setting off. |
| Replay buffer memory sized to the settings | Done | Scales with replay length, quality, resolution and frame rate, up to 75% of installed memory. |
| Low disk space warning | Done | On **Capture** when the clips drive has 5% or less free. |

## Settings

| Feature | Status | Notes |
| --- | --- | --- |
| Replay length 30 s to 10 min | Done | Six choices in the UI. |
| Resolution original, 1440p, 1080p, 720p | Done | |
| Frame rate 30, 60, 120, 144 fps | Done | |
| Encoder: automatic, NVIDIA, AMD, Intel, CPU | Done | Hardware encoders are probed with OBS test tools on the default adapter. |
| Codec H.264, HEVC, AV1 | Done | CPU encoding supports H.264 only. |
| Five quality presets | Done | **Very high** to **Very low**. |
| Capture modes: game or display, fullscreen games, entire display | Done | |
| Desktop and microphone device choice | Done | Devices are validated at apply time. |
| RNNoise and Speex noise suppression | Done | |
| Separate audio tracks | Done | Mix, Desktop, Microphone. |
| Clips folder | Done | Write access is tested before applying. |
| Sort clips into app folders | Done | Best-effort label from the foreground window. |
| Save sound | Done | |
| **Start with Windows** | Done | One switch, in guided setup and in Settings, App. Current-user `Run` entry. |
| Configurable hotkeys with conflict detection | Done | Save, show and screenshot. |
| Animations switch | Done | |
| Six color themes | Done | Applied to every window. |
| One explicit Apply with pending state | Done | Capture and audio changes restart the owned OBS session and resume capture. |

## Clips and viewer

| Feature | Status | Notes |
| --- | --- | --- |
| Clip list | Done | Every replay and edited export, with no limit, from confirmed saves. Videos already in the clips folder are added at launch. |
| **Replays** and **Edited exports** tabs | Done | On **Capture** (latest 8) and **Clips** (24 per page). |
| **Screenshots** tab | Done | On **Clips** only. PNG and JPG pictures in the clips folder (not in Edited Exports), with the same game chips, date filter, search and day headings as clips. A viewer with previous and next, Copy, Show in folder, Open file and Delete. |
| Screenshot a frame in the replay viewer | Done | A camera button saves the frame on screen at full size, taken from the clip file by FFmpeg, as `<clip> frame M-SS.ss.png` next to the clip, listed on **Screenshots**. |
| App filter and paging on **Clips** | Done | |
| Date filter on **Clips** | Done | One menu: Any time, Today, Last 7 days, Last 30 days, This year. |
| Day headings on **Clips** | Done | Today, Yesterday, then the date with a short age ("3d ago", "2w ago", "1m ago", "1y ago"). A date from another year shows its year. |
| Select several clips | Done | A check box on each card, Shift+click or Ctrl+click. Cards that are not selected go pale. A bar offers **Select all**, **Copy**, **Delete** and **Clear selection**. Opening a clip keeps the selection. Escape clears it. |
| Copy clips to the clipboard | Done | As files, from the clip menu or the selection bar. They paste into a chat or a folder. |
| Drag a clip out of the window | Done | Drops the file on a chat or a folder. A drag from a selected card carries the selection. |
| Search on **Clips** | Done | File name, game and saved date: as shown on the card, or by weekday, month name, year, local numeric date or `2026-11-15`. |
| Clip menu | Done | One button per card, or a right-click: play, edit, rename, select, copy, open, show in folder, delete. |
| Rename a clip | Done | Renames the file in its folder and keeps its extension. |
| Thumbnails and clip length | Done | Decoded in the page from the clip file once, then kept on disk as a small low-quality JPEG. |
| In-app viewer with timeline, filmstrip and waveform | Done | The waveform needs FFmpeg. |
| **Show in folder**, **Open file**, **Delete** | Done | Delete asks for confirmation and removes the file. |
| Remove missing files from the list | Done | Checked every second, 200 clips per pass in rotation. |

## Clip editor

| Feature | Status | Notes |
| --- | --- | --- |
| One editor window per clip | Done | Reopening a clip focuses its window. |
| Split, remove, restore, undo, redo | Done | Edit decisions live in the page. |
| Preview quality **Full**, **Half**, **Quarter** | Done | Half and Quarter build a proxy file with FFmpeg. |
| Per-track volume, in preview and in the export | Done | The volume set on a track in **Audio tracks** is the volume it is exported at. The main volume slider only changes how loud the editor plays. |
| Pick the audio tracks to export | Done | A check box per track in **Audio tracks**. All ticked exports the tracks as recorded. A smaller choice leads with a mix of the ticked tracks, then each ticked track. None ticked exports no sound. An unticked track is silent in preview. |
| Export quality 1 to 10 with size estimate | Done | 10 keeps the original video where the file allows it. |
| **Max size** export: 10, 25, 50, 100 or 500 MB | Done | 1 MB is 1,000,000 bytes. The file is measured after encoding and encoded again at a lower bitrate if it is over, up to three more times. It never reports a file over the limit. The video is always H.264, so it plays everywhere. A size the edit cannot meet is shown as unavailable. |
| **Copy** the exported file | Done | Puts the file on the clipboard from the result row. |
| Frame-exact cuts | Done | |
| Named exports in `Edited Exports` | Done | Never overwrites a file. |
| Delete original after exporting | Done | Opt-in checkbox in the export dialog, unticked each time. |
| FFmpeg detection and **Locate ffmpeg.exe** | Done | |
| Warn on quit with unexported cuts or a running export | Done | |
| Export menu beside **Export video** | Done | Vertical (9:16), GIF, MP3, WAV and the frame under the playhead, from one split button. An item that can't run is listed with the reason. |
| Export vertical (9:16) | Done | 1080×1920 H.264. **Fill** crops a window you drag over the picture; **Fit** shows the whole picture over a blurred copy. Quality and **Max size** apply. |
| Export as GIF | Done | Small 360p 12 fps, Medium 480p 15 fps, Large 720p 20 fps. A Compression slider (None to Maximum) uses fewer colours to make the file smaller, up to about a third of the size. No sound. Up to 60 s. |
| Export audio as MP3 or WAV | Done | The ticked tracks at their volumes, mixed to one stereo file. MP3 needs an FFmpeg with libmp3lame. |
| Save this frame (PNG) | Done | The frame under the playhead at full size. |

## Desktop integration

| Feature | Status | Notes |
| --- | --- | --- |
| Single instance | Done | A second launch shows the running window. |
| System tray icon and menu | Done | |
| Hide to tray on close | Done | |
| Show/hide hotkey | Done | |
| Tray notifications for saves and errors while hidden | Done | |
| Volume mixer shows OBSReplay name and icon | Done | |
| NVIDIA overlay notice | Done | When `NVIDIA Overlay.exe` runs, guided setup shows a notice and the app shows a toast: the overlay also uses Alt+Z. Dismissing either hides both for good. |
| Fullscreen video on any monitor | Done | The viewer and the editor fill the monitor the window is on. The controls drop to a strip at the bottom edge and slide up when you point at it. |
| Embedded third-party notices | Done | Opened from Help. |
| Self-install and uninstall | Done | The exe copies itself to `%LOCALAPPDATA%\Programs\OBSReplay` on first run. Uninstall from Settings or Windows **Installed apps**. |
| Updates from GitHub Releases | Done | Daily check, release notes, one-click install, **Skip this version**. |
| **Debug** tab | Done | Resource use of OBSReplay and OBS, version, session log and log export. |

## Out of scope

| Item | Status |
| --- | --- |
| Forced termination of OBS | Out |
| Dismissing dialogs of other processes | Out |
| Changing Windows Game Bar or capture settings | Out |
| Always-admin requirement | Out |
| Editing a personal OBS profile or an active session | Out |
| Arbitrary shell commands or URLs from the page | Out |
| Bundled OBS, WebView2 or FFmpeg | Out |
| A separate installer package | Out |
| Streaming and full-length recording controls | Out |
| Claims of AMD, Intel or anti-cheat compatibility without test evidence | Out |

## Constraints

- Toolchain: CMake, C++23, saucer v8.0.5, plain embedded web UI, Windows APIs.
- Transport: native WinHTTP WebSocket, correlated request IDs, protocol authentication, bounded waits.
- One worker thread runs controller actions in order. The page reads state and submits actions.
- C++ validates every setting. Settings files are written through a temporary file and a rename.
- The page never receives the WebSocket password.
- The page can load `saucer://embedded/index.html` or `saucer://embedded/editor.html` and nothing else.
- OBS WebSocket events do not expose the game hook. App labels stay separate from capture readiness.
- Setup never rewrites OBS configuration while OBS runs.

## Acceptance evidence

- Compile Release and inspect the dependencies and size of the exe.
- Run the native exe. Check the UI at normal size, minimum size and high DPI. Check keyboard focus, animations off, and the setup, error, capturing, saving and settings states.
- Exercise a real installed OBS: setup, authenticated connection, buffer start, a playable saved clip, stop, restart, reconnect, settings persistence, tray and hotkeys.
- Review the implementation, fix material findings, rerun the affected checks.
- Document build, run and prerequisite details and hardware limits in [validation](validation.md).
