# OBSReplay

A compact Windows instant replay controller for installed OBS Studio, written in C++ with saucer. The distributable is one small EXE containing its UI and app dependencies. OBS and Microsoft's Evergreen WebView2 Runtime are installed prerequisites.

The user wants a sleek dark interface matching the established PlayerCard project: navy/blue surfaces, bundled Inter, compact desktop chrome and useful quick motion. This explicit reference supersedes the initial neutral-black proposal. The daily task is to check capture status and save the last few minutes with a global hotkey. OBS remains open and performs capture; OBSReplay runs in the tray for shortcuts and configuration.

New application code belongs at repository root, outside the third-party ShadOBS-main reference. This is an original implementation of the concept. Avoid needless frameworks, abstractions, or system changes.

Setup creates a dedicated OBSReplay profile and scene collection. Respect existing OBS sessions and profiles. Microphone recording is opt-in. Start with Windows is enabled by default and clearly staged during setup; the user can disable it. Startup changes persist only through successful configuration/Apply. Desktop capture is explicit: the default Game or display mode is labelled as recording the display between games, and Fullscreen games only never falls back to the desktop. First run is a guided setup that installs OBS when needed, closes and reopens OBS on its own, and starts capture in one step. OBS crash recovery and Windows capture settings remain under the user's control.

The default window is 1040×760 logical pixels, minimum 840×620. Capture and Clips use the upper navigation rail; Settings and Help use its lower section. Settings groups Capture, Audio and App, with one explicit Apply model. The app's persisted Animations setting controls its transitions independently of Windows, following PlayerCard's explicit policy. Changes and verification must use isolated/headless tools rather than desktop mouse or keyboard input.

Assumptions: Windows x64; OBS 31 or newer; a single local OBS connection; local files only. The product name OBSReplay is provisional, based on the project folder. Game folder naming from the foreground app is best effort, not proof that OBS hooked that game.
