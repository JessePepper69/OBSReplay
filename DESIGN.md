---
name: OBSReplay
description: A compact OBS replay companion in the PlayerCard website's design language.
colors:
  page: "#090d13"
  canvas: "#10151d"
  card: "#121924"
  well: "#0c1118"
  surface: "#19222e"
  raise: "#1f2a38"
  text: "#f5f7fc"
  muted: "#c0ccdd"
  dim: "#8e9db2"
  line: "#2a3646"
  line-strong: "#344254"
  line-hover: "#4a5b72"
  green: "#70dfad"
  red: "#ee8293"
  warning: "#d3bd8b"
  ui-accent: "#0074d9"
  ui-accent-text: "#6bb6ff"
typography:
  title:
    fontFamily: "Inter, Segoe UI, sans-serif"
    fontSize: "28px"
    fontWeight: 750
    lineHeight: 1.15
    letterSpacing: "-0.03em"
  body:
    fontFamily: "Inter, Segoe UI, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: 1.5
  control:
    fontFamily: "Inter, Segoe UI, sans-serif"
    fontSize: "13px"
    fontWeight: 600
rounded:
  control: "9px"
  large-control: "10px"
  tile: "12px"
  card: "16px"
  dialog: "18px"
  hero: "20px"
spacing:
  gutter: "28px"
  compact-gutter: "22px"
  card: "20px"
components:
  button:
    backgroundColor: "#19222eb3"
    textColor: "{colors.text}"
    rounded: "{rounded.control}"
    padding: "0 14px"
    height: "36px"
  primary-button:
    backgroundColor: "{colors.ui-accent}"
    textColor: "#ffffff"
    rounded: "{rounded.control}"
    height: "36px"
  input:
    backgroundColor: "{colors.well}"
    textColor: "{colors.text}"
    rounded: "{rounded.control}"
    padding: "0 12px"
    height: "36px"
---

# Design System: OBSReplay

## Overview

The website in the user's PlayerCard project (`PlayerCard/site`, published as its artifact) is the visual authority, not the older PlayerCard desktop app. OBSReplay takes its tokens, components and motion from `site/base.css` and `site/settings.html`: a near-black page, cards one step up, wells one step down, flat buttons, the swept conic outline on the hero card and primary buttons, underline tabs, settings as rows inside a card, the 18px dialog over a blurred page, hover cards and the light toast. Generic palette or gradient bans from design skills do not override this reference.

This remains a task-focused OBS companion with a guided first run. Capture readiness and Save replay lead. OBS stays open to perform capture; OBSReplay supplies configuration, shortcuts and tray controls. The design does not import PlayerCard's XP, medals, reward colours, game artwork or activity statistics.

## Colors

