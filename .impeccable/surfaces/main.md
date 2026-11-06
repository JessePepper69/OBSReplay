# OBSReplay desktop surface

Mode: Operate. Build path: code-first. Authority: the user's explicit PlayerCard reference. The previous neutral-black direction was rejected and is superseded. Revised scope independently accepted before implementation.

## Direction contract

THESIS: Keep replay controls as approachable as the user's PlayerCard desktop app. Actual capture readiness and Save replay lead; the capture engine remains in OBS.

OWN-WORLD: PlayerCard navy canvas/surfaces, bundled Inter, blue action/selection, steel-blue controls, compact ruled settings and semantic switches. Its established colors and gradients override generic skill bans. No XP, gold, artwork or activity metrics.

STORY: Configure dedicated OBS capture, start the replay buffer, save a moment, and find its file. Minimize or close to tray to keep shortcuts available. Start with Windows is staged on by default and can be changed.

FIRST VIEWPORT: Default1040×760, minimum840×620. Custom38px titlebar over a56px fixed icon rail; Capture/Clips above, Settings/Help below. Hover/focus expands labels to176px over content. Capture state/action panel, four-column summary, destination and latest-file rows. Settings shows Capture/Audio/App tabs, aligned setting rows and a fixed Apply footer. First setup includes a visible Windows-startup switch.

FORM: Adapt the existing PlayerCard visual system directly, without another concept lottery. Real source and artifacts inspected read-only; no PlayerCard files or user desktop input changed.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance. No raster is shipped; bundled Inter and its unmodified OFL have local provenance.

## Reference evidence

Read PlayerCard/AGENTS.md and relevant source under src/PlayerCard.Summary/wwwroot. Source tokens: canvas#10151d, surface#19222e, Inter, blue#0074d9; titlebar38px; rail56px/176px; rows44px; control radius7px; switches42×24px; fast130ms/ease(.2,.8,.2,1). Reference source supports native-prepared entries, cancellation, category layouts and subtle control surfaces.

Viewed PlayerCard/artifacts/ui-review/home-staged-entrance-frozen/warm-settled.png and settings/tray-qualified/normal-settings-art-state.png. They show the navy/Inter/ruled-row and compact control hierarchy; their example data was not imported. Root also inspected background-texture-glow-candidate reference captures. No UIZZE research is needed for a user-supplied explicit visual authority.

InterVariable.woff2 and OFL.txt are copied unmodified from PlayerCard/src/PlayerCard.Summary/wwwroot/fonts to frontend/fonts. License credits The Inter Project Authors. No other PlayerCard asset or application logic is copied.

## State and motion contract

Capture activity/readiness and saved-file success come from native snapshots/events. A configured replay length is never measured buffer coverage. Damaged sources disable Save/Start while retaining Stop for active capture; Repair OBS setup forces setup with current fields even when unchanged.

Settings uses Apply throughout; edits survive category switches and failures. Queue acceptance is not completion. Hotkey errors have a Change shortcut route to App and the affected Change control. Recording suspends native bindings and cancels on Escape, navigation, blur or40s timeout.

The app's saved Animations switch independently governs CSS and Web Animations.130ms control feedback,160ms switches/rail/details/dialog entry,110ms dialog exit. Prepared real element entry:220ms,4px,30ms stagger capped270ms, first visible at opacity.35. Initial presentation waits only bounded fonts and2animationframes before ui_ready; native reveal does not wait on OBS. Replay entry on navigation/tray reopen, never routine polling. Hide and trusted interaction cancel active effects to the final state.

## Verification packet

Isolated synthetic fixtures, explicitly labelled and kept out of shipping code, live under .impeccable/review/playercard. report.json records29/29 focused checks: setup route/staged startup/mic off/font load/one ui_ready, actual encoder label and unsupported-codec filtering, minimum-size overflow and fixed Apply, edits across category changes, full setup submission, active capture gating, shortcut-conflict recovery/focus, capture-source repair, motion-off cancellation, app motion independent of simulated Windows preference, failed Apply retention, keyboard window action and native-call serialization. Five reader-fade checks verify pointer transparency, overflow visibility, zero at the bottom, half-opacity at14px remaining and a boundary above the action footer. A coalesced frame responds only to scroll, observed size/content changes and explicit render/route updates; it has no idle frame loop.

Reviewed fixture-setup-1040.png, fixture-audio-1040.png, fixture-app-1040.png, fixture-capture-840.png. Further captures cover setup840, capture1040, empty Clips840 and shortcut error1040. These are layout/interaction fixtures, not native OBS evidence. Source syntax passes. No desktop mouse/keyboard input was sent.

Final verdict: PASS. A fresh non-forked reviewer compared PlayerCard references against actual embedded Capture, Setup at default/minimum sizes, Audio/App settings, Clips and conflict recovery. The named-dialog correction and fresh-install Ctrl+Shift+F10 default were verified. Root's actual WebView checks passed 20 assertions, including zero unchanged-poll DOM mutations and 125/150/200% emulated pixel scales. Native OBS replay saving and complete video decoding passed separately. Native hide/reopen, single instance and hidden startup passed without desktop input. See VALIDATION.md for exact scope and hardware limits; source extraction in DESIGN.md and sidecar remains distinct from runtime evidence.

Replay viewer extension: Ship. A fresh reviewer inspected the thumbnail rows, default/minimum-size video dialog, empty state, source changes and actual WebView2 validation report. No blocking feature findings were reported. Twenty-nine native feature assertions and four edge-case assertions passed, including H.264/HEVC/AV1 thumbnails and scrubbing, deletion during playback, persisted cleanup, keyboard focus and unreadable-video recovery. Evidence is under ignored `build/player-check`. This extends the incumbent tokens; no visual-system replacement or shipping raster was introduced.

Player actions and five-row list extension: Ship. A fresh reviewer inspected default/minimum modal controls, the named permanent-deletion confirmation with Cancel focused, five compact replay rows, source and native evidence. No blocking feature findings were reported. Twenty-seven checks passed, including native deletion and failure recovery, cancel/Escape preservation, exact Open/reveal dispatch paths, minimum-size layout, list ordering and replacement by the next older replay. Evidence is under ignored `build/player-actions-check`. Existing palette, type, motion and controls remain the design authority.

Compression and automatic Apply extension: the fresh reviewer's buffer-preservation finding is Resolved; no other blocking scoped findings were reported. Capture/audio Apply now explains saving and restarting OBS, and an existing profile can apply the compression upgrade without dirty preferences. Named quality controls keep the established appearance. Seven embedded UI checks and actual restart/recovery checks passed; the disconnected-microphone regression confirms that a device issue does not discard valid video history. See VALIDATION.md for measured file sizes and the limits of the comparison. No design tokens or shipping rasters changed.
