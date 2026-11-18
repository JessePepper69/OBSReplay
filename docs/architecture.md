# Architecture

This file covers how the OBSReplay code is organized and how each part works: processes and windows, the native bridge, the controller, the OBS integration, settings, media, the editor, themes and the build.

## Contents

- [Overview](#overview)
- [Source map](#source-map)
- [Process and window model](#process-and-window-model)
- [Self-install](#self-install)
- [Updates](#updates)
- [Native bridge](#native-bridge)
- [Controller](#controller)
- [OBS integration](#obs-integration)
- [Settings and stored files](#settings-and-stored-files)
- [Media serving](#media-serving)
- [Replay viewer](#replay-viewer)
- [Clip editor](#clip-editor)
- [Themes](#themes)
- [Session log and Debug tab](#session-log-and-debug-tab)
- [Build and embedding](#build-and-embedding)

## Overview

- One process, `OBSReplay.exe`, hosts every window. Each window is a saucer `smartview` (WebView2) that loads an embedded page.
- C++ owns state and side effects. The page reads a JSON snapshot and submits actions.
- One controller worker thread talks to OBS over obs-websocket and writes the OBS configuration.
- OBS Studio runs as a separate process and does all capture and encoding.
- FFmpeg runs as hidden child processes for the editor and for viewer waveforms.
- All native code lives in namespace `replay`. `Json` is `nlohmann::json`. Strings that cross the bridge are UTF-8; file paths are wide.

See [product](product.md) for the rules the code must keep and [scope](scope.md) for feature status.

## Source map

### Native (`src/`)

| File | Responsibility |
| --- | --- |
| [`main.cpp`](../src/main.cpp) | Entry point, single instance, main window, tray icon and menu, global hotkeys, show/hide sequence, main-window bridge, clip thumbnails on disk. |
| [`controller.cpp`](../src/controller.cpp), [`controller.hpp`](../src/controller.hpp) | `Controller`: state snapshot, action queue, worker thread, OBS session logic, clip history. |
| [`integration.cpp`](../src/integration.cpp), [`integration.hpp`](../src/integration.hpp) | `Integration`: writes and removes the OBS profile, scene collection and WebSocket settings; probes encoders; reads credentials. |
| [`obs_socket.cpp`](../src/obs_socket.cpp), [`obs_socket.hpp`](../src/obs_socket.hpp) | `ObsSocket`: obs-websocket v5 client on WinHTTP. |
| [`platform.cpp`](../src/platform.cpp), [`platform.hpp`](../src/platform.hpp) | Paths, settings load/validate/save, atomic file writes, OBS discovery and launch/close, monitors, audio devices, process runner, login startup, help links, theme grounds, files to the clipboard, file drag-out, window fullscreen, running-process check. |
| [`media.cpp`](../src/media.cpp), [`media.hpp`](../src/media.hpp) | `ClipMedia`: serves clip files and generated preview files to a WebView2 view with range support. |
| [`editor.cpp`](../src/editor.cpp), [`editor.hpp`](../src/editor.hpp) | `EditorHost`: editor windows, FFmpeg detection, probe, waveform, preview proxies, export, editor bridge. |
| [`updater.cpp`](../src/updater.cpp), [`updater.hpp`](../src/updater.hpp) | `Updater`: asks GitHub for the latest release, downloads and verifies it, starts it as the installer. |
| [`audio_sessions.cpp`](../src/audio_sessions.cpp), [`audio_sessions.hpp`](../src/audio_sessions.hpp) | `AudioSessionLabel`: names the WebView2 audio sessions "OBSReplay" in the Windows volume mixer. |

### Frontend (`frontend/`)

| File | Responsibility |
| --- | --- |
| `index.html` | Main window markup: setup, **Capture**, **Clips**, **Settings**, **Help**, confirm dialog, replay viewer dialog. |
| `app.js` | Main window logic: polling, rendering, settings form, guided setup, clip lists, thumbnails, hotkey recording, entrance animations. |
| `viewer.js` | Replay viewer (`window.ReplayViewer`): playback, canvas timeline, filmstrip, waveform. |
| `editor.html`, `editor.js` | Editor window: edit model, undo/redo, canvas timeline, preview, export dialog, preferences. |
| `theme.js` | Theme list and `window.Theme`. Loaded in `<head>` without `defer`. |
| `tooltip.js` | Custom tooltips from `title`, `data-tip`, `data-tip-title` and `data-tip-keys`, in both pages. |
| `app.css` | Design tokens (Original theme), shared components, main window layout. |
| `themes.css` | Token overrides for the five other themes. |
| `viewer.css`, `editor.css` | Viewer dialog and editor window styles. |
| `fonts/InterVariable.woff2`, `fonts/OFL.txt` | Bundled Inter font and its license. |

### Other

| Path | Responsibility |
| --- | --- |
| `CMakeLists.txt` | Build definition, dependency fetch, notices file, frontend embedding. |
| `src/installer.cpp` | Self-install, update and uninstall. Runs before the app starts. |
| `src/log.cpp` | Session log, build version and commit. |
| `src/resources.cpp` | CPU, memory and GPU use of OBSReplay and OBS Studio. |
| `cmake/build_info.cmake` | Writes `build_info.hpp` (version, Git commit) on every build. |
| `src/tray_menu.cpp` | The tray menu window. `OBSREPLAY_TRAY_PREVIEW=<folder>` writes it to PNG in every theme and exits. |
| `build.ps1` | Configure, build, copy the exe to `dist/`. |
| `resources/logo/` | The logo: `logo.svg` (source of truth), `logo-small.svg` (larger arrow, for 24 px and below), PNG exports at 16 to 512 px. |
| `resources/app.ico` | Icon for the exe, taskbar, tray, install window and shortcut: 16, 20, 24, 32, 40, 48, 64, 128, 256 px. |
| `resources/app.rc.in` | Resource script: manifest, icon (ID 1), notices (`RCDATA` 101), save sound (`WAVE` 102), version info. |

## Process and window model

### Startup

1. `main` calls `handle_install` (`src/installer.cpp`). See [Self-install](#self-install). The steps below run only when it returns nothing.
1. `main` reads `--tray`. With it, the window starts hidden.
2. `main` creates the mutex `Local\OBSReplay.Controller.0.1`. When `OBSREPLAY_DATA_DIR` is set, the name gets a hash of that value as a suffix.
3. If the mutex exists, `main` finds the window whose property `OBSReplay.OwnedWindow.0.1` holds this instance tag, posts the registered message `OBSReplay.ShowWindow.0.1` to it, and exits. The tag is `1` for the normal instance and a hash of `OBSREPLAY_DATA_DIR` for an isolated one, so an isolated run never raises the normal window.
4. `main` checks for the WebView2 Runtime. Without it, a message box offers the Microsoft download page.
5. `main` creates `AudioSessionLabel`, then the saucer application with `quit_on_last_window_closed=false`.
6. `start` creates the main window, the `smartview`, `Controller`, `ClipMedia` and `EditorHost`, exposes the bridge and serves `/index.html`.

### Main window

- Size 1040 × 760 logical pixels, minimum 840 × 620. Both shrink to the monitor work area minus 24 px. The window opens centered.
- Decorations are `partial`. The page draws the title bar (`data-webview-drag`, `data-webview-minimize`, `data-webview-maximize`, `data-webview-close`).
- The view allows navigation to `saucer://embedded/index.html` and blocks every other URL and every new window.
- The view denies all permission requests, disables the context menu and disables WebView2 zoom control.
- The close event calls `Desktop::hide` and blocks the close.
- The view's `fullscreen` event calls `set_window_fullscreen` (`platform.cpp`) and blocks saucer's own handling. saucer 8.0.5 swaps the monitor's x and y, which moves the window off every screen on a monitor that is not at 0,0. `set_window_fullscreen` saves the placement and style, removes the frame styles and covers `rcMonitor` of the window's monitor; leaving fullscreen puts both back. The page leaves fullscreen before the window hides to the tray.

### Tray

- `Desktop::add_tray` adds the icon. `TaskbarCreated` re-adds it after Explorer restarts.
- Left click, keyboard select or double click shows the window.
- `ignore_alt_menu` (main window and editors) drops `WM_SYSCOMMAND` `SC_KEYMENU` with no key. A lone Alt, such as the end of Alt+Z pressed while the window has focus, would otherwise start keyboard menu mode: nothing shows, and the next click is swallowed. Alt+Space still opens the system menu.
- When the newest clip or the error changes while the window is open, `Desktop::reconcile` sends the page `obsreplay:state`. The page polls at once and does not wait for its one-second timer.
- Right click opens the tray menu (`src/tray_menu.cpp`): a borderless popup drawn with GDI+ in the colours of the saved theme. Header: app icon, name, a state dot and one status line. Rows: **Save replay** (with the save shortcut), **Start capture** or **Stop capture**, **Open OBSReplay**, **Close OBSReplay** (OBS keeps running), **Close OBSReplay and OBS**. Rows that cannot run are dim. It closes on Escape, a click elsewhere or lost focus. Arrow keys and Enter work.
- **Close OBSReplay and OBS** dispatches the `shutdown` action. The controller stops the replay buffer, closes the OBSReplay session of OBS through its main window, then sets `quit` in its state. `Desktop::reconcile` ends the app when it reads `quit`. OBS is left open, with a notification, when it runs a different session or is streaming or recording. Settings are not changed. Capture starts at the next launch when **Start replay capture automatically** is on.
- Quit asks for confirmation when `EditorHost::pending()` reports a running export or unexported cuts.
- A 500 ms timer runs `Desktop::reconcile`. While the window is hidden or minimized, it shows a tray balloon for a new error or a new saved clip.

### Global hotkeys

- `parse_hotkey` accepts `Alt`, `Ctrl`/`Control`, `Shift` plus one of `A`-`Z`, `0`-`9`, `F1`-`F24`. One modifier is the minimum.
- `Desktop::bind` registers both hotkeys with `RegisterHotKey` and `MOD_NOREPEAT`. If one fails, it restores the previous pair and returns an error that names the conflicting shortcut.
- The save hotkey dispatches `save`. The show hotkey toggles the window.
- `Desktop::reconcile` rebinds from the settings in the snapshot. While a `setup` or `apply` is pending, it waits until the settings match or the controller finishes.
- `hotkey_recording(true)` unregisters both hotkeys so the page can capture a key combination. Recording ends on `hotkey_recording(false)`, on window focus loss, on hide, or after 45 s.

### Show and hide to tray

Hide (`Desktop::hide`):

1. Stop hotkey recording.
2. If no tray icon exists, minimize the window and stop.
3. Dispatch the page event `obsreplay:hiding`. The page puts its content into the starting state of the entrance animation.
4. Hide the window.
5. After 180 ms, set the WebView2 controller invisible and call `TrySuspend`.

Show (`Desktop::show`):

1. Cancel the suspend timer, call `Resume`, set the WebView2 controller visible.
2. Restore, show and focus the window (`show_headless` in headless mode).
3. Dispatch the page event `obsreplay:reopen`. The page plays the entrance animation from the held state.

First reveal:

- The window stays hidden until the page calls `ui_ready`, or until a 2000 ms timer fires.
- `Desktop::ready` then shows the window, or hides it when the app started with `--tray`.

### Editor windows

- `EditorHost::open` creates one window per clip. Opening a clip that has an editor focuses that window.
- Preferred size is 1200 px wide with a 16:9 preview above 384 px of chrome, limited to the work area minus 32 px. Minimum is 900 × 640.
- Each editor has its own `smartview`, its own `ClipMedia` and its own analysis, preview and export threads.
- The view allows navigation to `saucer://embedded/editor.html` and nothing else.
- The close event dispatches the page event `editor:close-request` and blocks. The page confirms and calls `close_editor`. If the page has not polled for 5 s, the close goes through.

### Isolated and headless runs

| Variable | Effect |
| --- | --- |
| `OBSREPLAY_DATA_DIR` | Absolute path used as the data folder. The run gets its own single-instance mutex and its own WebView2 profile in `<data>\WebView2`. |
| `OBSREPLAY_HEADLESS` | Any value. Windows open off-screen at the bottom of the z-order without focus. A finished export does not open Explorer. Reopening an editor does not focus it. |

OBS configuration paths do not change in an isolated run: they still point at `%APPDATA%\obs-studio`.

## Self-install

`OBSReplay.exe` is its own installer. `handle_install` in `src/installer.cpp` runs first in `main`, before the single-instance check.

| Launch | Result |
| --- | --- |
| From `%LOCALAPPDATA%\Programs\OBSReplay\OBSReplay.exe` | The app runs. |
| From any other path | Install or update, then the installed copy opens. This process exits. |
| From any other path, same bytes already installed and registered | The installed copy opens. No window. |
| With `--uninstall`, from the install folder | Uninstall. |
| With `--portable`, or with `OBSREPLAY_DATA_DIR` or `OBSREPLAY_HEADLESS` set | The app runs in place. Nothing is installed. |

Install steps, on a worker thread:

1. Ask a running OBSReplay to quit. The installer finds the main window by the `OBSReplay.OwnedWindow.0.1` property with value 1 and posts the registered message `OBSReplay.QuitWindow.0.1`. It waits 6 seconds, then ends a copy that did not answer. OBS keeps running.
2. Copy the executable to `OBSReplay.exe.new` in the install folder, then move it over `OBSReplay.exe`. The move retries for 6 seconds.
3. Write the icon to `OBSReplay-<hash>.ico` in the install folder, then `OBSReplay.lnk` to the user's Start menu Programs folder with that icon. Windows caches icons by path and can keep an old picture after an exe is replaced. A name made from the icon's content gives a changed icon a new path. The installer then sends `SHChangeNotify` for the shortcut and the exe.
4. Write `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\OBSReplay`: name, version, icon, location, size, and `"<exe>" --uninstall`.
5. If a `Run\OBSReplay` value exists, point it at the installed copy. If none exists but kept settings have `start_at_login` on, write it.

The window is plain Win32 with GDI drawing: app icon, one state line, one progress bar. It uses no WebView2. It stays open at least 1.1 seconds. A failed install shows the reason, then the app runs from the downloaded file.

Uninstall asks for confirmation, then whether to delete settings. With `--quiet` it asks nothing and keeps settings; Windows uses that as `QuietUninstallString`. It closes the app, resets the OBS integration when OBS is closed, deletes the Run value, the shortcut and the registry key, and optionally `%LOCALAPPDATA%\OBSReplay`. A hidden `cmd` deletes the install folder after the process exits. Clips are never touched.

`OBSREPLAY_INSTALL_TEST=<absolute folder>` is for checks. The install goes to `<folder>\app`, the shortcut to `<folder>`, the registry key is `OBSReplay.Test`. No window shows, nothing launches, and the running app, Run value, settings and OBS integration are left alone.

## Updates

`Updater` in `src/updater.cpp` reads GitHub Releases. It never downloads or installs without a click.

### Flow

1. **Check.** `GET https://api.github.com/repos/JessePepper69/OBSReplay/releases/latest` over WinHTTP, on a worker thread.
2. **Compare.** The release tag (`v1.2.3`) against the version built into the app. A higher tag is an update.
3. **Offer.** The page opens the update dialog with the release notes. The tray menu gains an **Update to 1.2.3** row.
4. **Download.** On **Update now** the asset `OBSReplay.exe` goes to `<data>\updates\OBSReplay-1.2.3.exe`.
5. **Verify.** The size must match the asset's `size`. The SHA-256 must match the asset's `digest` when GitHub publishes one. The file's version resource must name the product `OBSReplay` and carry the tag's version.
6. **Hand over.** The app writes `updated_from` to `updates.json` and starts the downloaded exe. That exe is the installer described in [Self-install](#self-install): it closes this copy, replaces the installed file and opens it.
7. **First start.** The new version sees `updated_from`, shows "Updated to 1.2.3" once and deletes the download.

### Schedule

- First check 10 seconds after launch, skipped when the last check is under an hour old.
- Then once every 24 hours. A failed check retries after an hour.
- **Check now** in Settings checks at once and reports a failure. Scheduled checks fail silently into the session log.
- The **Check for updates automatically** switch turns the schedule off. **Check now** still works.

### Rules

- **Skip this version** stores the version in `updates.json`. That version is not offered again. A newer one is.
- Closing the dialog asks again on the next launch.
- No prompt while the window is hidden, during guided setup or while another dialog is open.
- An editor with a running export or unexported cuts blocks **Update now**.
- A file with the wrong size, hash or version is deleted and reported. The installed app is not touched.
- The download address must start with `https://github.com/JessePepper69/OBSReplay/releases/download/`.
- Release notes are Markdown. The page draws headings, lists, bold, italic, code and links as elements and never reads the text as HTML. Only `https://github.com/` links open, in the default browser.
- A release with a tag that has no version number, or no `.exe` asset, cannot be installed from the app. The dialog links to the releases page.

### Publishing a release

1. Set the version in `project(OBSReplay VERSION x.y.z)` in `CMakeLists.txt`. Commit and push.
2. Build Release from that clean commit.
3. Create a GitHub release with the tag `vx.y.z`. The tag must match step 1.
4. Attach the built file under the name `OBSReplay.exe`.
5. Write the release notes. They are the changelog users see.

Pre-releases and drafts are never offered.

### Checks

An isolated run (`OBSREPLAY_DATA_DIR`) never contacts GitHub. Two more variables work only in such a run:

| Variable | Effect |
| --- | --- |
| `OBSREPLAY_UPDATE_URL` | Address used in place of the GitHub API. Its release may point at `http://127.0.0.1`. |
| `OBSREPLAY_UPDATE_AS` | Version the build pretends to be, so the same build can be offered to itself. |

## Native bridge

The page calls `window.saucer.exposed.<name>(...)`. Each call returns a promise. A function that returns `std::expected` rejects the promise with the error string.

`app.js` and `editor.js` wrap calls in `nativeCall`, which runs them one at a time in order.

### Main window (`main.cpp`)

| Function | Arguments | Returns | Purpose |
| --- | --- | --- | --- |
| `snapshot` | none | JSON string | Controller snapshot plus `hotkey_error` when a hotkey failed to bind. |
| `dispatch` | `action` string, `payload` JSON string | `true` or error | Queue a controller action. See [action queue](#action-queue). |
| `media_url` | `path` string | URL string or error | Playable URL for a clip in history. |
| `open_editor` | `path` string | `true` or error | Open or focus the editor for a clip in history. |
| `editors_pending` | none | string | Text that describes running exports or unexported cuts. Empty when none. |
| `ffmpeg_state` | `refresh` (bool) | JSON string | FFmpeg as the editor host found it: `status` (`checking`, `ready`, `missing`, `invalid`), `version`, and `install` (`stage`, `error`, `available`). `refresh` looks again when FFmpeg is missing. Guided setup step 1 polls it. |
| `install_ffmpeg` | none | `true` or an error | Start `winget install --id Gyan.FFmpeg` on a worker thread. Same call as the editor window's. |
| `pick_ffmpeg` | none | `true`, `false` on cancel, or an error | Native file dialog for `ffmpeg.exe`. The editor host checks the file and adopts it. |
| `clip_waveform` | `path` string | JSON string | Viewer waveform. `{"status":"ready","rate":100,"peaks":"<base64>"}` or `{"status":"unavailable"}`. A new call cancels a running decode. |
| `pick_folder` | `current` string | path or empty string | Native folder dialog for the clips folder. |
| `pick_obs` | `current` string | path or empty string | Native file dialog for `obs64.exe`. |
| `set_theme` | theme id | `true` or an error | Save a colour theme at once, outside the action queue. Picking a theme in Settings calls it, so no Apply is needed. Refused while an action runs. |
| `installed` | none | bool | True when this process is the installed copy. Settings shows **Uninstall OBSReplay** only then. |
| `uninstall` | none | `true` or an error | Start the installed copy with `--uninstall`. The uninstaller confirms, then closes this app. |
| `app_info` | none | JSON | `{version, commit, commit_short, dirty, built, full, logs}`. |
| `resources` | none | JSON | Resource use. See [Session log and Debug tab](#session-log-and-debug-tab). |
| `log_tail` | `lines` int | text | The last lines of the session log (600 at most). |
| `export_log` | none | path, `""` or an error | Save dialog, then write the session log with the current state and resource use. Empty when cancelled. |
| `open_logs` | none | `true` or an error | Open the logs folder. |
| `ui_log` | `level`, `text` | nothing | Add a line from the page: page changes and script errors. |
| `update_state` | none | JSON string | `{enabled, status, current, latest, available, skipped, received, total, error, error_stage, checked_at, just_updated, previous, releases, blocked}`. `status` is `idle`, `checking`, `current`, `available`, `downloading`, `verifying`, `installing` or `error`. |
| `update_check` | none | nothing | Ask GitHub now. |
| `update_skip` | `skipped` | nothing | Hide the available version, or bring it back. |
| `update_enabled` | `enabled` | nothing | Turn scheduled checks on or off. |
| `update_start` | none | `true` or an error | Download, verify and install the available version. Refused while an editor has unfinished work. |
| `update_cancel` | none | nothing | Stop a download. |
| `update_seen` | none | nothing | The "Updated to" toast was shown. |
| `open_link` | `url` | `true` or an error | Open a `https://github.com/` address in the default browser. |
| `thumbnail_read` | `path` | JSON string or `""` | The stored picture of a listed clip: `{data, length}`. `data` is a JPEG data URL, `length` the clip length in seconds (0 when unknown). |
| `thumbnail_write` | `path`, `data`, `length` | bool | Store the picture of a listed clip. `data` must be a JPEG data URL of at most 96 KiB. |
| `overlay_running` | none | bool | A process named `NVIDIA Overlay.exe` is running. |
| `copy_clips` | JSON array of paths | number of files, or an error | Put listed clips on the clipboard as files (`CF_HDROP`, preferred effect copy). At most 500. |
| `drag_clips` | JSON array of paths | nothing | Start an OLE drag of listed clips out of the window. The page calls it while the left button is held and the pointer has moved 8 px. The drag runs after the call returns, through `app->post`, because it has its own message loop. Does nothing in headless mode. |
| `hide` | none | nothing | Hide to tray. |
| `hotkey_recording` | `enabled` bool | nothing | Suspend or restore global hotkeys while the page records a shortcut. |
| `ui_ready` | none | nothing | First frame is ready; reveal the window. |
| `theme_changed` | `id` string | nothing | Repaint the native window ground and set force-dark for the theme. |
| `quit` | none | nothing | Exit the controller. OBS keeps running. |

For `setup` and `apply`, `Desktop::dispatch` runs before the controller:

1. Reject when another settings change is pending.
2. Parse the payload. Take `settings` from a `{settings, start}` wrapper when present. Run `validate_settings` on the settings object.
3. Bind the new hotkeys. Return the bind error on conflict.
4. Queue the action. If the controller rejects it, rebind the previous hotkeys.


### Editor window (`editor.cpp`)

| Function | Arguments | Returns | Purpose |
| --- | --- | --- | --- |
| `editor_state` | none | JSON string | Editor state. See [editor state](#editor-state). Also marks the page alive. |
| `editor_waveform` | none | base64 string | Audio peaks, 100 per second. Empty until the waveform is ready. |
| `media_url` | `path` string | URL string or error | URL of this editor's clip. Any other path is refused. |
| `preview_urls` | none | JSON string | `{video, level, stems:[{track, url}]}` for the generated preview files. `video` is empty at level `full`. |
| `start_export` | JSON string `{segments:[[start,end],...], quality, name?, size_limit?, tracks?}` | `true` or error | Validate and start an export. `size_limit` is 0 or 10, 25, 50, 100, 500 (MB). `tracks` is an array of recorded track numbers to keep, or null for every track. `volumes` is an object of track number to a volume from 0 to 1, or null for full volume. |
| `cancel_export` | none | nothing | Cancel the running export. |
| `pick_ffmpeg` | none | `true`, `false` on cancel, or error | Native file dialog, then FFmpeg detection with that file. |
| `ffmpeg_help` | none | nothing | Open the FFmpeg download page. |
| `install_ffmpeg` | none | `true` or an error | Start `winget install --id Gyan.FFmpeg` on a worker thread. `editor_state` reports progress in `ffmpeg.install` (`stage`: `preparing`, `downloading`, `installing`, `verifying` or empty; `error`; `available`: winget found). On success the host adopts the new `ffmpeg.exe` and every open editor continues its analysis. |
| `reveal_output` | `reveal` bool | `true` or error | Show the last export in Explorer (`true`) or open it (`false`). |
| `play_output` | none | `true` or error | **Play video**: show the main window and send it `obsreplay:play` with the export's path. The page opens **Clips**, **Edited exports** and plays the file in the viewer. |
| `copy_output` | none | `true` or error | **Copy**: put the last export on the clipboard as a file (`copy_files_to_clipboard`). |
| `close_editor` | none | nothing | Cancel any export and close the window. |
| `editor_dirty` | `value` bool | nothing | Tell native whether unexported cuts exist. |
| `save_pref` | `key` string, `value` JSON string | `true` or error | Validate and store an editor preference. See [editor preferences](#editor-preferences). |
| `retry_analysis` | none | nothing | Run clip analysis again. |
| `ui_log` | `level`, `text` | nothing | Add a line from the editor page to the session log, prefixed "Editor:": messages it showed, script errors and video errors. |
| `theme_changed` | `id` string | nothing | Same as the main window, for this editor window. |

## Controller

`Controller` owns one worker thread (`Impl::run`), one `ObsSocket`, one `Integration` and the state object. A mutex guards the state and the queue.

### State snapshot

`Controller::snapshot()` returns the state as a JSON string under the mutex. `Impl::update` increments `revision` whenever a change alters the state.

| Key | Type | Meaning |
| --- | --- | --- |
| `settings` | object | Current settings. See [settings keys](#settings-keys). |
| `options` | object | `monitors` (`id`, `label`, `width`, `height`, `primary`), `audio_inputs`, `audio_outputs` (`id`, `label`), `encoders` (`id`, `label`, `codecs`). |
| `integration` | bool | The dedicated profile, scene collection and manifest exist with matching owner markers. |
| `compression_update` | bool | The manifest's `compression_policy` differs from the code's: the OBS profile is waiting to be rewritten. The page does not show it. See [Profile updates](#profile-updates). |
| `obs_found` | bool | `obs_path` is a file. |
| `obs_version` | string | `major.minor.patch` from the exe version resource. |
| `obs_supported` | bool | Major version is 31 or higher. |
| `winget` | bool | The winget alias exists. |
| `install_stage` | string | `""`, `preparing`, `downloading`, `installing`, `verifying`. |
| `setup_stage` | string | `""`, `closing`, `configuring`, `launching`, `starting`, `done`. |
| `obs_running` | bool | An `obs64.exe` process exists. |
| `connected` | bool | The WebSocket is identified. |
| `owned` | bool | OBS runs the `OBSReplay` profile and scene collection. |
| `replay_active` | bool | The replay buffer is running. |
| `capture_ready` | bool | Devices and the scene match the settings. |
| `capture_issue` | string | Reason when `capture_ready` is `false`. |
| `busy` | bool | An action is queued or running. |
| `operation` | string | Name of that action. `initializing` at startup. |
| `status` | string | `initializing`, `Setup required`, `Disconnected`, `Different OBS session`, `Capturing`, `Ready`. |
| `message` | string | Last progress or result text. |
| `error` | string | Last error text. Cleared when an action starts. |
| `revision` | integer | Change counter. |
| `clips` | array | Newest first. Each: `path`, `name`, `app`, `bytes`, `saved_at` (local ISO time), `edited` (`true` on exports only). |
| `hotkey_error` | string | Added by `main.cpp`, not by the controller. |

### Action queue

`Controller::dispatch(action, payload)` accepts an action or returns an error. Acceptance does not mean completion.

- Unknown actions are rejected.
- The payload must be a JSON object of at most 128 KiB.
- A dispatch while `busy` is `true` is rejected. The queue holds at most 4 actions.
- On acceptance the controller sets `busy`, `operation`, a working `message`, clears `error` and increments `revision`.
- For `save`, the controller reads the foreground app name at dispatch time.
- The worker runs one action at a time. A thrown error becomes `error` with the message "Action needs attention".

| Action | Payload | Effect |
| --- | --- | --- |
| `setup` | settings object, or `{settings, start}` | Run `apply` with a forced profile rewrite. With `start: true`, launch OBS and start the buffer. Reports `setup_stage`. |
| `apply` | settings object | Validate, store, and when capture settings changed, rewrite the OBS profile. See [apply](#apply). |
| `install_obs` | none | Run `winget install` (or `upgrade` when OBS exists) for `OBSProject.OBSStudio`, then re-detect. Reports `install_stage`. |
| `locate_obs` | `{path}` | Accept an absolute `obs64.exe` of version 31 or newer and store it as `obs_path`. |
| `start` | none | Launch OBS if closed, connect, verify ownership and readiness, send `StartReplayBuffer`, wait up to 5 s for confirmation. The `replay_enabled` setting is not changed. |
| `stop` | none | Send `StopReplayBuffer`, wait up to 5 s. The `replay_enabled` setting is not changed. |
| `shutdown` | none | Tray **Close OBSReplay and OBS**: stop the buffer, close the OBSReplay session of OBS, set `quit` in the state. |
| `save` | none | Save the replay buffer. See [replay save](#replay-save). |
| `reconnect` | none | Close the socket and connect again. Does not launch OBS. |
| `reset` | none | With OBS closed: remove the integration, delete the login entry, set `start_at_login` to `false`. `replay_enabled` is kept. |
| `refresh_options` | none | Re-read monitors, audio devices and encoders. |
| `open_folder` | none | Create and open `clips_dir`. |
| `open_obs` | none | Launch the dedicated session, or restore the running OBS main window. |
| `open_clip` | `{path}` | Open a clip from history in the default app. |
| `reveal_clip` | `{path}` | Select a clip from history in Explorer. |
| `delete_clip` | `{path}` | Delete a clip file from history and drop it from the list. |
| `delete_clips` | `{paths}` | Delete several listed clip files (at most 5000). A file that is already gone counts as deleted. Reports how many could not be deleted. |
| `rename_clip` | `{path, name}` | Rename a clip file from the list in its folder. Keeps the extension, the place in the list and the saved time. Refuses a name another file has. |
| `help` | `{topic}` | Open a fixed URL for `obs`, `webview2`, `ffmpeg` or `capture`, open Windows storage settings for `storage`, or write and open the notices file for `notices`. |

### Worker loop

- `initialize` loads settings, detects OBS, loads `clips.json`, runs `reconcile_clips` and `discover_clips`, reads options, applies a pending [profile update](#profile-updates), then starts capture when `replay_enabled` is `true` and the integration exists.
- The loop waits up to 100 ms for an action, runs it, then drains WebSocket events.
- Once per second it runs `reconcile_clips(false)` (drops list entries whose file is gone, 200 clips per pass in rotation) and `background_connection`.
- Every 3 s it runs `detect_obs` while no supported OBS is known.
- `background_connection` connects when OBS runs and the integration exists. Failed attempts back off: 2 s plus 2 s per failure, capped at 15 s.
- While connected, `refresh` runs every 5 s and after a profile or collection change event.

### Events

| OBS event | Reaction |
| --- | --- |
| `CurrentProfileChanging`, `CurrentProfileChanged`, `CurrentSceneCollectionChanging`, `CurrentSceneCollectionChanged` | Set `owned=false`, refresh on the next pass. |
| `ReplayBufferStateChanged` | Update `replay_active`. |
| `ExitStarted` | Clear `connected`, `owned`, `replay_active`. |
| `ReplayBufferSaved` | Confirm the pending save. With no pending save and an owned session, add the file to history with an empty app. |

### Page polling

- `app.js` polls `snapshot` every 1000 ms, or every 500 ms while an action is pending or `busy` is `true`.
- Polling stops while the document is hidden and no action is pending.
- A poll with an unchanged `revision` and `hotkey_error` skips rendering.
- After `dispatch` succeeds, the page treats the action as finished when `busy` is `false` and either `revision` changed or `busy` was seen `true`. It gives up after 120 s.

### Clip history

- `clips.json` holds every replay and edited export, newest first. The list has no size limit.
- An entry leaves the list only when its file is gone. `reconcile_clips()` checks the whole list at startup, after a delete, and when a rename finds its file gone. The periodic `reconcile_clips(false)` checks `reconcile_batch` (200) entries per pass and continues from there on the next pass.
- `discover_clips` runs at startup and when `clips_dir` changes. It adds videos in the clips folder that the list does not hold, up to two folders deep. The first folder is the app label, an `Edited Exports` folder marks an export, and the file's write time is the saved time. It then sorts the list by saved time.
- Accepted extensions: `.mp4`, `.mkv`, `.mov`, `.flv`, `.ts`.
- `remember_clip` adds a confirmed replay. `Controller::add_clip` adds an edited export with `edited: true`.
- `Controller::knows_clip` gates every clip action, media URL and editor open.

## OBS integration

### Files written

`Integration::prepare` writes these in one transaction while OBS is closed. `<obs>` is `%APPDATA%\obs-studio`; `<data>` is the data folder.

| File | Content |
| --- | --- |
| `<obs>\basic\profiles\OBSReplay\basic.ini` | Profile: advanced output, MP4 with `movflags=+faststart`, replay buffer, video and audio settings. |
| `<obs>\basic\profiles\OBSReplay\recordEncoder.json`, `streamEncoder.json` | Encoder settings. |
| `<obs>\basic\profiles\OBSReplay\.obsreplay-owner.json` | Owner marker for the profile. |
| `<obs>\basic\scenes\OBSReplay.json` | Scene collection with one scene named `OBSReplay`. |
| `<obs>\basic\scenes\OBSReplay.owner` | Owner marker for the scene collection. |
| `<obs>\plugin_config\obs-websocket\config.json` | Shared file. Sets `server_enabled`, `auth_required`, `first_load=false`; fills `server_password` and `server_port` (4455) when missing or invalid. |
| `<obs>\user.ini` | Adds `FirstRun=true` under `[General]`. Keeps the BOM and line endings. |
| `<data>\integration.json` | Manifest: `type`, `owner`, `created_at`, `updated_at`, `socket_existed`, `socket_before`, `socket_applied`, `compression_policy`. |
| `<data>\websocket.original.json` | Copy of the WebSocket config before the first setup. |
| `<data>\settings.json`, `settings.backup.json` | Resolved settings and the previous settings. |

Rules in `prepare`:

- It refuses to run while `obs64.exe` exists (`require_closed`), and checks again before each file write.
- It refuses an existing `OBSReplay` profile or scene collection whose owner marker does not match the manifest.
- It refuses paths that contain a symlink or reparse point.
- It tests write access to `clips_dir` with a temporary file.
- `commit_changes` restores every file in reverse order when a write fails.
- It returns resolved settings: `obs_path`, `monitor_id` and a concrete `encoder` in place of `auto`.

`Integration::reset` removes owned files whose markers match and restores each shared WebSocket value that still equals the applied value.

### Profile values

| Setting | Profile effect |
| --- | --- |
| `clips_dir` | `RecFilePath`. File names use `%CCYY-%MM-%DD %hh-%mm-%ss` with prefix `Replay`. |
| `clip_seconds` | `RecRBTime`. `RecRBSize` (the most memory the buffer may use, in MB) comes from `memory_limit`: `seconds × (video kbps + audio kbps) ÷ 8 ÷ 1024 × 1.25`. Video kbps is `bitrate_ceiling` for these settings (see [Encoders](#encoders)), doubled for AMD, Intel and CPU because constant quality has no bitrate limit. Audio is 160 kbps per track. The result is never below the earlier figure (`min(seconds × 8, installed MB ÷ 10)`, 256 to 2048 MB) and at most 75% of installed memory, the most OBS's own settings accept. OBS holds only what the encoder produced, so the figure is a ceiling and not a reservation. |
| `output_height` | `OutputCY`. `0` or a value above the display height uses the display height. Width keeps the aspect ratio, rounded to even. |
| `fps` | `FPSInt`. |
| `separate_tracks` | `RecTracks=7` (Mix, Desktop, Microphone) or `1`. Audio is AAC at 160 kbps per track, 48 kHz stereo. |

### Capture modes

| `capture` | Scene items, bottom to top |
| --- | --- |
| `auto` | `OBSReplay Display` (`monitor_capture`), `OBSReplay Game` (`game_capture`, `any_fullscreen`) |
| `game` | `OBSReplay Game` |
| `desktop` | `OBSReplay Display` |

- Desktop audio is `wasapi_output_capture` (`OBSReplay Desktop Audio`) unless `desktop_device` is `off`.
- The microphone is `wasapi_input_capture` (`OBSReplay Microphone`) when `mic_enabled` is `true`, with a `noise_suppress_filter_v2` filter when `noise` is `rnnoise` or `speex`.
- `scene_issue` checks the live scene over the WebSocket: current scene name, item count, order, enabled state, input kinds, `monitor_id` and `capture_mode`.
- `device_issue` checks that the selected display, microphone and desktop device are present.

### Encoders

`encoder_options` probes the default DXGI adapter with the test tools in the OBS `bin\64bit` folder.

| Adapter vendor | Tool | `encoder` id | OBS encoder ids |
| --- | --- | --- | --- |
| NVIDIA (`0x10de`) | `obs-nvenc-test.exe` | `nvidia` | `obs_nvenc_<codec>_tex` |
| AMD (`0x1002`) | `obs-amf-test.exe` | `amd` | `h264_texture_amf`, `h265_texture_amf`, `av1_texture_amf` |
| Intel (`0x8086`) | `obs-qsv-test.exe` | `intel` | `obs_qsv11_v2`, `obs_qsv11_hevc`, `obs_qsv11_av1` |
| any | none | `cpu` | `obs_x264` (H.264 only) |

- The options list is `auto`, the detected hardware encoder if any, then `cpu`.
- `auto` resolves to the hardware encoder when one exists, otherwise `cpu`.

Rate control by `quality` (16, 18, 20, 24, 28):

| Encoder | Mode | Values in quality order |
| --- | --- | --- |
| `cpu` | CRF, preset `veryfast`, profile `high` | 18, 20, 22, 25, 28 |
| `amd` | CQP, preset `balanced` | 20, 22, 24, 27, 30 |
| `intel` | CQP, `TU4`, 2 B-frames | 20, 22, 24, 27, 30 |
| `nvidia` | CQVBR, preset `p5`, tune `hq`, multipass `qres`, adaptive quantization, 2 B-frames | target quality 22, 24, 26, 29, 32 |

- `bitrate_ceiling` gives 24000, 16000, 10000, 7000, 4000 kbps at 1080p60 in quality order. The code scales them by pixel count and frame rate, multiplies by 1.5 for H.264, and clamps to 1000..500000 kbps. NVIDIA encodes with this value as `max_bitrate`. Every encoder uses it to size the replay buffer memory.

### Profile updates

A new app version can need the OBS profile written again, for example for a new buffer memory limit or new encoder values. `compression_policy` in `integration.cpp` (now 3) is the version of the profile. Raising it marks every existing profile as out of date.

- `initialize` runs `Integration::prepare` with the saved settings when the profile is out of date and OBS is closed. This happens before OBS is opened, so the user does nothing and no buffer is lost.
- While OBS is open the profile is left alone. It is rewritten at the next launch that finds OBS closed, or with the next Apply that changes a capture setting.
- A failed rewrite (for example a disconnected microphone) is written to the session log and tried again at the next launch.
- The page shows nothing for a pending update, and an Apply of app settings does not restart OBS for it.
- Every encoder uses a 2 s keyframe interval.

### Launching and closing OBS

- `launch_obs` starts `obs64.exe --profile "OBSReplay" --collection "OBSReplay" --minimize-to-tray --disable-updater` from the OBS folder. It refuses when OBS already runs.
- `ensure_connection(true)` launches OBS, then retries the connection every 600 ms for up to 25 s.
- `close_obs(exe)` posts `WM_CLOSE` to the main window (title starts with `OBS `) of the one process that runs the configured exe, then waits up to 20 s.
- `close_all_obs` does the same for every `obs64.exe` process. Setup uses it for sessions it does not own.
- Before `close_all_obs`, `other_session_busy` connects with the shared WebSocket credentials and refuses when a stream or recording is active.
- No code path terminates an OBS process.

### WebSocket connection

- `ObsSocket::connect` opens `ws://127.0.0.1:<port>/` with WinHTTP, no proxy, subprotocol `obswebsocket.json`.
- It waits up to 5 s for `Hello`, requires authentication, and sends `Identify` with `rpcVersion` 1 and `eventSubscriptions` 67 (General, Config, Outputs).
- The authentication string is `base64(sha256(base64(sha256(password + salt)) + challenge))`.
- A reader thread parses messages (1 MiB cap) and queues up to 128 events.
- `request` allows one pending request at a time, uses ids `replay-<n>`, and times out after 5000 ms by default.
- `Integration::credentials` reads the port and password from the shared `config.json`. The password never enters the snapshot.

### Apply

`Impl::apply(payload, setup)`:

1. Validate the settings.
2. Decide `prepare`: `true` for setup, a missing integration, or a change in any capture key (`obs_path`, `clips_dir`, `clip_seconds`, `capture`, `monitor_id`, `output_height`, `fps`, `encoder`, `codec`, `quality`, `mic_enabled`, `mic_device`, `desktop_device`, `noise`, `separate_tracks`).
3. Without `prepare`: update the login startup entry if it changed, store the settings and stop here. OBS keeps running.
4. With `prepare` and an owned OBS session: refuse on an active stream or recording, stop the buffer, close OBS with `close_obs`. The buffer is not saved first; it starts again from empty.
5. With `prepare` and any other OBS session: check `other_session_busy`, then `close_all_obs`.
6. Update the login startup entry when it changed or on setup.
7. Run `Integration::prepare`.
8. After an owned-session restart: relaunch OBS, verify ownership, resume the buffer if it was active.
9. On setup with `start: true`: launch OBS and run `start`.

On failure the controller restores the previous login entry and, after an owned-session restart, relaunches OBS and resumes capture.

### Replay save

1. Verify connection, ownership, an active buffer and capture readiness.
2. Record a pending save with the app label, `sort_by_app` and `save_sound`.
3. Send `SaveReplayBuffer` (5 s timeout).
4. Wait up to 12 s for a `ReplayBufferSaved` event whose file exists.
5. `remember_clip`: with `sort_by_app`, move the file into a subfolder named after the app (`safe_name`, numbered suffix on collision); add it to history; write `clips.json`; play the save sound.
6. Without confirmation, report an error. The controller does not retry the save.

## Settings and stored files

### Settings keys

`validate_settings` in `platform.cpp` checks every key. `load_settings` merges the stored file over the defaults.

| Key | Type | Default | Validation |
| --- | --- | --- | --- |
| `obs_path` | string | detected `obs64.exe`, or `""` | Empty, or an absolute path named `obs64.exe`. |
| `clips_dir` | string | `<Videos>\OBSReplay` | Absolute path. |
| `clip_seconds` | integer | `120` | 30..600. |
| `capture` | string | `auto` | `auto`, `game`, `desktop`. |
| `monitor_id` | string | `""` | String. Empty selects the primary display at prepare time. |
| `output_height` | integer | `1080` | `0`, `720`, `1080`, `1440`. `0` means original. |
| `fps` | integer | `60` | `30`, `60`, `120`, `144`. |
| `encoder` | string | `auto` | `auto`, `nvidia`, `amd`, `intel`, `cpu`. |
| `codec` | string | `h264` | `h264`, `hevc`, `av1`. `cpu` requires `h264`. |
| `quality` | integer | `18` | `16`, `18`, `20`, `24`, `28`. |
| `mic_enabled` | bool | `true` | Boolean. |
| `mic_device` | string | `default` | `default` or a device id. |
| `desktop_device` | string | `default` | `default`, `off` or a device id. |
| `noise` | string | `off` | `off`, `rnnoise`, `speex`. |
| `separate_tracks` | bool | `true` | Boolean. |
| `sort_by_app` | bool | `true` | Boolean. |
| `save_sound` | bool | `true` | Boolean. |
| `start_at_login` | bool | `true` | Boolean. |
| `save_hotkey` | string | `Alt+Z` | `Ctrl`/`Alt`/`Shift` modifiers, then one of `A`-`Z`, `0`-`9`, `F1`-`F24`. Must differ from `show_hotkey`. |
| `show_hotkey` | string | `Alt+X` | Same shape as `save_hotkey`. |
| `replay_enabled` | bool | `true` | Boolean. **Start replay capture automatically**: capture starts at launch. `start`, `stop` and `reset` do not change it. |
| `settings_version` | integer | `2` | Not validated. A stored file without it was written by 1.0.1 or earlier: `load_settings` sets `replay_enabled` to `true` once, adds the version and saves. Up to 1.0.1, start and stop overwrote `replay_enabled`, so the stored value was not a choice. |
| `animations_enabled` | bool | `true` | Optional. Boolean when present. |
| `theme` | string | `original` | Optional. 1..32 characters of `a`-`z`, `0`-`9`, `-`. |

- String values allow at most 32700 bytes, no control characters, valid UTF-8.
- `start_at_login` writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\OBSReplay` as `"<exe>" --tray`.
- At launch, with `start_at_login` on, a missing entry or one whose executable no longer exists is written again. Isolated runs (`OBSREPLAY_DATA_DIR`) skip this.

### Data folder

The data folder is `%LOCALAPPDATA%\OBSReplay`, or `OBSREPLAY_DATA_DIR`.

| File | Written by | Content |
| --- | --- | --- |
| `settings.json` | `save_settings`, `Integration::prepare` | Settings object. |
| `settings.backup.json` | same | Previous valid settings. |
| `clips.json` | controller | Clip history array. |
| `integration.json` | `Integration` | Ownership manifest. |
| `websocket.original.json` | `Integration::prepare` | WebSocket config before first setup. |
| `editor.json` | `EditorHost` | `ffmpeg_path` and editor preferences. |
| `updates.json` | `Updater` | `enabled`, `skipped`, `checked_at`, the latest release seen, `updated_from`. |
| `updates\` | `Updater` | A downloaded update. Emptied 10 seconds after each launch. |
| `THIRD-PARTY-NOTICES.txt` | `open_help("notices")` | Notices extracted from the exe. |
| `thumbnails\` | `main.cpp` | One `<key>.json` per clip: `{data, length}`. The key is a SHA-256 of the clip's size, creation time and write time, so a renamed or moved clip keeps its picture and a file whose content changes gets a new one. A thread looks every 10 seconds and, whenever the list of clip paths has changed, removes the pictures no listed clip uses: a deleted clip's picture goes within 10 seconds. |
| `WebView2\` | WebView2 | Browser profile, in isolated runs only. |

- `atomic_write` writes a temporary file, flushes it, and renames it over the target.
- Unreadable settings fall back to defaults with an error. The original file stays on disk.
- The page keeps five `localStorage` keys: `obsreplay.theme`, `obsreplay.clipTabs`, `obsreplay.viewerVolume`, `obsreplay.diskDismissed`, `obsreplay.overlayDismissed`.

### Temporary files

- Editor previews and export work files live in `%TEMP%\OBSReplay-preview\<pid>-<editor id>-<suffix>`.
- Each editor deletes its folder on close. `sweep_previews` deletes folders of dead processes at startup.

## Media serving

`ClipMedia` registers a `WebResourceRequested` filter for `https://replay.obsreplay.invalid/*` on one WebView2 view.

- `url(path)` returns `https://replay.obsreplay.invalid/<64 hex chars>` for a clip that `Controller::knows_clip` accepts. The same clip keeps its URL. URLs of clips that left history are dropped.
- `generated_url(path)` does the same for files the editor generated, without the history check.
- `respond` answers `GET`, `HEAD` and `OPTIONS`. Unknown URLs get 404.
- A `Range` request gets a 206 response limited to a 4 MiB window. WebView2 reads a custom response stream to the end before delivering it, so open-ended ranges would read the rest of the file.
- `Content-Type` is `audio/mp4` for `.m4a`, `video/mp4` for `.mp4` and `.mov`, `video/x-matroska` otherwise.
- Responses carry `Access-Control-Allow-Origin: *`, `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`.
- `FileRange` is the read-only `IStream` over one byte range of a file.

## Replay viewer

- The viewer is the `<dialog id="replay-player">` in `index.html`, driven by `viewer.js` through `window.ReplayViewer` (`open`, `release`, `play`, `pause`, `isOpen`, `isPlaying`, `clip`, `inspect`).
- `app.js` opens it with a clip and two callbacks: `mediaUrl` (bridge `media_url`) and `waveform` (bridge `clip_waveform`).
- Playback uses a `<video>` element, so codec support comes from WebView2 and Windows.
- The canvas timeline has a ruler, a filmstrip lane, a waveform lane and an overview bar. It supports zoom, scroll, scrubbing and frame steps.
- The filmstrip comes from a second muted `<video>` that seeks through the clip, coarse pass first.
- The waveform comes from `EditorHost::waveform`: FFmpeg decodes the first audio track to 8 kHz mono, and the code keeps one peak (0..255) per 80 samples. One decode runs at a time. The last result is cached by path, size and write time.
- Without FFmpeg the waveform lane stays flat.
- `app.js` owns the clip actions: **Delete**, **Show in folder**, **Open file**, **Edit clip**.
- Each clip card has one menu, opened by its button or a right-click: **Play**, **Edit clip**, **Rename**, **Open in video player**, **Show in folder**, **Delete**. **Delete** is last and red. A click asks first. Shift+click deletes at once.
- **Clips** search matches every typed word against the file name, the game and the saved date. The date is matched as the card shows it and as weekday, month name, year, local numeric date and `2026-11-15`.
- `formatBytes` shows a size in the largest unit that keeps the number at 1 or more: KB, MB, GB or TB.
- Clip list thumbnails come from `takeThumbnail` in `app.js`: a muted `<video>` draws one frame to a 320 × 180 canvas, saved as a JPEG at quality 0.6 (about 10 KB). The page asks `thumbnail_read` first and decodes the video only when nothing is stored, then stores the result with `thumbnail_write`.
- The Clips page keeps a selection (a set of paths) apart from the list. `clipView.shown` is the list after the date filter, search and game filter, across all pages; **Select all** works on it. A path that leaves the clip list leaves the selection.
- The date filter (`clipView.when`) keeps clips saved today, in the last 7 or 30 days, or this calendar year. Day headings come from each clip's `saved_at`.

## Clip editor

### FFmpeg detection

- `ffmpeg_candidates` checks, in order: the saved `ffmpeg_path`, absolute `PATH` entries, WinGet links and packages, Scoop shims, `%ProgramFiles%\ffmpeg\bin`, Chocolatey, `C:\ffmpeg\bin`.
- `inspect_ffmpeg` runs `-version` and `-encoders`, then test-encodes a 0.2 s clip with `h264_nvenc` and `hevc_nvenc`.
- Status values: `checking`, `ready`, `missing`, `invalid`. A build needs `libx264` or a working `h264_nvenc`.
- Detection runs on a thread. The first `ready` result is saved as `ffmpeg_path`.

### Analysis

`Session::analyze` runs on a thread after the window opens and after `retry_analysis`:

1. Probe: run `ffmpeg -i <clip>` and parse stderr for duration, bitrate, codec, pixel format, size, fps, audio track count and track names (`title` or `name` tags).
2. Keyframes: `list_keyframes` lists every video packet with `-c copy -f framecrc`. It records clean cut points (closed-GOP keyframes whose decode index equals their frame number), the frame count and the decode delay from B-frames.
3. Stems: `build_stems` copies audio tracks to `audio-<n>.m4a` without re-encoding. With three or more tracks, or two tracks where the first is named `mix`, it skips the first track. It makes at most 6 stems.
4. Waveform: `waveform_pass` computes peaks for the first audio track.
5. Preview: apply the saved `preview_quality`.

### Preview proxies

| Level | Source | Build |
| --- | --- | --- |
| `full` | Original clip | None. |
| `half` | `preview-half.mp4` | Half source height (minimum 144 px, even). |
| `quarter` | `preview-quarter.mp4` | Quarter source height. |

- Each level is built once per editor and kept until the editor closes. A level change cancels a running build.
- A level change during analysis is recorded and applied when analysis ends.
- Proxies use a 6-frame GOP with no B-frames and no audio.
- Attempt order: CUDA decode with `scale_cuda` and `h264_nvenc` (`p2`, CQ 21); CPU decode with `h264_nvenc`; `libx264` (`veryfast`, CRF 20).
- With NVENC, a clip of 20 s or more and at least 600 frames, `build_proxy_halves` encodes two halves in two concurrent FFmpeg processes. It splits on the clean cut point nearest the middle (30% to 70% of the clip), writes MPEG-TS parts and joins them by stream copy. The joined frame count must equal the source frame count, or the single-process build runs.

### Editor state

`editor_state` returns:

| Key | Content |
| --- | --- |
| `clip` | The history entry (`path`, `name`, `app`, ...). |
| `animations` | `animations_enabled` from the controller, refreshed every 3 s. |
| `quality` | Saved export quality, 1..10, default 10. |
| `source_missing` | The clip file no longer exists. |
| `ffmpeg` | `status`, `path`, `version`, `message`, `encoder`. |
| `probe` | `status` (`pending`, `ready`, `failed`, `unavailable`), `duration`, `fps`, `width`, `height`, `codec`, `pix_fmt`, `bitrate`, `kbps`, `audio_tracks`, `audio_names`. |
| `waveform` | `status` (`pending`, `ready`, `failed`, `none`, `unavailable`), `rate` (100). |
| `preview` | `status` (`pending`, `building`, `original`, `ready`, `failed`, `unavailable`), `level`, `stage`, `progress`, `engine`. |
| `export` | `status` (`idle`, `running`, `done`, `failed`, `canceled`) plus progress or result fields. While running it carries `size_limit` and `attempt`; when done, `size_limit`, `attempts` and `audio_streams`. |
| `prefs` | Editor preferences without `ffmpeg_path`. |
| `quality_plans` | Ten entries: `video_kbps`, `audio_kbps`, `codec`, `height`, `original`. The page uses them for the size estimate. |
| `revision` | Change counter. |

`editor.js` polls `editor_state` every 250 ms during an export, every 500 ms while analysis or a preview build runs, and every 1500 ms otherwise.

### Editor preferences

`save_pref` accepts these keys and stores them in `editor.json`:

| Key | Values |
| --- | --- |
| `quality` | Integer 1..10. |
| `timecode` | `duration`, `clock`, `smpte`. |
| `volume_master` | Number 0..1. |
| `track_volumes` | Object of track name to number 0..1, at most 32 entries. |
| `scale` | `0.75`, `1.0`, `1.25`, `1.5`. Applied as the WebView2 `ZoomFactor`. |
| `preview_cuts` | Boolean. Preview playback skips removed sections. |
| `preview_quality` | `full`, `half`, `quarter`. Also switches the preview. |
| `size_limit` | `0` or `10`, `25`, `50`, `100`, `500`. The chosen **Max size**. |

### Export

`start_export` validates the request:

- `quality` is 1..10.
- Segments are `[start, end]` pairs in seconds, ordered, non-overlapping, at most 200. The code clamps ends to the probed duration and drops pieces shorter than 0.01 s.
- FFmpeg is `ready`, the probe is `ready`, no export is running, and the source file exists.

`run_export` then runs on a thread:

1. Pick the output path with `output_for`.
2. Create a work folder under `%TEMP%\OBSReplay-preview`.
3. At quality 10 when `smart_cut_possible`: run `smart_export`. If it fails, record a note and continue with step 4.
4. Otherwise run `encode_export`.
5. Rename `<name>.mp4.part` to `<name>.mp4` without overwriting.
6. Add the file to history with `Controller::add_clip`. Explorer is not opened. The editor offers **Show in folder**, **Play video** and **Copy**.

Cancel or window close terminates FFmpeg and deletes the partial file. FFmpeg children run hidden inside a kill-on-close job object.

### Smart cut (quality 10)

Conditions: source codec H.264 or HEVC, pixel format `yuv420p`, at least one clean cut point, and a matching encoder (`h264_nvenc` or `libx264`; `hevc_nvenc` or `libx265`).

- For each kept section, frames before the first clean keyframe and after the last one are re-encoded. Frames between them are stream-copied with `-frames:v`.
- Re-encode settings: NVENC `p4`, tune `hq`, CQ 18, no B-frames; or x264 CRF 15 / x265 CRF 16. A `setts` bitstream filter gives the re-encoded frames the source decode delay.
- Encoder attempt order per section: CUDA decode with NVENC, CPU decode with NVENC, CPU encoder.
- Each part is an MPEG-TS file. An ffconcat list with exact per-part durations joins them by stream copy.
- Audio is cut with `atrim` per track, joined with `concat` and encoded to AAC at 192 kbps.
- The joined video must report the exact kept frame count. Any other count triggers the full re-encode.

### Re-encode (quality 1 to 9)

| Quality | Share of source video bitrate | Audio kbps | Height cap |
| --- | --- | --- | --- |
| 1 | 10% | 96 | 720 |
| 2 | 14% | 96 | 720 |
| 3 | 19% | 128 | none |
| 4 | 25% | 128 | none |
| 5 | 32% | 160 | none |
| 6 | 40% | 160 | none |
| 7 | 50% | 192 | none |
| 8 | 62% | 192 | none |
| 9 | 78% | 192 | none |
| 10 (fallback) | 100% | 192 | none |

- Output codec is HEVC (`hevc_nvenc`, tag `hvc1`) when the source is HEVC and NVENC HEVC works. Otherwise it is H.264.
- An H.264 output from a source that is not H.264 gets 1.35 times the share. The minimum video bitrate is 300 kbps.
- Up to 4 segments on GPU decode, or 16 on CPU decode, each get their own `-ss`/`-t` input. Above that, one input feeds `trim` filters.
- A `concat` filter joins video and every audio track. The code writes the filter graph to a file and passes it with `-/filter_complex` (FFmpeg 7 or newer) or `-filter_complex_script`.
- Attempt order: CUDA decode with `scale_cuda` and NVENC, CPU decode with NVENC, `libx264` preset `faster`.
- Audio is AAC. Track titles from the source are written to the new tracks.

### Preview of cuts

- With **Preview cuts** on, `skipRemoved` in `editor.js` moves the playhead out of a removed section while the video plays, and when Play is pressed inside one.
- The video element rounds a time it is given to microseconds, so a jump to a cut can read back just before the cut. The jump therefore lands 1 ms inside the kept section, a position within 1 ms of a section's end counts as past it, and a jump already under way is not asked for again. Without this a cut placed by dragging kept the video seeking on every frame and it never started.
- A `play()` that a pause interrupts (`AbortError`) is not shown as an error.

### Max size

A size limit replaces the quality choice until the user moves the slider again. `fit_export` in `editor.cpp` runs it.

- Limits are 10, 25, 50, 100 and 500 MB, with 1 MB as 1,000,000 bytes.
- Budget: 97% of the limit, less an estimate of what MP4 adds (`container_bytes`), less the audio of every exported stream (64, 96 or 128 kbps by budget, counted at 105%). The rest is the video bitrate (`limit_plan`).
- Under 500 kbps of video the limit counts as impossible for that length. The page shows the size as unavailable and `start_export` refuses it.
- `limit_height` lowers the height through 1440, 1080, 720, 540, 480 and 360 until the bitrate gives at least 0.03 bits per pixel for HEVC or 0.045 for H.264.
- The video bitrate is never more than the source needs as H.264: its own bitrate, or 1.35 times it for an HEVC or AV1 source. A large limit on a small clip therefore does not grow the file to the limit.
- Rate control caps the video: `maxrate` and `bufsize` equal the video bitrate scaled by length ÷ (length + 1 s), for NVENC and x264.
- When the source is H.264 and its original video is estimated at 90% of the limit or less, the smart cut runs first.
- Every result is measured. A file over the limit is encoded again with the bitrate cut by the overshoot plus 15%, at most three more times. After that the export fails and the file is deleted. No file over the limit is ever reported as done.
- The output is always H.264 (`h264_nvenc`, or `libx264` without NVENC), whatever the source codec. A size limit is for sharing, and H.264 plays everywhere. An HEVC or AV1 source is therefore always re-encoded.

### Audio tracks in an export

- The **Audio tracks** panel lists each recorded track with a check box and a volume slider. A clip with one track is listed too.
- Every listed track ticked at full volume: the export keeps its tracks as recorded, OBS's Mix included.
- A volume set on a track is the volume it is exported at. The export applies it as a linear gain (`volume=0.1` for 10%, which is 20 dB down), the same gain the preview uses. A track turned down counts as a changed choice: the export leads with a mix of the tracks at their set volumes, followed by each track at its volume. A track at 0, or muted, is left out like an unticked one.
- The main volume slider is the listening volume. It does not change the export.
- Volumes are remembered between clips (`track_volumes`), so the export dialog states the sound when it differs from the recording: "Sound: Desktop 100%, Microphone 10%".
- Two or more ticked, but not all: the first track is a mix of the ticked tracks (`amix`, `normalize=0`, titled "Mix"), followed by each ticked track with its name. Most players play only the first track, so it carries exactly the choice.
- One ticked: that track alone. None ticked: no audio, and the result line says "No sound".
- The choice applies to the smart cut, the re-encode and the size limit, and the size budget counts only exported streams.
- An unticked track is silent in preview. A track choice, or a volume moved in this window, counts as an unexported edit.

### Export naming

- The folder is `Edited Exports` beside the source. A source already inside `Edited Exports` exports to the same folder.
- Without a typed name: `<source stem> (edited).mp4`, then `(edited 2)`, `(edited 3)`, up to 999.
- With a typed name: `<name>.mp4`, then `<name> (2).mp4`, and so on.
- `file_name_from` strips characters Windows forbids, a trailing `.mp4`, and reserved device names, and limits the name to 120 characters.

### Volume mixer label

- WebView2 plays audio from child `msedgewebview2.exe` processes.
- `AudioSessionLabel` runs a thread that checks every active render endpoint once per second.
- For sessions owned by descendants of this process, it sets the display name to `OBSReplay` and the icon to `<exe>,-1`.

## Themes

| Part | Role |
| --- | --- |
| `app.css` `:root` block | Tokens for the Original theme: surfaces (`--page`, `--well`, `--card`, `--canvas`, `--surface`, `--raise`), lines (`--line`, `--line-strong`, `--line-hover`), text (`--text`, `--muted`, `--dim`), accent (`--ui-accent`, `--accent-hover`, `--on-accent`, `--ui-accent-text`, `--accent-bright`), status (`--green`, `--red`), motion (`--fast`, `--ui`, `--ease`), shape (`--r-control`, `--r-card`). |
| `app.css` `--ov-*` tokens | Fixed dark colors for controls drawn over video. Identical in every theme. |
| `themes.css` | One `:root[data-theme="<id>"]` block per theme (`mono`, `light`, `rose`, `amethyst`, `ember`) that overrides the color tokens, plus rules for `:root[data-scheme="light"]`. |
| `theme.js` | Theme list (`id`, `name`, `scheme`, `swatch`) and `window.Theme`: `list`, `get`, `set`, `save`, `preview`, `known`, `token`, `timeline`, `onChange`. |
| `theme_ground` in `platform.cpp` | Native window background color and light flag per theme id. Unknown ids give Original. |

Flow:

- `theme.js` reads `localStorage["obsreplay.theme"]` before first paint and sets `data-theme` (absent for Original) and `data-scheme` on `<html>`.
- The saved theme is the `theme` setting. On each poll, `app.js` calls `Theme.save` with it, and `Theme.set` when the form has no unapplied edits.
- The Settings picker calls `Theme.preview` for this window and `set_theme` to save the choice at once. No Apply. If an action is running the save is refused and the choice waits for Apply.
- Other windows follow through the `storage` event.
- Every paint calls the bridge function `theme_changed`, which updates the native window and view background and `set_force_dark`.
- At window creation, native code reads `saved_theme()` so the window ground matches before the page loads.
- A theme change cross-fades with `document.startViewTransition` (240 ms) unless `data-motion` is `off`.
- `Theme.timeline()` resolves tokens to concrete colors for the canvas timelines in the viewer and editor.

A new theme needs three edits: the list in `theme.js`, a block in `themes.css`, and an entry in `theme_ground`.

An editor window gets the saved theme from the native side twice. A script injected before the page loads writes it to `localStorage`, so the first paint is right. `editor_state` carries `theme`, so an open editor follows a change within 3 seconds.

## Session log and Debug tab

### Version and commit

- `project(OBSReplay VERSION x.y.z)` in `CMakeLists.txt` is the one place the version is set. The version resource and the notices file read it.
- `cmake/build_info.cmake` runs on every build. It writes `build/generated/build_info.hpp` with the version, the full Git commit and a flag for uncommitted changes to tracked files. It rewrites the file only when the content changes.
- `app_version()` returns `1.0.2+abc1234`. A build from changed files ends in `.dirty`. Release builds come from a clean commit.

### Log file

- `log_start` opens `<data>\logs\session-<date>-<time>-<pid>.log` after the single-instance check. The 3 newest files stay, this run included. **Export log** saves this run's log.
- Each line: local time, level (`DEBUG`, `INFO`, `WARN`, `ERROR`), message, then `key=value` pairs. Text values are quoted.
- The app writes each line to disk at once. A crash leaves its exception code as the last line.
- Header: version, commit, build time, executable, data folder, Windows build, CPU, memory, graphics adapters, displays, WebView2 runtime.
- `Controller::Impl::update` is the one place state changes. `log_changes` compares before and after and logs changed flags, each changed setting (old and new value), messages, errors and the clip count.
- Each action logs its request, start, end, duration and error. Also logged: OBS WebSocket events and connection attempts, OBS launch and close, shortcuts, tray menu choices, window show and hide, FFmpeg checks and installs, editor opens, exports.
- The log holds file paths and device names. It holds no passwords. The WebSocket password is never logged.

### Resource use

`resource_usage` (`src/resources.cpp`) reads two process groups: OBSReplay with every process it started (WebView2, FFmpeg), and each `obs64.exe` with its children. OBS started by OBSReplay counts as OBS.

| Figure | Source |
| --- | --- |
| CPU | Process times between two calls, as a share of all cores. The first call reports `sampled: false`. |
| Memory | Private working set, summed. Same figure as Task Manager. The plain working set counts shared browser code once per process and reads more than twice as high. |
| GPU, GPU video encoder | Performance counter `GPU Engine`. Per group: the busiest engine type, and `VideoEncode`. |
| GPU memory | Performance counter `GPU Process Memory`, dedicated. |
| OBS frame rate, render time, missed and skipped frames | `GetStats` from OBS. The controller asks only while a page polled `resources` in the last 5 seconds. |
| Clips drive | Free space on the drive of the clips folder. |
| While in tray | Memory and process count of OBSReplay, read 90 seconds after the window hides and kept in `tray-usage.json`. A suspended page releases its memory over 30 to 45 seconds, so an earlier reading is several times too high. Before the first reading: 20 MB and the current process count. |

The controller state also carries `disk`: `{drive, free, total, percent}` for the clips drive, refreshed every 30 seconds and after each save. Free bytes are rounded down to 100 MB so normal disk activity does not bump the state revision. The Capture page warns at 5% or less.

The **Debug** tab in Settings polls `resources` every second and `log_tail` every 2 seconds, only while the tab is open and the window is visible.

## Build and embedding

### Requirements

- Windows x64, Visual Studio C++ desktop tools (MSVC 19.44 or newer), CMake 3.25 or newer, Git.
- The first configure downloads the dependencies.

### Commands

Run `./build.ps1` (default `Release`) or `./build.ps1 -Configuration Debug`. The script:

1. Finds `cmake` on `PATH` or in Visual Studio and CMake install folders.
2. Configures `build/` with `-A x64`.
3. Builds with `--parallel`.
4. Copies `build/<Configuration>/OBSReplay.exe` to `dist/OBSReplay.exe`.

### CMake

| Item | Value |
| --- | --- |
| Language | C++23, extensions off. |
| Runtime | Static MSVC runtime (`MultiThreaded`, `MultiThreadedDebug`). |
| Dependencies | saucer `v8.0.5` (static, Git tag), nlohmann_json `v3.12.0` (tarball with SHA-256). `FETCHCONTENT_UPDATES_DISCONNECTED` is on. |
| Target | `OBSReplay`, a `WIN32` executable from the thirteen `src/*.cpp` files and the generated `app.rc`. |
| Compile options | `/W4 /utf-8 /permissive- /EHsc`; defines `UNICODE`, `_UNICODE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`. |
| Link | `saucer::saucer`, `saucer::private`, `saucer::embedded`, `nlohmann_json::nlohmann_json`, and the Windows libraries `comctl32 comdlg32 shell32 ole32 uuid propsys advapi32 bcrypt crypt32 winmm winhttp version dxgi`. |
| Link options | `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MANIFEST:NO /OPT:REF /OPT:ICF`; interprocedural optimization in Release. |

### Embedding

- `saucer_embed("frontend" TARGET OBSReplay DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/embedded")` compiles every file under `frontend/` into the exe.
- `view.embed(saucer::embedded::all())` registers the files. `view.serve("/index.html")` and `web.serve("/editor.html")` load them from `saucer://embedded/`.
- A frontend change needs a rebuild. No npm step and no bundler exist.
- CMake collects `LICENSE*` and `COPYING*` files from fetched dependencies, the WebView2 SDK license and `frontend/fonts/OFL.txt` into `THIRD-PARTY-NOTICES.txt`. `app.rc` embeds it as `RCDATA` 101.
- `resources/app.manifest` is embedded through the resource script, so the linker manifest is off. CMake fills in its version from `project()`.
