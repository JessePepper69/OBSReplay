# OBSReplay

A compact Windows replay controller written in C++23 with [saucer](https://github.com/saucer/saucer). OBS handles capture and encoding; OBSReplay handles setup, shortcuts, settings and confirmed replay saves.

The application lives at repository root. `ShadOBS-main` is an untouched third-party reference, outside the build.

## Run

1. Install OBS Studio 31+ and Microsoft Edge WebView2 Runtime.
2. Run `dist/OBSReplay.exe` from a location you intend to keep.
3. Close OBS, choose capture mode and destination, then select **Configure OBS**.
4. Select **Start capture**. Press **Ctrl+Shift+F10** to save a replay and **Alt+X** to show or hide the window. Shortcuts are configurable.

Setup creates an OBSReplay profile and scene collection. The companion starts with Windows by default after setup; this can be disabled in App settings. Microphone recording is opt-in. Game mode captures a compatible fullscreen game; display mode records the selected entire monitor, including other windows and notifications. Game mode never falls back to desktop capture.

OBSReplay runs alongside OBS. Once capture is enabled, login startup launches the companion and starts OBS minimized with its rolling replay buffer. OBS performs all capture and encoding. Closing the companion window keeps it in the tray. Quitting the controller leaves OBS and capture running, but the controller's shortcuts stop. Stopping capture discards unsaved buffer history.

Apply handles capture and audio changes by saving the current healthy replay buffer, closing the dedicated OBS session, applying the settings and reopening OBS. Active replay capture resumes afterward. Streams, full recordings and other OBS profiles prevent a disruptive restart. App-only settings apply while capture continues. First-time setup still requires OBS closed once. Saved replays use MP4 with web playback metadata at the beginning of the file, preserving separate AAC audio tracks. H.264 offers the widest playback compatibility; HEVC and AV1 remain available for efficient compression. Replay length is an upper limit constrained by the configured memory budget.

Quality choices are **Very high, High, Balanced, Low and Very low**. NVIDIA recording combines quality targets with bitrate ceilings scaled to resolution and frame rate, plus a more efficient encoding preset. Other encoders use retuned quality targets. Resolution and FPS stay as selected; higher quality allows larger files. Balanced remains the default. Existing installations can select **Apply improved compression** without editing their preferences.

Latest replays shows the five most recent videos; Clips shows the full recent history. Select a replay's thumbnail or name to watch it inside the app, with playback controls and a scrubbable timeline. The player provides **Show in folder** to select the video in Explorer, **Open file** to use your default video app, and **Delete** with confirmation of permanent removal. Deleted or moved files disappear from recent history automatically; deleting the video being watched closes its player. Playback uses WebView2 and installed Windows codecs, with an external-player option if a codec is unavailable. FFmpeg is not required or bundled.

Settings and confirmed replay history are stored in `%LOCALAPPDATA%\OBSReplay`. The Help screen offers integration reset; it removes owned OBSReplay configuration and conditionally restores shared WebSocket changes while leaving clips intact. Software notices are embedded in the EXE and available from Help.

## Build

Use Windows x64, Visual Studio C++ desktop tools (MSVC 19.44+), a recent Windows SDK, CMake 3.25+ and Git. The first build downloads pinned dependencies.

```powershell
./build.ps1
```

The output is `dist/OBSReplay.exe`. The frontend, application icon, notices, saucer, JSON library and MSVC runtime are compiled into that one file. OBS Studio and Evergreen WebView2 remain installed prerequisites.

The frontend uses plain HTML, CSS and JavaScript; there is no npm build or frontend framework. Native worker threads own OBS transport and setup. The UI only submits actions and reads state; a completed OBS save event and file existence confirm a replay.

For isolated app-data checks, set `OBSREPLAY_DATA_DIR` to an absolute directory before launching. OBS configuration still refers to the current Windows user's installed OBS.

The UI follows the user's PlayerCard reference: navy/blue, bundled Inter, a compact rail and quiet settings rows. See [SCOPE.md](SCOPE.md) for reviewed behavior, [DESIGN.md](DESIGN.md) for the UI contract and [VALIDATION.md](VALIDATION.md) for release evidence and hardware coverage.
