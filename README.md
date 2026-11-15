# OBSReplay

A compact Windows replay controller written in C++23 with [saucer](https://github.com/saucer/saucer). OBS handles capture and encoding; OBSReplay handles setup, shortcuts, settings and confirmed replay saves.

The application lives at repository root. `ShadOBS-main` is an untouched third-party reference, outside the build.

## Run

1. Make sure Microsoft Edge WebView2 Runtime is installed (it ships with Windows 11).
2. Run `dist/OBSReplay.exe` from a location you intend to keep. Guided setup opens on first launch.
3. **OBS Studio:** if OBS 31+ isn't installed, select **Install OBS Studio**. OBSReplay installs (or updates) it with Windows Package Manager (`winget install OBSProject.OBSStudio`); Windows asks for permission once. Without winget, **Download from obsproject.com** opens the download page and setup continues as soon as OBS is installed; **Locate obs64.exe…** picks a copy in another folder.
4. **Capture:** choose what to record, the display, replay length, quality, microphone, clips folder and Start with Windows. Encoder, frame rate, audio devices and shortcuts start with defaults (the best detected hardware encoder) and stay editable in Settings.
5. Select **Set up and start capture**. Any open OBS closes gracefully, the dedicated profile is written, OBS reopens minimized in the tray and the replay buffer starts. Press **Ctrl+Shift+F10** to save a replay and **Alt+X** to show or hide the window. Shortcuts are configurable.

Setup creates an OBSReplay profile and scene collection and marks OBS's first run as complete, so OBS's auto-configuration wizard doesn't open over the hidden session. If an open OBS is streaming or recording, setup stops and asks you to stop it first; OBS's own exit confirmation also stays in effect, and no process is forced to stop. The dedicated session starts with `--disable-updater`, so an update prompt never appears over a game; opening OBS normally still checks for updates. The companion starts with Windows by default after setup; this can be disabled in App settings. Microphone recording is opt-in. Settings → Capture also offers **Set up and start** for anyone who skips the guided setup.

Capture modes: **Game or display** (the default for new setups) layers a fullscreen-game capture over display capture, so the selected display is recorded, including other windows and notifications, whenever no compatible fullscreen game is captured. **Fullscreen games only** never falls back to desktop capture. **Entire display** always records the selected monitor.

OBSReplay runs alongside OBS. Once capture is enabled, login startup launches the companion and starts OBS minimized with its rolling replay buffer. OBS performs all capture and encoding. Closing the companion window keeps it in the tray. Quitting the controller leaves OBS and capture running, but the controller's shortcuts stop. Stopping capture discards unsaved buffer history.

Apply handles capture and audio changes by saving the current healthy replay buffer, closing the dedicated OBS session, applying the settings and reopening OBS. Active replay capture resumes afterward. Streams, full recordings and other OBS profiles prevent a disruptive restart. App-only settings apply while capture continues. Saved replays use MP4 with web playback metadata at the beginning of the file, preserving separate AAC audio tracks. H.264 offers the widest playback compatibility; HEVC and AV1 remain available for efficient compression. Replay length is an upper limit constrained by the configured memory budget.

Quality choices are **Very high, High, Balanced, Low and Very low**. NVIDIA recording combines quality targets with bitrate ceilings scaled to resolution and frame rate, plus a more efficient encoding preset. Other encoders use retuned quality targets. Resolution and FPS stay as selected; higher quality allows larger files. Balanced remains the default. Existing installations can select **Apply improved compression** without editing their preferences.

Latest replays shows the five most recent videos; Clips shows the full recent history. Select a replay's thumbnail or name to watch it inside the app, with playback controls and a scrubbable timeline. The player provides **Show in folder** to select the video in Explorer, **Open file** to use your default video app, and **Delete** with confirmation of permanent removal. Deleted or moved files disappear from recent history automatically; deleting the video being watched closes its player. Playback uses WebView2 and installed Windows codecs, with an external-player option if a codec is unavailable.

**Edit** (on Capture, Clips and the player) opens the clip in its own editor window. Scrub the timeline (filmstrip and waveform), press **S** to split at the playhead, click sections to select them (Shift for a range, Ctrl to add) and press **Delete** to remove or restore them; **I**/**O** delete everything before/after the playhead, cuts can be dragged or double-clicked away, and undo/redo cover every change. Preview playback skips removed sections. **Preview quality** in the titlebar sets what the editor shows: **Full** (default) plays the original recording at full resolution; **Half** and **Quarter** build a high-quality scrubbing copy at half or quarter height on the GPU (each built once per editor, the original keeps playing until it's ready). Once the clip is analysed, the editor also copies each audio track. The transport shows one master volume; the track button next to it opens per-track sliders named from the recording (OBSReplay records Desktop and Microphone beside the Mix). The temporary files are deleted when the editor closes. **Export video** writes one MP4, named `… (edited).mp4`, into an **Edited Exports** folder beside the original (for app-sorted clips, inside that game's folder). Every export cuts on the exact frame. **Quality 10 · Original** (default) keeps the recording's video untouched between keyframes and re-encodes only the frames from each cut to the next keyframe (at most 2 s on OBS replays) with the same codec at near-lossless quality, so a typical export takes a couple of seconds. **Quality 1–9** re-encode the whole video at a share of the original bitrate (78% down to 10%; 1–2 also cap at 720p), keeping HEVC when the replay is HEVC and NVIDIA NVENC is available (otherwise H.264, x264 on CPU), so each step down gives a visibly smaller file. The export bar shows the estimated size. Audio is re-encoded to AAC so every track is cut to the exact sample, and track names are kept. When the export finishes, File Explorer opens with the new file selected and it appears at the top of recent clips. The original replay is never modified. Quality, time format (duration, clock or frame timecode), master and per-track volumes, preview cuts and the titlebar interface scale (75–150%) are remembered. Press **?** in the editor for all shortcuts.

Exporting needs an installed FFmpeg build (for example `winget install Gyan.FFmpeg`); it is found on PATH, in WinGet/Scoop/Chocolatey locations, or chosen with **Locate ffmpeg.exe**. FFmpeg is not bundled, and editing and previewing work without it.

Settings and confirmed replay history are stored in `%LOCALAPPDATA%\OBSReplay`. The Help screen offers integration reset; it removes owned OBSReplay configuration and conditionally restores shared WebSocket changes while leaving clips intact. Software notices are embedded in the EXE and available from Help.

## Build

Use Windows x64, Visual Studio C++ desktop tools (MSVC 19.44+), a recent Windows SDK, CMake 3.25+ and Git. The first build downloads pinned dependencies.

```powershell
./build.ps1
```

The output is `dist/OBSReplay.exe`. The frontend (including the editor window), application icon, notices, saucer, JSON library and MSVC runtime are compiled into that one file. OBS Studio (installed by guided setup when missing) and Evergreen WebView2 remain separate installs.

The frontend uses plain HTML, CSS and JavaScript; there is no npm build or frontend framework. Native worker threads own OBS transport and setup. The UI only submits actions and reads state; a completed OBS save event and file existence confirm a replay.

For isolated app-data checks, set `OBSREPLAY_DATA_DIR` to an absolute directory before launching. That run uses its own single-instance lock and WebView2 profile, so it can run beside your normal OBSReplay. Setting `OBSREPLAY_HEADLESS=1` as well (test use) opens windows off-screen without focus and never opens Explorer. OBS configuration still refers to the current Windows user's installed OBS.

The UI follows the user's PlayerCard reference: navy/blue, bundled Inter, a compact rail and quiet settings rows. See [SCOPE.md](SCOPE.md) for reviewed behavior, [DESIGN.md](DESIGN.md) for the UI contract and [VALIDATION.md](VALIDATION.md) for release evidence and hardware coverage.
