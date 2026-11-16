# Architecture

This file covers how the OBSReplay code is organized and how each part works: processes and windows, the native bridge, the controller, the OBS integration, settings, media, the editor, themes and the build.

## Contents

- [Overview](#overview)
- [Source map](#source-map)
- [Process and window model](#process-and-window-model)
- [Native bridge](#native-bridge)
- [Controller](#controller)
- [OBS integration](#obs-integration)
- [Settings and stored files](#settings-and-stored-files)
- [Media serving](#media-serving)
- [Replay viewer](#replay-viewer)
- [Clip editor](#clip-editor)
- [Themes](#themes)
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
| [`main.cpp`](../src/main.cpp) | Entry point, single instance, main window, tray icon and menu, global hotkeys, show/hide sequence, main-window bridge. |
| [`controller.cpp`](../src/controller.cpp), [`controller.hpp`](../src/controller.hpp) | `Controller`: state snapshot, action queue, worker thread, OBS session logic, clip history. |
| [`integration.cpp`](../src/integration.cpp), [`integration.hpp`](../src/integration.hpp) | `Integration`: writes and removes the OBS profile, scene collection and WebSocket settings; probes encoders; reads credentials. |
| [`obs_socket.cpp`](../src/obs_socket.cpp), [`obs_socket.hpp`](../src/obs_socket.hpp) | `ObsSocket`: obs-websocket v5 client on WinHTTP. |
| [`platform.cpp`](../src/platform.cpp), [`platform.hpp`](../src/platform.hpp) | Paths, settings load/validate/save, atomic file writes, OBS discovery and launch/close, monitors, audio devices, process runner, login startup, help links, theme grounds. |
| [`media.cpp`](../src/media.cpp), [`media.hpp`](../src/media.hpp) | `ClipMedia`: serves clip files and generated preview files to a WebView2 view with range support. |
| [`editor.cpp`](../src/editor.cpp), [`editor.hpp`](../src/editor.hpp) | `EditorHost`: editor windows, FFmpeg detection, probe, waveform, preview proxies, export, editor bridge. |
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
| `themes.css` | Token overrides for the six other themes. |
| `viewer.css`, `editor.css` | Viewer dialog and editor window styles. |
| `fonts/InterVariable.woff2`, `fonts/OFL.txt` | Bundled Inter font and its license. |

### Other

| Path | Responsibility |
| --- | --- |
| `CMakeLists.txt` | Build definition, dependency fetch, notices file, frontend embedding. |
| `build.ps1` | Configure, build, copy the exe to `dist/`. |
| `resources/app.rc.in` | Resource script: manifest, icon (ID 1), notices (`RCDATA` 101), save sound (`WAVE` 102), version info. |

## Process and window model

### Startup

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

### Tray

- `Desktop::add_tray` adds the icon. `TaskbarCreated` re-adds it after Explorer restarts.
- Left click, keyboard select or double click shows the window.
- Right click opens a menu: **Show OBSReplay**, **Save replay**, **Start capture** or **Stop capture (discards buffer)**, **Quit controller (OBS stays running)**.
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
| `start_export` | JSON string `{segments:[[start,end],...], quality, name?}` | `true` or error | Validate and start an export. |
| `cancel_export` | none | nothing | Cancel the running export. |
| `pick_ffmpeg` | none | `true`, `false` on cancel, or error | Native file dialog, then FFmpeg detection with that file. |
| `ffmpeg_help` | none | nothing | Open the FFmpeg download page. |
| `install_ffmpeg` | none | `true` or an error | Start `winget install --id Gyan.FFmpeg` on a worker thread. `editor_state` reports progress in `ffmpeg.install` (`stage`: `preparing`, `downloading`, `installing`, `verifying` or empty; `error`; `available`: winget found). On success the host adopts the new `ffmpeg.exe` and every open editor continues its analysis. |
| `reveal_output` | `reveal` bool | `true` or error | Show the last export in Explorer (`true`) or open it (`false`). |
| `close_editor` | none | nothing | Cancel any export and close the window. |
| `editor_dirty` | `value` bool | nothing | Tell native whether unexported cuts exist. |
| `save_pref` | `key` string, `value` JSON string | `true` or error | Validate and store an editor preference. See [editor preferences](#editor-preferences). |
| `retry_analysis` | none | nothing | Run clip analysis again. |
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
| `compression_update` | bool | The manifest's `compression_policy` is older than the code's. The next Apply rewrites the profile. |
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
| `start` | none | Launch OBS if closed, connect, verify ownership and readiness, send `StartReplayBuffer`, wait up to 5 s for confirmation, store `replay_enabled=true`. |
| `stop` | none | Send `StopReplayBuffer`, wait up to 5 s, store `replay_enabled=false`. |
| `save` | none | Save the replay buffer. See [replay save](#replay-save). |
| `reconnect` | none | Close the socket and connect again. Does not launch OBS. |
| `reset` | none | With OBS closed: remove the integration, delete the login entry, set `replay_enabled` and `start_at_login` to `false`. |
| `refresh_options` | none | Re-read monitors, audio devices and encoders. |
| `open_folder` | none | Create and open `clips_dir`. |
| `open_obs` | none | Launch the dedicated session, or restore the running OBS main window. |
| `open_clip` | `{path}` | Open a clip from history in the default app. |
| `reveal_clip` | `{path}` | Select a clip from history in Explorer. |
| `delete_clip` | `{path}` | Delete a clip file from history and drop it from the list. |
| `help` | `{topic}` | Open a fixed URL for `obs`, `webview2`, `ffmpeg` or `capture`, or write and open the notices file for `notices`. |

### Worker loop

- `initialize` loads settings, detects OBS, loads `clips.json`, reads options, then starts capture when `replay_enabled` is `true` and the integration exists.
- The loop waits up to 100 ms for an action, runs it, then drains WebSocket events.
- Once per second it runs `reconcile_clips` (drops history entries whose file is gone) and `background_connection`.
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

- `clips.json` holds up to 50 replays and 50 edited exports. `trim_history` counts the two kinds apart.
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
| `clip_seconds` | `RecRBTime`. `RecRBSize` is `min(seconds × 8, total RAM in MB ÷ 10)` clamped to 256..2048 MB. |
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

- NVIDIA bitrate ceilings are 24000, 16000, 10000, 7000, 4000 kbps at 1080p60. The code scales them by pixel count and frame rate, multiplies by 1.5 for H.264, and clamps to 1000..500000 kbps.
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
2. Decide `prepare`: `true` for setup, a missing integration, a pending `compression_update`, or a change in any capture key (`obs_path`, `clips_dir`, `clip_seconds`, `capture`, `monitor_id`, `output_height`, `fps`, `encoder`, `codec`, `quality`, `mic_enabled`, `mic_device`, `desktop_device`, `noise`, `separate_tracks`).
3. Without `prepare`: update the login startup entry if it changed, store the settings and stop here. OBS keeps running.
4. With `prepare` and an owned OBS session: refuse on an active stream or recording, save the current buffer (app label `Before settings change`), stop the buffer, close OBS with `close_obs`.
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
| `quality` | integer | `20` | `16`, `18`, `20`, `24`, `28`. |
| `mic_enabled` | bool | `false` | Boolean. |
| `mic_device` | string | `default` | `default` or a device id. |
| `desktop_device` | string | `default` | `default`, `off` or a device id. |
| `noise` | string | `off` | `off`, `rnnoise`, `speex`. |
| `separate_tracks` | bool | `true` | Boolean. |
| `sort_by_app` | bool | `true` | Boolean. |
| `save_sound` | bool | `true` | Boolean. |
| `start_at_login` | bool | `true` | Boolean. |
| `save_hotkey` | string | `Alt+Z` | `Ctrl`/`Alt`/`Shift` modifiers, then one of `A`-`Z`, `0`-`9`, `F1`-`F24`. Must differ from `show_hotkey`. |
| `show_hotkey` | string | `Alt+X` | Same shape as `save_hotkey`. |
| `replay_enabled` | bool | `false` | Boolean. The controller sets it on start and stop. |
| `animations_enabled` | bool | `true` | Optional. Boolean when present. |
| `theme` | string | `original` | Optional. 1..32 characters of `a`-`z`, `0`-`9`, `-`. |

- String values allow at most 32700 bytes, no control characters, valid UTF-8.
- `start_at_login` writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\OBSReplay` as `"<exe>" --tray`.

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
| `THIRD-PARTY-NOTICES.txt` | `open_help("notices")` | Notices extracted from the exe. |
| `WebView2\` | WebView2 | Browser profile, in isolated runs only. |

- `atomic_write` writes a temporary file, flushes it, and renames it over the target.
- Unreadable settings fall back to defaults with an error. The original file stays on disk.
- The page keeps three `localStorage` keys: `obsreplay.theme`, `obsreplay.clipTabs`, `obsreplay.viewerVolume`.

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
- Clip list thumbnails come from `takeThumbnail` in `app.js`: a muted `<video>` draws one frame to a 320 × 180 canvas.

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
| `export` | `status` (`idle`, `running`, `done`, `failed`, `canceled`) plus progress or result fields. |
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
6. Add the file to history with `Controller::add_clip` and select it in Explorer (not in headless mode).

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
| `themes.css` | One `:root[data-theme="<id>"]` block per theme (`mono`, `light`, `rose`, `forest`, `amethyst`, `ember`) that overrides the color tokens, plus rules for `:root[data-scheme="light"]`. |
| `theme.js` | Theme list (`id`, `name`, `scheme`, `swatch`) and `window.Theme`: `list`, `get`, `set`, `save`, `preview`, `known`, `token`, `timeline`, `onChange`. |
| `theme_ground` in `platform.cpp` | Native window background color and light flag per theme id. Unknown ids give Original. |

Flow:

- `theme.js` reads `localStorage["obsreplay.theme"]` before first paint and sets `data-theme` (absent for Original) and `data-scheme` on `<html>`.
- The saved theme is the `theme` setting. On each poll, `app.js` calls `Theme.save` with it, and `Theme.set` when the form has no unapplied edits.
- The Settings picker calls `Theme.preview`, which changes the current window until Apply.
- Other windows follow through the `storage` event.
- Every paint calls the bridge function `theme_changed`, which updates the native window and view background and `set_force_dark`.
- At window creation, native code reads `saved_theme()` so the window ground matches before the page loads.
- A theme change cross-fades with `document.startViewTransition` (240 ms) unless `data-motion` is `off`.
- `Theme.timeline()` resolves tokens to concrete colors for the canvas timelines in the viewer and editor.

A new theme needs three edits: the list in `theme.js`, a block in `themes.css`, and an entry in `theme_ground`.

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
| Target | `OBSReplay`, a `WIN32` executable from the eight `src/*.cpp` files and the generated `app.rc`. |
| Compile options | `/W4 /utf-8 /permissive- /EHsc`; defines `UNICODE`, `_UNICODE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`. |
| Link | `saucer::saucer`, `saucer::private`, `saucer::embedded`, `nlohmann_json::nlohmann_json`, and the Windows libraries `comctl32 comdlg32 shell32 ole32 uuid propsys advapi32 bcrypt crypt32 winmm winhttp version dxgi`. |
| Link options | `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MANIFEST:NO /OPT:REF /OPT:ICF`; interprocedural optimization in Release. |

### Embedding

- `saucer_embed("frontend" TARGET OBSReplay DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/embedded")` compiles every file under `frontend/` into the exe.
- `view.embed(saucer::embedded::all())` registers the files. `view.serve("/index.html")` and `web.serve("/editor.html")` load them from `saucer://embedded/`.
- A frontend change needs a rebuild. No npm step and no bundler exist.
- CMake collects `LICENSE*` and `COPYING*` files from fetched dependencies, the WebView2 SDK license and `frontend/fonts/OFL.txt` into `THIRD-PARTY-NOTICES.txt`. `app.rc` embeds it as `RCDATA` 101.
- `resources/app.manifest` is embedded through the resource script, so the linker manifest is off.
