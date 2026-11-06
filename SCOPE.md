# Reviewed implementation scope

## Deliverable

One release OBSReplay.exe with compiled-in HTML/CSS/JavaScript and statically linked C++ dependencies. No Node, frontend runtime, sidecar UI, Lua command bridge, admin task, or installer is required to run the controller. Installed OBS and WebView2 perform capture and rendering respectively.

## Core workflows

- First run: detect or browse for OBS; choose capture mode, monitor, clip duration and destination; create a separate OBS profile/collection and configure authenticated local WebSocket control while OBS is closed. Back up the exact shared config changed. Explain any missing prerequisite with an actionable link.
- Capture: start/stop replay buffer, save via button or configurable global hotkey; show actual connection and replay state, pending work, timeout/failure and confirmed saved file. Stopping discards buffered history. Reconnect without blindly retrying a save.
- Settings: duration, 30/60/120/144 FPS, original/1440p/1080p/720p resolution, probed H.264/HEVC/AV1 hardware encoder choices with explicit CPU fallback, quality, display/fullscreen-game capture, desktop and microphone audio devices, RNNoise/Speex noise suppression and separate tracks, save folder, sound, best-effort app folders, login startup and tray behavior. One explicit Apply model with pending state; settings that require OBS restart are applied only when the dedicated session is closed and clearly explained.
- Recent clips: bounded local list from confirmed saves, open/reveal file and open destination folder. Never invent media, counters or successful operations.
- Desktop integration: single instance, system tray, show/hide shortcut, conflict-aware global hotkeys, optional current-user login entry. No forced OBS termination, no blanket process popup dismissal, no Windows Game Bar changes, no always-admin requirement.

## Implementation constraints

Use CMake, C++23, saucer v8.0.5, plain embedded web UI and Windows APIs. Keep modules limited to platform/settings, OBS profile/setup, WebSocket/session and application/UI. Use native WinHTTP WebSocket transport, correlated request IDs, protocol authentication, bounded waits, serialized actions and live event reconciliation. Store app settings atomically, validate inputs in C++, keep secrets out of logs and frontend responses.

OBS's actual game hook is not exposed by standard WebSocket events. Fullscreen-game capture uses OBS's native game source; best-effort app labels are kept distinct from capture readiness. Desktop mode excludes game-hook sources entirely.

Do not edit an active personal OBS profile/session. Check profile and scene ownership before recording actions. Setup must not overwrite unrelated profile/collection paths or rewrite global config while OBS is running. Existing active recording/streaming always prevents disruptive work.

## Acceptance evidence

- Compile Release locally and inspect dependencies/artifact size.
- Run original native EXE, inspect rendered UI at normal/minimum sizes and high DPI; keyboard focus, reduced motion, setup/error/recording/saving/settings states.
- Exercise real installed OBS: setup, authenticated connection, buffer start, save a playable clip, stop, restart/reconnect, settings persistence and tray/hotkey controls.
- Review implementation independently; fix material findings and rerun affected checks.
- Document build/run/prerequisite details and material hardware-specific limits. Do not claim untested AMD/Intel/anti-cheat compatibility.

## Review resolutions

- Quit closes the controller; OBS and any active capture continue. Window close hides to tray. Both behaviors are stated in the UI. Replay enabled/disabled intent persists; optional login startup starts capture only if that intent is enabled.
- Apply saves a healthy active replay buffer, closes the owned OBSReplay session gracefully, applies capture/audio changes and reopens OBS with replay capture resumed when it was active. App-only settings do not restart OBS. Streams, full recordings and other profiles block disruptive work. First setup requires OBS closed once; the controller never forces a process to stop.
- Setup uses owned-path markers, validates collisions, stages and atomically replaces generated files, and rolls back failures. Shared WebSocket configuration is backed up. Reset integration, while OBS is closed, restores only shared values that still match this app's applied values and removes only owned files. Clips remain intact.
- Missing/disconnected devices and monitors are surfaced; selected devices are validated at apply time. No silent switch from game capture to desktop capture.
- Replay coverage says “up to” the configured duration. No promise that a running buffer contains a full-duration clip, and no fake fill meter.
- Setup, probing, file work and network requests run off the UI thread. Native bridge access stays restricted to the embedded UI; external help opens separately through a fixed allowlist.
- Sorting uses the request-time best-effort app label, collision-safe destinations and no overwrite. Sorting failure retains and displays the confirmed original clip.
- Embedded notices cover linked dependencies. Inspect the finished EXE's imports to verify the single-file packaging claim.

Preimplementation gate passed after the original-code audit, engine and saucer research, design planning, and an independent review with documented revisions. Application implementation follows this scope.
