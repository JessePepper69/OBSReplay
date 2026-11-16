# Standalone OBS research

This file records the research and scope for shipping OBSReplay without a separately installed OBS Studio.

## Contents

- [Status](#status)
- [Recommendation](#recommendation)
- [Current OBS dependency](#current-obs-dependency)
- [Options compared](#options-compared)
- [Option A: bundled portable OBS](#option-a-bundled-portable-obs)
- [Requirements for every option](#requirements-for-every-option)
- [Licensing](#licensing)
- [Installer and distribution](#installer-and-distribution)
- [Docs to update](#docs-to-update)
- [Tests to run on a clean Windows VM](#tests-to-run-on-a-clean-windows-vm)
- [Open decision](#open-decision)

## Status

- Researched 2026-11-15.
- Five research passes: codebase audit, licensing and trademark, bundled against embedded OBS, non-OBS capture, installer and distribution.
- Findings were checked against the OBS 33.0.1 source.
- Nothing has been tested on a clean VM.
- Items marked *[untested]* come from reading the source only.

## Recommendation

Shipping without an installed OBS is feasible and allowed, under three conditions:

1. Rename the product. OBS's IP policy forbids "OBS" in an app's name or branding.
2. Ship OBS's own signed capture-hook files unmodified.
3. Meet the GPL source obligations for every distributed OBS binary.

| Phase | Option | Work | Effort |
|---|---|---|---|
| 1 | A | Bundle a private, unmodified, portable OBS 33 as a hidden child process. Keep the WebSocket control | 4 to 6 dev-weeks, including installer, signing and rename |
| 2 | B2 | Replace the bundled OBS with a small capture host built on libobs. Removes the OBS tray icon, OBS dialogs, Qt and the firewall prompt | 4 to 6 more weeks |

Option C (own capture stack without OBS) takes 4 to 7 months to reach about 85 to 90% parity. It cannot capture true exclusive-fullscreen games under anti-cheat. It is the worse route for a solo developer.

## Current OBS dependency

About 1,200 of the 3,967 C++ lines are OBS-specific.

| File | OBS-specific part |
|---|---|
| `src/obs_socket.cpp` | WebSocket client (whole file) |
| `src/integration.cpp` | Most of the file: writes `basic.ini`, the encoder JSON, the scene collection, the shared obs-websocket config, owner markers and rollback |
| `src/controller.cpp` | About 400 lines: launch, connect, ownership, save, restart on Apply |
| `src/platform.cpp` | About 120 lines: `find_obs`, launch and close, `obs_running`, probes |

WebSocket surface: 11 requests and 7 events.

OBS features in use:

- `game_capture` (`any_fullscreen`, anti-cheat hook) and `monitor_capture`.
- `wasapi_output_capture` and `wasapi_input_capture`, plus `noise_suppress_filter_v2`.
- 3-track AAC.
- Bicubic NV12/709 rescale.
- The x264, NVENC, AMF and QSV encoders.
- `replay_buffer` with faststart through `RecMuxerCustom`.
- The `obs-*-test.exe` probes.

OBS features not in use: streaming, recording, transitions.

The clip editor needs `ffmpeg.exe`. OBS ships FFmpeg DLLs only, so OBS does not supply it. This dependency is separate from OBS and also needs bundling for an out-of-box install.

## Options compared

| | A: bundled portable OBS | B1: libobs inside the EXE | B2: own GPL capture host plus closed UI | C: own stack, no OBS |
|---|---|---|---|---|
| How | Unmodified OBS 33 in the install folder, portable mode, hidden, WebSocket control | Link `obs.dll` into `OBSReplay.exe` | `capture-host.exe` links libobs. The UI talks to it over a named pipe | WGC/DXGI capture, FFmpeg libs (NVENC, AMF, QSV), WASAPI, RNNoise, own ring buffer and muxer |
| App must be open source | No (mainstream "aggregation" reading) | Yes (GPL) | Host yes. UI arguably no, the weakest legal position (see [Licensing](#licensing)) | No |
| Exclusive-fullscreen and anti-cheat games | Same as OBS (signed hook) | Same, if the official signed hook files are used | Same | Gap: DD/WGC freeze on true exclusive fullscreen. An own hook is unsigned and risky under anti-cheat |
| Visible OBS UI | OBS tray icon, possible OBS dialogs, firewall prompt naming OBS | None | None | None |
| Payload | About 104 to 120 MB unpacked, about 40 to 46 MB compressed | About 56 MB | About 56 MB | Small |
| Effort | 2.5 to 3.5 weeks engine work (4 to 6 weeks with installer and the rest) | 5 to 8 weeks | 4 to 6 weeks after A | 4 to 7 months |
| Upkeep | 1 to 2 days per OBS upgrade | 1 to 2 days | 1 to 2 days | All in-house. Three GPU vendors to test |

## Option A: bundled portable OBS

### Runtime layout

- OBS 33.0.1 portable (`portable_mode.txt`) in a per-user install, for example `%LOCALAPPDATA%\Programs\<App>\obs\`.
- Config is fixed at `<root>/config/obs-studio`, so the folder must be writable. This rules out Program Files.
- Plugins live in `core/<name>/`.
- Run OBS with the working directory set to `bin/64bit`.
- The portable mutex is `OBSStudioPortable<config path>`. It never conflicts with an OBS the user installed.

### Launch flags

- `--portable --profile <P> --collection <C> --minimize-to-tray --disable-updater --disable-missing-files-check --only-bundled-plugins`
- Add `--websocket_port <free port> --websocket_password <random>` on each launch.
- `--disable-shutdown-check` does not exist in OBS 33.

### Config to seed before first launch

| File | Key | Reason |
|---|---|---|
| `user.ini` | `FirstRun=true` | Seed value, no reason recorded |
| `user.ini` | `SysTrayEnabled=true` | Without it the OBS window shows |
| `user.ini` | `ConfirmOnExit=false` | Without it OBS cancels Windows shutdown while the buffer runs |
| `global.ini` | `LastVersion` | Stops the auto-config wizard |
| `plugin_config/obs-websocket/config.json` | `server_enabled` | No command-line equivalent exists |

### Before each launch

- Delete `config/obs-studio/.sentinel/run_*`. A leftover file makes OBS show a safe-mode prompt, and safe mode disables the WebSocket server.
- Re-probe encoders. Rewrite the profile to x264 if the configured encoder has disappeared. OBS 33 otherwise shows a modal "missing encoder" dialog.
- Check that the clip folder exists and has free space. Both conditions otherwise cause modal dialogs.

### Trimming the bundle *[untested]*

- OBS 33 loads a compiled-in list of core modules in alphabetical order.
- A missing module file stops the loader, which then skips every later module without an error. `win-capture` and `win-wasapi` come last, so capture is lost.
- A module file that is present but fails to load makes OBS show a fatal error.
- Never delete a module. Replace each unwanted module with a stub file that has no `obs_module_load` export.
- The largest unwanted module is obs-browser/CEF at 377 MB.
- Keep `libcurl.dll`, `srt.dll`, `librist.dll` and `obs-transitions`.

### Game-capture hook

- `win-capture` copies its hook DLLs to `%ProgramData%\obs-studio-hook`.
- That folder is shared with any OBS the user has installed. The newest version wins.
- `win-capture` registers the Vulkan layer in HKCU, or in HKLM when elevated.
- The `ALL_APP_PACKAGES` ACL needed for UWP and Store games is applied only when OBS runs elevated.

### UX limits of unmodified OBS

- A second tray icon with Show and Exit for the full OBS window.
- Errors titled "OBS Studio".
- A Windows Firewall prompt naming OBS on first launch. Cause: obs-websocket binds all interfaces and has no localhost-only option.
- Whether loopback works after the user dismisses the firewall prompt is *[untested]*.
- An admin-installed block rule suppresses the firewall prompt.
- Sleep inhibition: an active output prevents sleep and keeps the display on. OBS has no setting for this.
- Sleep mitigation: run the buffer only while a game is detected or the user is active.

### App code changes

- Use a fixed bundled path and a private config root in place of `%APPDATA%\obs-studio` (`platform.cpp:102`).
- Match processes by path. `obs_running` and `launch_obs` match any `obs64.exe` (`platform.cpp:248-258`).
- Close OBS by PID. The current code closes by a window title starting "OBS " (`platform.cpp:290`).
- Add a supervisor that relaunches with backoff, clears the sentinel and resumes the buffer. `background_connection` only marks the session disconnected.
- Probe the adapter OBS uses, in place of adapter 0, so hybrid-GPU laptops work.
- Delete the shared-config backup, marker and reset logic.
- Delete the OBS path picker.
- Remove the "31 or newer" version check.
- Reword about 30 UI strings.

Out-of-box default: the current default (game capture `any_fullscreen`, `replay_enabled=false`) records nothing until a game is hooked. Change it as follows:

- Auto-run setup and auto-start the buffer on first launch.
- Use a scene with display capture beneath game capture.
- Detect a successful hook (source width above 0).

## Requirements for every option

### Rename

- OBS IP policy: "You are not permitted to use the OBS name or logo in your application or plugins branding."
- OBS enforces the policy. Streamlabs dropped "OBS" from its name in 2021.
- Factual attribution is allowed, for example "Capture engine: OBS Studio (GPLv2). Not affiliated with the OBS Project."
- Rename the profile, the collection and the Videos folder too.

### Anti-cheat

- Anti-cheats exempt only OBS's official signed `graphics-hook*`, `inject-helper*` and `get-graphics-offsets*` files.
- These files are dual-signed since OBS 31.1.
- They must come from the same OBS tag as `win-capture`.
- OBS will not sign anyone else's build.

### VC++ runtime

- OBS 33 is built with VS 2026.
- `obs64.exe` aborts if `msvcp140.dll` is older than 14.40.
- Ship the current v14 x64 CRT DLLs app-local in `bin/64bit`. This needs no admin rights.
- x86 CRT DLLs are not needed. The 32-bit helpers are statically linked.

### Fresh-reset GPU state

- On the Microsoft Basic Display Adapter, OBS fails with "Failed to initialize video".
- Detect this before launch and tell the user to install the graphics driver.
- If hardware probes fail, fall back to x264.

### Signing

- Sign the installer, the app and any bundled `ffmpeg.exe`.
- Smart App Control on fresh Windows 11 can block unsigned binaries with no override.
- EV certificates do not give instant SmartScreen reputation.
- Certificate options: Azure Artifact Signing (individuals in the US or Canada only) or an OV certificate.
- Never re-sign OBS files.
- Expect antivirus false positives on `inject-helper`. Submit releases to Microsoft.

### Editor FFmpeg

- Bundle a GPL `ffmpeg.exe` as a separate executable. This counts as aggregation.
- Mirror its source and sign the binary.
- Alternative 1: an LGPL build (no x264 or x265; use NVENC or OpenH264).
- Alternative 2: a download on first use.
- Size: about 25 MB.

### Elevated games

- Hooking a game that runs as administrator needs an elevated OBS.
- Elevated binaries in a user-writable folder open a privilege-escalation hole.
- Offer an optional per-machine "admin games" install mode: Program Files, HKLM Vulkan layer, ProgramData ACL, firewall rule.

### Privacy toggles

- Capture fails if "Let desktop apps access your microphone" is off.
- Capture fails if the Windows 11 screen-capture privacy setting is off.
- Detect both and deep-link to the setting.

### Coexisting with the user's own OBS

- Use a different WebSocket port.
- The ProgramData hook folder is shared.
- Two OBS instances cannot hook the same game. Warn the user, or fall back to display capture.
- On uninstall, remove the hook folder and Vulkan keys only if no other OBS is installed.

## Licensing

This section is not legal advice.

- OBS is GPL-2.0-or-later.
- libobs has no linking exception.
- OBS's bundled FFmpeg is GPLv3 and includes x264.

| Option | Position |
|---|---|
| A | Shipping unmodified OBS as a separate program over a public protocol is "mere aggregation". Strongest position for keeping the app closed |
| B1 | The whole app becomes GPL |
| B2 | Weaker than A. The GPL FAQ requires "arms length" separation and calls wrapper modules "legally irrelevant" |

Obligations under option A:

- Ship the GPLv2, GPLv3 and LGPL (Qt) texts plus component notices.
- Host the exact OBS, obs-deps, FFmpeg and x264 source, or include a 3-year written offer.
- Add no restrictions on the OBS parts.

Notes on option B2:

- A host written only for a closed UI is a weaker case than A.
- No verified closed-UI precedent was found. Segra and Streamlabs are fully open source.
- For B2 with a closed UI: make the host a standalone, documented, open-source tool with a coarse protocol, and get a lawyer's opinion.
- The other route is to GPL the whole app.

Patents:

- Shipping the x264 or x265 software encoders in a commercial product carries H.264/HEVC patent-pool exposure.
- Hardware encoders are largely covered by the vendors.

## Installer and distribution

| Topic | Finding |
|---|---|
| Installer | Inno Setup 6. Per-user by default (no UAC), with an optional per-machine mode |
| Mutex | `AppMutex=Local\OBSReplay.Controller.0.1` |
| WebView2 | Bundle the bootstrapper (about 2 MB). Run it only if the runtime is missing |
| MSIX | Avoid. Registry and AppData virtualization hides the HKCU Vulkan layer from games |
| Runtime layout | A versioned side-by-side runtime folder (`runtime\obs-33.0.1\`) with a stable config folder |
| Version switch | Switch OBS versions only at a safe OBS restart. Keep the previous version for rollback |
| App updates | WinSparkle or a custom updater |
| Size | About 40 to 46 MB `setup.exe`, or about 65 to 70 MB with FFmpeg |
| Uninstall | Remove the app, runtime, config and Run key. Keep clips and ask before deleting them |

## Docs to update

Phase 1 makes these statements false. Update them when Phase 1 lands.

| File | Section | Statement |
|---|---|---|
| `README.md` | **Install**, **Build from source** | A single EXE with no installer, and installed OBS as a prerequisite |
| `docs/scope.md` | Distribution and prerequisites | Same |

## Tests to run on a clean Windows VM

Run these before committing to Phase 1:

1. The stub-module approach loads the needed modules.
2. A fresh config plus `--profile` shows no dialogs.
3. The firewall prompt appears, and loopback works after the user dismisses it.
4. The hook works in an elevated game and in a non-elevated game.
5. The bundled OBS coexists with an installed OBS.
6. Shutdown and sleep behave with the buffer active.

## Open decision

The owner must decide whether the app stays closed-source.

| Answer | Path |
|---|---|
| Closed-source | A, then B2 with a lawyer's review |
| Open-source | B1 or B2 become simple. Go straight to an embedded engine |