Use the frontmatter tokens, which are the site's. `page` is the window ground (titlebar, rail, footer, content). `card` is every raised surface (hero, settings cards, setup panels, help cards). `well` is recessed: inputs, the hero's summary tile, task lists. `canvas` is dialogs, menus and popovers. Three text tiers: text; muted for a sentence that explains a state; dim (#8e9db2, at least 5.8:1 on every surface) for labels, hints and metadata. Structure uses `line`; a control edge uses `line-strong` and brightens to `line-hover`. Blue marks the primary action, the selected tab's underline and the current rail icon; green is capture readiness only; rose is destructive and error. Warnings and errors always carry text as well as colour.

No glow under buttons, no page gradients, no grain, no background animation, no cursor effects.

## Typography

Inter Variable is bundled locally at frontend/fonts/InterVariable.woff2 and preloaded. The unmodified SIL OFL license is beside it. The font and license were copied from the user's PlayerCard reference, which credits The Inter Project Authors. No remote font requests occur.

Sizes are 11/12/13/14/15/16/22/28px (16 and 20 also carry the editor's time and export figures); no half-pixel sizes. Page titles and the capture state are 28px/750 with -0.03em tracking, as the site sets its names and titles. Card titles are 16px/650, dialog and setup titles 22px/700. Controls are 13px/600, row titles 14px/650, descriptions 13px in dim. Durations, sizes and counts use tabular figures. Sentence case throughout.

## Layout

Default window: 1040×760 logical pixels; minimum 840×620. The 38px custom titlebar has native saucer drag, double-click maximize and window controls, and carries the brand tile (the site's blue `.mark`) with the replay glyph. Closing hides to tray; Help exposes explicit Quit.

The rail occupies 56px permanently and expands to 188px over the content the moment the pointer enters it or a rail item takes keyboard focus. It collapses as soon as the pointer leaves: a mouse click on a rail item never holds it open, because only `:focus-visible` counts as focus. Expansion never shifts the content. The current item is the site's settings-nav state: a surface fill with a 1px edge, white label, accent icon.

Content has a 28px gutter (22px at compact width) and a 1120px maximum, and every page uses the full width:

- **Capture** opens with the hero card, the site's identity card at app scale: the state in 28px with its sentence, then one row of actions with exactly one primary for the state: Save replay (with the shortcut inside the button) while capturing, Start capture when stopped (with Reconnect and Open OBS beside it when OBS is disconnected), Repair OBS setup when the capture source is broken; Stop capture and a settings icon button complete the row; below, a well tile with four cells (replay length, capture, video, microphone); below that, the clips folder, the capture-mode note and Open folder on one line. There is no separate page title. Under the hero, underline tabs (Latest replays, Edited exports, with View all at the right end) head a grid of up to eight clip cards.
- **Clips** is a title with Open folder, the same tabs, and the full grid.
- **Settings** is a title, Capture / Audio / App underline tabs, and one card per category whose rows have the label and hint on the left and the control on a shared right edge. Video encoding is a disclosure row inside the Capture card.
- **Guided setup** is a title, the three-step indicator, then full-width cards. Step 2 sets the three capture choices side by side as cards, then one rows card.
- **Help** is cards in two columns, with the reset action in a full-width card at the end.

Only the content reader scrolls; the titlebar, rail and action footer remain available. The Apply button stays reachable at minimum size. Controls in hidden categories retain their edits.

## Elevation & Depth

Depth is a tone step and a 1px line, not a shadow. Only things that float carry a shadow: the expanded rail, dialogs (over a dimmed page blurred 8px), menus, popovers, the toast and tooltips.

## Shapes

Two button sizes, as on the site: 40px with a 10px radius for a page's own actions (the hero, page headings, the footer, dialog and viewer action rows, Export video), 36px with a 9px radius inside rows and toolbars. Square icon buttons match the height of the row they sit in. Every button in a row has the same treatment: a bordered button never sits beside a bare text or bare icon button. Borderless text buttons are only for a lone link at the end of a bar (View all, All settings, Hide to tray); borderless icon buttons only where a whole group of them repeats (clip cards, transport). Tiles and thumbnails are 12px, cards 16px, dialogs 18px, the hero 20px. Switches are the site's 42×24px toggle, accent blue when on, with on/off text beside it. Selects use the site's chevron; disclosures a chevron at the right edge that turns over. No rounded primary pills.

## Components

- Guided setup is the first-run view (the Capture rail item routes to it until setup succeeds). A heading, then a three-step indicator (OBS Studio · Capture · Start) with numbered 24px marks: the current step is filled accent blue, finished steps show a green check, unreachable steps are disabled. Step 1 is one restrained panel (the capture-panel surface) with a status dot (gray missing, warning outdated, blue working, green ready, red failed), a title naming the state and one primary action: Install / Update OBS Studio (winget), or Download OBS Studio when winget is missing, plus Download and Locate obs64.exe as secondary. Installing replaces the actions with a three-row task list (Download, Install with the Windows-permission note, Check). Step 2 is the essentials only: a vertical radio list for what to record (Game or display, marked Recommended in accent text; Fullscreen games only; Entire display), then ruled rows for Display (hidden for games only), Replay length, Quality, Record microphone, Clips folder and Start with Windows, and one dim line naming the detected encoder and that other defaults are editable in Settings. These controls write straight into the Settings form, so both stay in sync. Step 3 shows a task list (Close OBS only when it was open, Create the OBSReplay profile, Open OBS in the tray, Start replay capture) whose rows go pending → spinning accent ring → green check, or a red cross at the failing stage with the error in an inline notice above. Success reads “Replay capture is running” with the save shortcut as a key cap and a note that a Windows network prompt for OBS can be answered either way. The footer carries Back and the forward action (Continue, Set up and start capture, Try again, Go to Capture). All settings leads to Settings, whose unconfigured action is Set up and start.
- Capture displays actual native readiness. Save is available only for an owned, connected, ready, active replay buffer. Source failures expose the exact issue and a Repair OBS setup route; stopping active capture remains possible.
- Clips and the Capture view's Latest section each have Replays and Edited exports tabs (the site's underline tabs, with one underline that slides to the selected tab) with quiet counts set on the label's baseline at the same size, arrow/Home/End keys and a remembered selection. Latest shows up to eight cards of the selected tab, newest first; Clips shows that tab's full recent history (up to 50 each); each tab has its own empty state. View all opens Clips on Latest's tab. Both use 16:9 video thumbnails with a quiet play affordance. The thumbnail/name opens the replay viewer: a dialog with the video, the editor's transport (time, frame step, play, volume, fullscreen) and the editor's timeline (ruler, filmstrip, waveform, playhead; wheel zoom, Shift+wheel scroll, drag to scrub) but none of its editing tools. Its action row has Delete on the left and Show in folder, Open file and the primary Edit clip on the right, all labelled and the same height. Delete pauses playback and requires a named permanent-deletion confirmation, with Cancel focused first; cancellation resumes previously playing video. Folder reveal pauses playback; opening externally closes the player. Escape or Close stops playback and restores focus. Missing files leave history automatically, including closing a deleted video's player. Thumbnails decode one visible clip at a time and keep a bounded in-memory cache.
- Clips are cards: a 16:9 thumbnail with the file size as a chip, the name, then the app and saved time as separate pieces. While the pointer or keyboard focus is on a card, the thumbnail lifts 3px, its outline lights and sweeps (the site's game-card edge: 300ms in, 140ms out), a white play disc appears, and one group of three icon buttons (Edit, Open in your video player, Show in folder; each with a tooltip and an accessible name) appears on the thumbnail's corner. A thumbnail still being decoded shows the site's skeleton sweep, then the frame fades in. The editor is its own 1200×820 window (minimum 900×640) with the same 38px titlebar, navy canvas and steel-blue controls. From top: a black preview stage; a transport row (time, “In export” position, previous cut / frame (hold to repeat) / Play (accent, 44px; double-click restarts from the first kept section) / frame / next cut, Preview cuts switch, one master volume (50% by default) and a Track volumes button opening a popover with a slider and mute per recorded track, labelled with the file's own track names); an edit toolbar (Split, Delete/Restore section with key hints, Before/After, undo/redo, Fit timeline only while zoomed, Shortcuts); a canvas timeline (ruler, filmstrip lane, waveform lane, white playhead, draggable cut handles, overview scrollbar only while zoomed); and a non-modal export bar (export length as a time-format menu button, a 10-step quality slider with plain-language labels from “Smallest · 720p” to “Original”, a “Click/Drag to select quality level” hint and a tooltip explaining Original versus smaller files, the estimated file size and codec, Export video). The titlebar has a compact Scale menu (75–150%) and a Preview quality menu (Full · original video, Half, Quarter, each listed with its height) left of the clip name; Ctrl+wheel never rescales the UI. Transient feedback appears as the site's light toast over the preview. Every tooltip in both windows uses one custom component (`tooltip.js`): the site's hover card (near-black, 10px radius, 1px edge, deep shadow, no arrow), 450 ms first-hover delay (60 ms when moving between controls), 180 ms in and 90 ms out, optional bold title and key caps; native title tooltips are never shown. Export-bar regions have fixed widths so changing quality or edits never shifts controls. A pill in the preview's corner shows progress while a Half or Quarter preview is prepared; the current picture stays until it is ready. Kept sections show the filmstrip in rounded blocks; removed sections are dimmed with a thin red-pink hatch and a “Removed” label; selection is a light-blue outline. A removed playhead dims the preview and shows a red “Removed section” flag. Export progress, success (with Show in folder / Open file) and failure replace the quality controls inline, and a missing FFmpeg shows a warning-colored notice with Get FFmpeg and Locate ffmpeg.exe while editing still works. Closing with unexported edits or a running export asks first, with Keep editing focused. Keyboard: Space/K, ←/→ (Shift ×5), , . frames, ↑/↓ cuts, S, Delete, I/O, Ctrl+Z/Y, Ctrl+A, Esc, +/−/0, Ctrl+E, ?, M.
- Settings has one explicit Apply model. Dirty, pending, applied and failed states remain distinct. Full settings values are submitted; failed edits are retained. Repair forces the existing setup operation even without dirty changes.
- Capture/audio Apply explains the automatic OBS restart and buffer preservation. Pending status follows save, close, configuration and reopen; success confirms capture resumed. App-only changes keep capture running. A pending compression upgrade enables Apply without dirty edits. Quality labels stay Very high, High, Balanced, Low and Very low; encoder parameters stay out of the control.
- Hotkey conflicts have a Change shortcut action leading to Settings → App and the affected Change button. Recording suspends native shortcuts, supports Escape/reset, and expires before the native guard.
- Switches use semantic checkbox inputs with switch roles and visible state text. Every field has a real label. Category tabs support arrow/Home/End keys; rail and window controls support keyboard access.
- Motion follows the PlayerCard site: 130ms control feedback (colour, edge, a 0.97 press scale); 150ms rail; 160–180ms switches and disclosures; a 240ms sliding tab underline; blocks rise in on presentation, navigation and tab changes (260ms, 6px, 40ms apart, at most four steps); 260ms dialog entry (10px rise from 0.97 scale) and 150ms exit; menus and popovers 160ms. The swept outline is the signature, one per surface: on enabled primary buttons (4.8s a turn), on the capture hero while the replay buffer is recording (7s; a static 1px edge otherwise, and the Save button inside a live hero holds still), and on a clip thumbnail while it is hovered or focused. Play buttons are filled accent without an outline. The live dot pulses while recording. Nothing else moves on its own except spinners that mark work in progress. Entrances occur on initial presentation, explicit navigation and tray reopening; routine polling does not replay them.
- Until the first state arrives, the four capture summary values are skeleton bars at their final size rather than dashes.
- The persisted Animations setting controls app motion independently of Windows, matching the reference's explicit policy. Turning it off through Apply cancels effects and leaves the final static state. Hiding or trusted interaction cancels running entry effects.
- Initial presentation waits only for bounded font readiness and two animation frames, then acknowledges ui_ready. Prepared real elements remain visible at partial opacity until the native reveal and progress together; no fake placeholders or delays wait for OBS startup.

## Do's and Don'ts

- Do take patterns from the PlayerCard site's own stylesheet before inventing one, and keep one button height and one radius per row.
- Do keep replay readiness, Save, shortcut recovery and Apply discoverable.
- Do give a rail click back to the page: a mouse click on a rail item moves focus to the content, so the rail closes when the pointer leaves and keys scroll the page.
- Do distinguish actual buffer activity from configured replay duration and actual game capture from foreground app naming.
- Do keep microphone off by default and whole-display capture explicit.
- Do preserve dirty settings across navigation and failures.
- Do verify in isolated/headless contexts; do not move the user's mouse or send desktop keyboard input.
- Don't import XP, gold rewards, artwork, invented metrics, cursor effects or PlayerCard's application logic.
- Don't claim fixture screenshots prove native recording or that bridge queue acceptance confirms a save.
- Don't load runtime assets from the network or hide errors behind generic success messages.
