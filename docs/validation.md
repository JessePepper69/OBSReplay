# Validation

This file records what has been tested in OBSReplay, how each check ran, the results, and what remains untested.

## Contents

- [Changes since the release verification](#changes-since-the-release-verification)
- [How to run checks](#how-to-run-checks)
- [Release artefact](#release-artefact)
- [Clip editor](#clip-editor)
- [MP4 output](#mp4-output)
- [Compression and automatic Apply restart](#compression-and-automatic-apply-restart)
- [Quality selector](#quality-selector)
- [Explorer reveal](#explorer-reveal)
- [Replay viewer and clips history](#replay-viewer-and-clips-history)
- [Save chime](#save-chime)
- [Capture engine and startup](#capture-engine-and-startup)
- [Main window UI](#main-window-ui)
- [Test hygiene](#test-hygiene)
- [Known limits and untested areas](#known-limits-and-untested-areas)

All results belong to the release verification dated 2026-11-06, except [Changes since the release verification](#changes-since-the-release-verification). The source records no per-check dates.

## Changes since the release verification

The app changed after the results below were recorded. Where a result below disagrees with this list, this list is the current behaviour.

| Recorded below | Current behaviour |
|---|---|
| Apply saves the current replay before it restarts OBS ("the buffer save", "pre-restart backups", "backs up any verified active scene") | Apply stops the buffer and restarts OBS without saving. The buffer starts again from empty, and the UI says so |
| Capture lists five recent replays | Capture lists the latest eight |
| Saved replays are MKV in the older capture checks | Profiles write MP4. Existing MKV clips still play |
| A profile update shows as **Apply improved compression** and waits for a click ("compression-upgrade action", "detect the old compression policy") | A profile update is applied at launch while OBS is closed. The page shows no button for it |
| Clip history holds 50 replays and 50 exports | The clip list has no limit |
| `replay_enabled` follows Start and Stop | `replay_enabled` is the **Start replay capture automatically** setting. Start and Stop leave it alone |
| Replay buffer memory is capped at 2048 MB | The memory ceiling scales with length, quality, resolution and frame rate, up to 75% of installed memory |

### Check of 2026-11-17

`build/fix-check/check.mjs` runs `build/Release/OBSReplay.exe` headless, isolated and muted (port 9461) against 130 hard-linked copies of a sample clip in two game folders.

| Check | Result |
|---|---|
| All 130 clips are listed from the clips folder, and `clips.json` keeps all 130 | Passed |
| **Replays** tab count and pager show 130 | Passed |
| Search by file name, by game, by year, by full month name, by `2026-11`, and by game with year | Passed |
| A search with no match reports none | Passed |
| A 29 MB clip still shows its size in MB | Passed |
| A clip deleted outside the app leaves the list | Passed |
| A settings file without `settings_version` gets `replay_enabled` turned on once, and the result is saved | Passed |
| Release compile and JavaScript syntax check | Passed |

### Second check of 2026-11-17: Clips page and editor

`build/ui-work/shots.mjs` runs `build/Release/OBSReplay.exe` headless, isolated and muted (port 9462) against 20 clips with saved dates from today to two years back. A copy of `ping.exe` named `NVIDIA Overlay.exe` stands in for the NVIDIA overlay. Screenshots are in `build/ui-shots`.

| Check | Result |
|---|---|
| Day headings: Today, Yesterday, dates with "3d ago", "1w ago", "1m ago", "1y ago", and the year on older dates | Passed |
| Cards under a heading show the time alone | Passed |
| Date filter: Today, Last 7 days, Last 30 days, Any time | Passed |
| Select mode: check boxes, Shift+click to select and again to deselect, fully grey unselected cards, selected cards at 104% with no edge, the bar | Passed |
| The search field is 132px wide at rest and 270px while focused or holding text | Passed |
| The selection survives opening a clip and changing the filter; Escape clears it | Passed |
| Delete of two selected clips asks first, removes both files and leaves 18 listed | Passed |
| Clip menu rows include **Select** and **Copy** | Passed |
| Thumbnails stored on disk (one per distinct file), read back with the clip length, refused for a file outside the list | Passed |
| A renamed clip keeps its picture; a deleted clip's picture is removed within seconds | Passed |
| Each day heading counts its clips | Passed |
| After a page reload thumbnails and lengths show from disk | Passed |
| Overlay notice in setup and toast in the app while `NVIDIA Overlay.exe` runs | Passed |
| One **Start with Windows** switch in Settings | Passed |
| Total | 38 of 38 |

Editor (`build/edtest`, run by a second engineer the same day):

| Check | Result |
|---|---|
| Max size 10 MB, first 30 s | 9,047,337 bytes, H.264 540p, 1 encode, 1.8 s |
| Max size 25 MB, first 60 s | 21,607,574 bytes, H.264 720p, 1 encode, 4.1 s |
| Max size 50 MB, full 118 s | 45,370,165 bytes, H.264 720p, 1 encode, 7.6 s |
| Max size 100 MB, full 118 s | 90,689,838 bytes, H.264 1080p, 1 encode, 13.4 s |
| Max size 500 MB, full 118 s (HEVC source, 206 MB) | 249,271,225 bytes, H.264 1080p, 1 encode, 13.7 s. The bitrate stops at what the source needs |
| Max size 10 MB, full 118 s | Shown as unavailable; `start_export` refuses it |
| Retry: an edit estimated to fit came out at 208 MB against 100 MB | Second pass gave 49,708,424 bytes |
| Cancel during a Max size export | No file left |
| Every Max size export above | H.264 video |
| Audio track choice across smart cut, re-encode and Max size (stream layout and tone content) | 13 of 13 |
| Editor suite `build/editor-check/check.mjs`, updated for the current pages | Main 118 of 118, no-FFmpeg 7 of 7 |

Live run of version 1.0.2 on the development PC, started as `dist\OBSReplay.exe --portable` beside an installed 1.0.1 and a running OBS session:

| Check | Result |
|---|---|
| Connects to the running OBSReplay session of OBS; status **Capturing**, capture ready, no error | Passed |
| Both shortcuts register | Passed |
| A 1.0.1 settings file gains `settings_version` 2 with **Start replay capture automatically** on | Passed |
| All 29 videos in the clips folder are listed | Passed |
| The profile update waits because OBS is open (session log: "OBS profile update waits: OBS is open") | Passed |
| The update check reports 1.0.1 as the latest release and offers nothing | Passed |
| The installed copy stays at 1.0.1 (`--portable` installs nothing) | Passed |

The live run read state only. It did not stop capture, save a replay or play a clip.

Play from a removed section (`build/edtest/play-removed.mjs`, after 1.0.2): one press of Play starts playback from inside a removed section for cuts made at the playhead, for a cut dragged to an arbitrary time (three positions), and for a removed ending. Playback that runs into a removed section carries on past it, including one with a dragged cut. 9 of 9 passed. The dragged-cut cases failed on the 1.0.2 code: the video stayed seeking at the cut. The editor suite still passes 118 of 118.

Track volumes in the export (`build/edtest/volumes.mjs`, after 1.0.3), measured on tone clips with FFmpeg: Microphone at 10% came out 20.0 dB down in the mix and on its own track, with Desktop unchanged, in the smart cut, the re-encode and a Max size export; both tracks at 50% came out 6.0 dB down; a track at 0% was left out; a single-track clip at 25% came out 12.0 dB down. 14 of 14 passed. The audio track suite (13 of 13), the playback test (9 of 9) and the editor suite (118 of 118) still pass.

Not exercised in the second check: fullscreen on a second monitor, copy to the clipboard, and dragging a clip out of the window. These need real mouse input or would overwrite the user's clipboard. The Max size failure after four encodes and the x264 Max size path were read in code only; NVENC always fit on the test machine.

Not exercised: the profile update at launch, Start and Stop against a live OBS, the new buffer memory figure inside OBS, sizes of 1 GB and above in the page, and the display fix button. These need a live OBS session or real hardware changes.

## How to run checks

Checks run the real `OBSReplay.exe` with its embedded WebView2 in a headless, isolated mode. A script drives the page over the WebView2 remote debugging port.

### Environment variables

| Variable | Value | Effect |
|---|---|---|
| `OBSREPLAY_HEADLESS` | `1` (any value) | Windows open off-screen (-32000, -32000), at the bottom of the z-order, without focus. The app never opens Explorer |
| `OBSREPLAY_DATA_DIR` | Absolute directory | The app reads and writes `settings.json`, `clips.json` and `editor.json` there. The run gets its own single-instance lock and its own WebView2 profile (`<dir>\WebView2`) |
| `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` | `--remote-debugging-port=<port> --disable-features=CalculateNativeWinOcclusion --disable-backgrounding-occluded-windows` | Exposes the page to a DevTools client. Keeps the off-screen page rendering |

### Rules

- Set `OBSREPLAY_DATA_DIR` to an absolute path. A relative path makes the app throw at startup.
- An isolated run can run beside the user's normal OBSReplay.
- OBS configuration in an isolated run still refers to the current Windows user's installed OBS.
- List debug targets at `http://127.0.0.1:<port>/json/list`. Main-window and `editor.html` pages appear as separate targets.
- Add `--mute-audio` to `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` to keep playback checks silent.
- Seed `settings.json` in the data folder with `save_sound: false`, `replay_enabled: false`, `start_at_login: false` and `settings_version: 2`. The app merges the file over its defaults. Without `settings_version` the app treats the file as one from 1.0.1 and turns `replay_enabled` on.
- Do not move the user's mouse or send desktop keyboard input.
- Keep disposable clips, data folders and evidence under the ignored `build/` folder.

### Editor suite

| Item | Value |
|---|---|
| Script | `build/editor-check/check.mjs` |
| Phases | `main` (port 9432), `noffmpeg` (port 9433) |
| Run | `node build/editor-check/check.mjs` or `node build/editor-check/check.mjs noffmpeg` |
| Executable under test | `build/Release/OBSReplay.exe` |
| Data folder | `build/editor-check/data-<phase>` (recreated each run) |
| Report | `build/editor-check/report.json` (latest result per phase) |
| No-FFmpeg phase | Restricts `PATH` to the Windows folders and redirects `LOCALAPPDATA` so the app finds no FFmpeg |
| Playback check | `build/editor-check/playback.mjs` |

### Evidence folders

| Folder | Area |
|---|---|
| `build/editor-check` | Clip editor |
| `build/mp4-check` | MP4 output |
| `build/compression-check` | Compression and Apply restart |
| `build/quality-check` | Quality selector |
| `build/reveal-check` | Explorer reveal |
| `build/player-actions-check` | Viewer actions |
| `build/player-check` | Viewer playback and thumbnails |

## Release artefact

| Check | Result | Evidence |
|---|---|---|
| `dist/OBSReplay.exe` size | 2,133,504 bytes | File |
| SHA-256 | `0DA3842441F1E3EF668A8F4FC114FF967543BF13382F14B3E2E21A227D034F4B` | File |
| Toolchain | MSVC 19.51, saucer 8.0.5, static MSVC runtime, static WebView2 loader | Release compile |
| Imports | Windows system DLLs only | Import table |
| Embedded files | All three frontend files of that build, the Inter font and its OFL verified byte-for-byte inside the EXE | Byte comparison |
| Distribution directory | One file | Directory listing |
| Release compile and JavaScript syntax checks | Passed | Build output |

## Clip editor

### Suite totals

| Run | Main checks | No-FFmpeg checks | Notes |
|---|---|---|---|
| Export run (frame-exact quality 10, quality scale, tray **Quit**) | 119 of 119 passed | 7 of 7 passed | |
| Preview and mixer run (preview cache, instant export, mixer, scale, time formats) | 106 of 107 passed | 7 of 7 passed | The one failure was a test tolerance, see below |
| Base editor run | 54 of 54 passed | 4 of 4 passed (FFmpeg hidden) | `build/editor-check/check.mjs`, report in `build/editor-check/report.json` |

Tolerance failure in the preview and mixer run:

- An Instant export of three sections measured 72.36 s against the footer's 72.15 s.
- Cause: stream-copy cuts run about 0.07 s long per section.
- The check allows 0.1 s per section.

### Quality 10 export (smart cut)

| Check | Result | Evidence |
|---|---|---|
| 118 s HEVC replay, 3 sections, 69.15 s kept: time | 2.8 s | Editor suite |
| Same: frame count | 4,149 frames (kept length × 60) | Frame comparison |
| Same: bit-identical frames | 3,849 identical to the source | Frame comparison |
| Same: re-encoded edge frames | 41.4 dB or better | Frame comparison |
| Same: duration | 69.150 s | FFprobe |
| Same: decode errors | None | Full decode |
| Same: audio | All three tracks with their names (Mix, Desktop, Microphone) | FFprobe |
| H.264 B-pyramid test clip: frame count | 1,200 frames | Frame comparison |
| Same: bit-identical frames | 1,000 | Frame comparison |
| Same: worst edge frame | 49.6 dB | Frame comparison |
| Same: time | 1.9 s | Editor suite |
| Playback of both exports through every splice in WebView2 | 0 corrupted frames | `build/editor-check/playback.mjs` |

### Quality scale and size

| Check | Result | Evidence |
|---|---|---|
| Quality 5 export of the same edit | HEVC (hvc1), 43.8 MB | Export file |
| Quality 10 export of the same edit | 140.8 MB | Export file |
| Source file | 206.1 MB | Source file |
| Size estimate error, quality 5 | Within 4% | Estimate against file |
| Size estimate error, quality 10 | Within 13% | Estimate against file |
| Changing quality moves export-bar controls | No movement. The suite found a 4 px label shift at "10"; it is fixed | Layout check |
| Real 118 s HEVC replay, 3 tracks, quality 3: duration | 70.144 s against 70.15 s expected | FFprobe |
| Same: tracks | All 3 | FFprobe |
| Same: size | 77.8 MB against the 206.1 MB original | Export file |

### Export modes and timing

| Check | Result | Evidence |
|---|---|---|
| Instant export, 100 s edit of the 118 s HEVC replay | 0.6 s. HEVC and three AAC tracks copied | Editor suite |
| Re-encode with NVENC p4, same edit | 13.4 s, frame-exact, capped at 1.5× the source bitrate | Editor suite |
| NVENC export of a 30 s H.264 test clip with 2 AAC tracks | 20.000 s, both tracks, decoded completely | FFprobe, full decode |
| Same export in recent clips | Appeared first | Clips list |
| Same export revealed in Explorer | Explorer opened with the file selected | Shell automation |
| Cancel during export | No partial file left | Folder listing |
| Hardware encoder fallback | AMD and Intel hardware encoders are not used. Exports fall back to x264 | Code path |

### Preview and seeking

| Check | Result | Evidence |
|---|---|---|
| Seek on original clips with media range responses capped at 4 MB | 64 ms (about 700 ms without the cap) | Editor suite |
| Seek on the 720p GOP-6 preview | 32 to 46 ms | Editor suite |
| Preview ready after opening a 2-minute clip | About 7 s | Editor suite |
| "In export" time widening the window past its edge | Layout overflow found by the suite, fixed and re-verified | Layout check |

### Behaviour covered by the base editor run

- **Edit** beside **Open** on Capture and Clips.
- One editor window per clip.
- Preview video letterboxed inside the stage at the default and minimum (900×640) sizes.
- The default window gives a 16:9 frame its full height.
- Split with S and with the button.
- Click, Shift and Ctrl selection.
- Delete, R restore, I/O trims.
- Cut drag and double-click join.
- Undo and redo.
- Wheel zoom and Shift+wheel pan.
- Space plays after a toolbar click.
- Playback skips removed sections.
- Close and quit warnings.

## MP4 output

Generated profiles use MP4 with `movflags=+faststart`. The profile-policy update goes through the buffer-preserving restart flow.

| Check | Result | Evidence |
|---|---|---|
| Native assertions: MP4 profile generation, separate audio tracks, migration marker | 25 passed | Native test |
| Installed OBS session applies the update and resumes capture | Passed | Live OBS |
| Saved replay | 17,287,381 bytes, 8.267 s, MP4, 1920×1080/60 HEVC, three AAC tracks | `build/mp4-check` |
| Codec, quality and other preferences | Unchanged | Settings comparison |
| Container | FFprobe recognised MP4 | FFprobe |
| Faststart | Atom scan confirmed playback metadata precedes media data | Atom scan |
| Decode | Entire video and all audio tracks decoded without errors | Full decode |
| WebView2 checks: history, thumbnail generation, `video/mp4` MIME and byte ranges, modal playback and seeking, continued capture | 5 passed | Embedded WebView2 |
| Existing MKV recordings | Remain available | Clips list |

- No files were uploaded to Discord or another site.
- H.264 remains the broad-compatibility codec option.

## Compression and automatic Apply restart

### Offline encode comparison

| Check | Result | Evidence |
|---|---|---|
| Supplied Fortnite replay | 95.067 s, 1920×1080/60 HEVC, 713,338,284 bytes | Source file |
| Copy encoded with the High policy | 190,995,218 bytes, 73.2% smaller | `build/compression-check` |
| Duration, resolution, frame rate, audio tracks of the copy | Same as the source | FFprobe |
| Original file | Untouched | File |
| Sampled VMAF against the existing compressed original | 96.39 | VMAF |
| Cropped comparisons | Inspected at three timestamps | Images |

- The VMAF score measures additional loss against an already compressed file. It does not measure quality against a lossless source.
- FFmpeg served as a local test tool and stays outside the EXE.

### Live OBS session

| Check | Result | Evidence |
|---|---|---|
| OBS log: encoder settings | CQVBR, the tested High target and bitrate ceiling, P5, quarter-resolution multipass, adaptive quantization | OBS log |
| Subsequent OBS replay | 179.925 s, 289,787,350 bytes, 1080p/60 HEVC | Replay file |
| Render and encode during runtime checks | Active 60 FPS, zero skipped render or encode frames | OBS stats |

The live session is a short runtime check. It is not a long GPU-load benchmark.

### Policy and Apply checks

| Check | Result | Evidence |
|---|---|---|
| Native policy checks: quality choices, encoder JSON, bitrate scaling with resolution and FPS, persistence, rejecting an unrelated process path | 116 passed | Native test |
| Live Apply: detect the old compression policy, save the current replay, close, reconfigure and reopen OBS, resume the owned buffer, retain High/1080p/60/HEVC | 9 passed | Live OBS |
| Upgrade duration, including the buffer save | 4.4 s | Live OBS |
| Shutdown wait exposed by the first test | Fixed by disconnecting the control socket before requesting OBS close. No process is forced to stop | Live OBS |
| Live edge: a full recording blocks restart and preserves capture and settings | Passed (part of 7) | Live OBS |
| Live edge: a failed audio-device configuration restores the prior settings and resumes capture | Passed (part of 7) | Live OBS |
| Live edge: a no-change Apply leaves the OBS process running | Passed (part of 7) | Live OBS |
| Simulated disconnected microphone preference with video capture active: Apply preserved the buffer, repaired the preferences, resumed capture without stale errors, restored the original user preferences | 4 passed | Live OBS |
| Embedded UI: named quality, settled and dirty Apply states, automatic-restart guidance, minimum-size layout, **Discard**, compression-upgrade action without unrelated edits | 7 passed | Embedded WebView2 |
| Backup during repair | The code backs up any verified active scene before stopping. It aborts if it cannot verify and save that scene | Code review, `build/compression-check` |

- Pre-restart backups remain in the clips folder.
- Test-only full recordings sit under ignored build evidence.
- No stream was started during testing.

## Quality selector

Quality choices are Very high, High, Balanced, Low and Very low. High sits between the former High and Balanced steps. Saved presets keep their encoder values.

| Check | Result | Evidence |
|---|---|---|
| Native assertions: accepted and rejected values, encoder JSON, unchanged encoder options, persistence, legacy compatibility | 173 passed | Native test |
| Embedded WebView2: named choices without numeric labels, the existing Balanced selection, dirty-edit retention, minimum-window layout, the submitted High value | 7 passed | `build/quality-check` |
| Build and JavaScript syntax checks | Passed | Build output |
| User settings and the existing OBS process | Preserved | Isolated run |

- The UI Apply payload was recorded and routed to a harmless native refresh.
- Encoder output and persistence were checked separately.
- No live encoder reconfiguration occurred in these checks.
- These checks ran before the compression retune. Saved quality identifiers and labels stay compatible. Encoder targets follow the compression policy.

## Explorer reveal

The shell call returns `E_FAIL` for forward-slash paths and `S_OK` for the same file with native separators. `open_path` normalises separators before creating the shell item.

| Check | Result | Evidence |
|---|---|---|
| Native: backslash, forward-slash, mixed, Unicode and comma paths, missing-file rejection | 5 passed | Native test |
| Embedded app through the real Explorer handler, without interception | 4 passed | `build/reveal-check` |
| Viewer **Show in folder** selects the exact file | Passed | Explorer automation |
| Clips list **Show in folder** selects the exact file | Passed | Explorer automation |
| Playback stays paused | Passed | Embedded app |
| Controller actions finish without errors | Passed | Embedded app |
| Release compilation | Passed | Build output |
| User settings and the existing OBS process | Preserved | Isolated run |

## Replay viewer and clips history

### Viewer actions

The viewer has **Delete** with permanent-removal confirmation, **Show in folder** and **Open file**. At test time Capture listed five recent replays.

| Check | Result | Evidence |
|---|---|---|
| Embedded WebView2 checks | 27 passed | `build/player-actions-check` |

Covered:

- All three controls.
- Correct shell-action paths.
- Confirmation wording and default **Cancel** focus.
- Paused playback.
- Cancellation and Escape preserve the file and resume playback.
- Minimum-window layout.
- Native deletion.
- Persisted history.
- The next-older replay fills the five-row list.
- Unrelated clips preserved.
- A file held without delete sharing is rejected with visible recovery.
- Deletion outside confirmed history is rejected.

Notes:

- Open and reveal UI dispatches were recorded and routed to a harmless native refresh during these isolated checks. Their Windows shell handlers are unchanged.
- Only disposable test files were deleted.
- The release remains one EXE with matching embedded frontend and chime bytes.

### Thumbnails, playback and history pruning

| Check | Result | Evidence |
|---|---|---|
| Feature checks in the embedded WebView2 | 29 passed | `build/player-check` |
| Further checks: startup pruning, an unreadable video's external-playback recovery, zero thumbnail and history DOM mutations across unchanged polls, isolated startup and capture settings | 4 passed | `build/player-check` |
| Release compilation and JavaScript syntax checks | Passed | Build output |

The 29 feature checks cover:

- H.264, HEVC and AV1 MKV thumbnail decoding and seeking.
- Playback progression.
- Bounded, suffix and open-ended byte ranges.
- Rejected invalid ranges and unknown paths.
- HEAD requests.
- Default and minimum-window layouts.
- Escape and focus restoration.
- Stream release on close.
- External deletion during playback.
- Advancing Latest replay.
- Persisted history removal.
- The empty state.

Notes:

- Tests used disposable copies under `build/player-check`. No user clips or OBS configuration changed.
- Playback streams local files through a read-only WebView2 response. It needs no server, FFmpeg runtime or additional distribution file.
- Codec availability depends on Windows. All three app-supported codecs played on the test machine.
- Missing codecs show an external-player recovery option.

## Save chime

| Check | Result | Evidence |
|---|---|---|
| Save notification sound | A 102,618-byte PCM WAV chime (`resources/replay-saved.wav`) | EXE WAVE resource |
| Embedded bytes | Verified byte-for-byte through the EXE's WAVE resource | Byte comparison |
| Confirmed-save gate and sound toggle | Unchanged | Code |

## Capture engine and startup

The capture, setup, transport and startup checks ran on the capture and startup release. Later viewer work did not change those engine paths.

### Live capture

Test machine: OBS 32.2.2, NVIDIA RTX 5070 Ti.

| Check | Result | Evidence |
|---|---|---|
| Authenticated connection | Passed | Live OBS |
| Dedicated profile and source validation | Passed | Live OBS |
| Replay start | Passed | Live OBS |
| Confirmed save | Passed | Live OBS |
| History and sorting | Passed | Live OBS |
| Live app settings | Passed | Live OBS |
| Rejected disruptive settings | Passed | Live OBS |
| Damaged-source protection | Passed | Live OBS |
| Stop and reconnect | Passed | Live OBS |
| Saved file | 6.233 s H.264 1280×720/30 FPS MKV with separate AAC tracks, decoded completely with FFmpeg, exit 0 | Full decode |
| INI backslash-path bug found during this check | Fixed. The complete capture test passed on rerun | Live OBS |

### Setup and persistence

| Check | Result | Evidence |
|---|---|---|
| Standalone integration assertions: Unicode paths, literal backslash escape sequences, ownership collisions, conditional reset, forced WebSocket and settings write failures with rollback | 46 passed | Native test |
| Focused path regression assertions | 12 passed | Native test |

### Transport

All passed against a disposable protocol server:

- Challenge authentication.
- Fragmented messages.
- Unrelated response IDs.
- Response timeout and recovery.
- Idle connection.
- Cancellation of a blocked receive.

### Startup and tray

| Check | Result | Evidence |
|---|---|---|
| Current-user Run entry: enabled, verified as a quoted executable plus `--tray`, disabled, restored exactly | 4 assertions passed | Registry |
| `--tray` launch | Stayed hidden after readiness | Live process |
| Second launch | Reopened the same process and exited with success | Live process |
| Hide and show | Preserved the process | Live process |

## Main window UI

| Check | Result | Evidence |
|---|---|---|
| Embedded WebView checks | 20 passed | Embedded WebView2 |
| PlayerCard design adaptation: isolated interaction and layout checks | 29 passed | Embedded screenshots |

The 20 embedded WebView checks cover:

- Native RPC.
- Bundled font.
- Default shortcut registration.
- Default and minimum layouts.
- Pinned actions.
- Categories.
- Accessible navigation.
- Dirty edits and **Discard**.
- No idle animation.
- Zero DOM mutations across unchanged polls.
- 125%, 150% and 200% emulated pixel scales.

Notes:

- Windows display settings were not changed.
- Native sizing fits and centres the window within its monitor work area, including high DPI.
- Synthetic fixture evidence is kept separate from native capture evidence.

## Test hygiene

- Disposable checks and recordings sit under ignored `build/`.
- The native OBS test restored the original Untitled profile and collection selection and the shared WebSocket settings.
- The native OBS test removed its generated integration and retained its test video.
- The startup registry check restored its original value.
- No PlayerCard files were changed.
- Isolated checks preserved user settings and the existing OBS process.

## Known limits and untested areas

| Area | Status |
|---|---|
| Tray menu fix (open the menu only on `WM_CONTEXTMENU` for version-4 icons, no nested menus, quit after the menu closes) | Not exercised by a test. It needs real input on the desktop |
| AMD and Intel encoding | Not exercised on the test machine |
| Real fullscreen-game hooks | Not exercised |
| Anti-cheat compatibility | Not exercised |
| Microphone hardware capture | Not exercised |
| Long GPU-load benchmark of the compression policy | Not run. Only a short runtime check exists |
| Quality against a lossless source | Not measured. VMAF compares against a compressed original |
| Live encoder reconfiguration in the quality-selector checks | Not performed |
| Codecs missing from Windows | Playback depends on Windows codecs. Only the test machine's set was checked |

- Untested capture features use OBS's own sources and encoders.
- The verified live capture path was NVIDIA H.264 desktop capture with the microphone disabled.
- OBS Studio and Evergreen WebView2 remain installed prerequisites.
- Fixture screenshots do not prove native recording.
