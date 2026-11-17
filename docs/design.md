# Design system

This file is the visual and interaction reference for OBSReplay: tokens, themes, layout, components, motion and rules.

## Contents

- [Purpose and reference](#purpose-and-reference)
- [Colour tokens](#colour-tokens)
- [Themes](#themes)
- [Typography](#typography)
- [Radii, spacing and sizes](#radii-spacing-and-sizes)
- [Elevation](#elevation)
- [Layout](#layout)
- [Components](#components)
- [Motion](#motion)
- [States and behaviours](#states-and-behaviours)
- [Do](#do)
- [Don't](#dont)

## Purpose and reference

- OBSReplay is a compact, task-focused OBS replay companion with a guided first run.
- The PlayerCard website (`PlayerCard/site`, published as its artifact) is the visual authority.
- The older PlayerCard desktop app is not a reference.
- Tokens, components and motion come from the site's `site/base.css` and `site/settings.html`.
- Patterns taken from the site: a near-black page, cards one step up, wells one step down, flat buttons, the swept conic outline on the hero card and primary buttons, underline tabs, settings as rows inside a card, the 18px dialog over a blurred page, hover cards, the light toast.
- Generic palette or gradient bans from external style guides do not override this reference.
- Capture readiness and **Save replay** lead the interface.
- OBS stays open to perform capture. OBSReplay supplies configuration, shortcuts and tray controls.
- The design does not import PlayerCard's XP, medals, reward colours, game artwork or activity statistics.

Source files:

| File | Holds |
|---|---|
| [`../frontend/app.css`](../frontend/app.css) | Token declarations on `:root` (the **Original** theme), main-window components |
| [`../frontend/themes.css`](../frontend/themes.css) | Token overrides per `data-theme` |
| [`../frontend/theme.js`](../frontend/theme.js) | Theme list, theme switching, timeline canvas colours |
| [`../frontend/tooltip.js`](../frontend/tooltip.js) | The one tooltip component, used by both windows |
| [`../frontend/viewer.css`](../frontend/viewer.css), [`../frontend/editor.css`](../frontend/editor.css) | Replay viewer and clip editor styles |

## Colour tokens

Values are the **Original** theme. Every theme overrides the same names.

### Surfaces

| Token | Value | Role |
|---|---|---|
| `--page` | `#090d13` | Window ground: titlebar, rail, footer, content |
| `--card` | `#121924` | Every raised surface: hero, settings cards, setup panels, help cards |
| `--well` | `#0c1118` | Recessed surfaces: text inputs, the hero summary tile, task lists |
| `--canvas` | `#10151d` | Dialogs, menus, popovers, selects |
| `--surface` | `#19222e` | Button hover fill, current rail item, thumbnail placeholder |
| `--raise` | `#1f2a38` | Pressed filter chip, skeleton highlight, timeline flag |
| `--tip` | `#0b1017` | Tooltip background |
| `--toast` | `#f5f7fc` | Toast background |

### Text

| Token | Value | Role |
|---|---|---|
| `--text` | `#f5f7fc` | Primary text |
| `--muted` | `#c0ccdd` | A sentence that explains a state |
| `--dim` | `#8e9db2` | Labels, hints, metadata. At least 5.8:1 on every **Original** surface |
| `--tip-text` | `#f5f7fc` | Tooltip text |
| `--toast-text` | `#0b1017` | Toast text |
| `--on-accent` | `#ffffff` | Text on a primary button |
| `--on-selection` | `#ffffff` | Selected text |

### Lines

| Token | Value | Role |
|---|---|---|
| `--line` | `#2a3646` | Structure: card edges, row dividers |
| `--line-strong` | `#344254` | Control edges |
| `--line-hover` | `#4a5b72` | Control edge on hover |

### Accent and semantic

| Token | Value | Role |
|---|---|---|
| `--ui-accent` | `#0074d9` | Primary action fill, switch on |
| `--accent-hover` | `#1683e6` | Primary action fill on hover |
| `--ui-accent-text` | `#6bb6ff` | Selected tab underline, current rail icon, focus ring (`--focus`), accent text |
| `--accent-bright` | `#f2fbff` | Brightest point of the swept outline |
| `--green` | `#70dfad` | Capture readiness only |
| `--red` | `#ee8293` | Destructive and error text, edges |
| `--warning` | `#d3bd8b` | Warnings |
| `--danger` | `#c9455b` | Destructive button fill in dialogs, close-button hover |
| `--danger-hover` | `#d4536a` | Destructive button fill on hover |

### Controls and utility

| Token | Value | Role |
|---|---|---|
| `--switch-off` | `#2c394a` | Switch track when off |
| `--switch-off-hover` | `#364558` | Switch track when off, on hover |
| `--thumb` | `#f5f7fc` | Switch thumb |
| `--selection` | `#2f5f94` | Text selection background |
| `--shadow` | `#000000` | Shadow colour for floating elements |
| `--scrim` | `#04070b` | Dialog backdrop, mixed at 60% |
| `--tint` | `#ffffff` | Faint overlays such as key caps, mixed at 6% |
| `--gray` | `var(--dim)` | Idle state dot |
| `--hover` | `--line-hover` at 22% | Hover wash |

### Timeline

| Token | Value | Role |
|---|---|---|
| `--wave` | `#5f93c8` | Waveform |
| `--wave-bg` | `#122236` | Waveform lane |
| `--wave-selected` | `#a9d3ff` | Waveform in a selected section |
| `--wave-selected-bg` | `#16304b` | Waveform lane in a selected section |
| `--lane-edge` | `#3c5674` | Lane block edge |
| `--playhead` | `#ffffff` | Playhead |

### Over-video tokens

These tokens are fixed dark values in every theme. Use them for anything drawn over video: thumbnail chips, the clip action group, the play disc, stage messages. The video stage stays near-black in every theme.

| Token | Value |
|---|---|
| `--ov-bg` | `#0b1017eb` |
| `--ov-chip` | `#090d13d1` |
| `--ov-veil` | `#090d1366` |
| `--ov-text` | `#f5f7fc` |
| `--ov-muted` | `#c0ccdd` |
| `--ov-ink` | `#0b1017` |
| `--ov-line` | `#344254` |
| `--ov-hover` | `#19222e` |
| `--ov-red` | `#ee8293` |
| `--ov-red-bg` | `#1c1218f2` |

### Colour rules

- Declare every colour as a token on `:root` in `app.css`. No rule elsewhere names a colour.
- Write translucent variants as `color-mix(in srgb, var(--token) N%, transparent)` so they follow the theme.
- Use `--line` for structure.
- Use `--line-strong` for control edges. Brighten to `--line-hover` on hover.
- Use blue for the primary action, the selected tab's underline and the current rail icon.
- Use green for capture readiness only.
- Use rose for destructive actions and errors.
- Pair every warning and error colour with text.
- No glow under buttons.
- No page gradients, grain, background animation or cursor effects.

## Themes

Six themes exist. **Original** is the default and matches the token tables above. The other five change colour only. Layout, components and motion are identical in every theme.

| Theme | `data-theme` | Scheme | `--page` | `--ui-accent` | `--ui-accent-text` | Character |
|---|---|---|---|---|---|---|
| **Original** | (attribute absent) | dark | `#090d13` | `#0074d9` | `#6bb6ff` | The PlayerCard site palette |
| **Mono** | `mono` | dark | `#0a0a0a` | `#ededed` | `#ffffff` | Zero-chroma greys. White primary with dark text (`--on-accent` `#0a0a0a`). The semantic colours are its only hue |
| **Light** | `light` | light | `#f2f4f7` | `#0068c6` | `#0061ba` | White cards on a pale ground, blue accent, dark inverse tooltips and toast |
| **Rose** | `rose` | dark | `#100b0d` | `#bf2d74` | `#f48eb6` | Neutrals tinted toward the hue |
| **Amethyst** | `amethyst` | dark | `#0d0c11` | `#7d49da` | `#b6a1f5` | Neutrals tinted toward the hue |
| **Ember** | `ember` | dark | `#0f0c0a` | `#b14a07` | `#fb9363` | Neutrals tinted toward the hue |

### How themes work

- `app.css` declares the **Original** tokens on `:root`.
- `themes.css` overrides the same tokens under `:root[data-theme="<id>"]`.
- `theme.js` sets `data-theme` and `data-scheme` (`dark` or `light`) on `<html>`. **Original** removes `data-theme`.
- `theme.js` runs in the `<head>` of both pages without `defer`, so the saved theme applies before the first paint.
- `localStorage` key `obsreplay.theme` holds a copy of the saved theme. Editor windows follow a change made in the main window through the `storage` event.
- `theme.js` updates `<meta name="theme-color">` and calls the native `theme_changed` so the native window ground matches.
- `Theme.timeline()` in `theme.js` supplies the timeline canvas colours, because a canvas cannot read CSS variables.
- A theme change cross-fades the whole window over 240ms. The first paint and the Animations-off state do not cross-fade.
- **Rose**, **Amethyst** and **Ember** keep the lightness steps of **Original**.
- Light schemes use softer floating shadows (`:root[data-scheme="light"]` rules in `themes.css`).
- Each theme overrides the select chevron image so its stroke matches that theme's `--dim`.
- **Mono** steps the filled quality-slider segments down to `--dim` so they differ from the current step.

### Semantic colours per theme

Semantic colours stay clear of the accent in every theme.

| Theme | Adjustment |
|---|---|
| **Rose** | `--red` is coral (`#ff916b`) |
| **Ember** | `--red` is pink (`#f47fae`) |

### Rules for a new theme

A new theme must pass these contrast floors on every surface.

| Pair | Minimum contrast |
|---|---|
| `--text` on surface | 12:1 |
| `--muted` on surface | 7:1 |
| `--dim` on surface | 4.5:1 |
| `--on-accent` on `--ui-accent` | 4.5:1 |
| Control edge on surface | 1.35:1 |

- Override tokens only. Do not add layout, component or motion rules per theme.
- Leave the `--ov-*` tokens unchanged.
- Add the theme to the `THEMES` list in `theme.js` with `id`, `name`, `scheme` and a three-colour `swatch`.

### Theme setting

- The theme is a setting under Settings, **App**: a grid of swatch buttons, four columns.
- Choosing a swatch previews the theme at once in that window.
- **Apply** saves the theme for every window.
- **Discard** returns to the saved theme.

## Typography

### Font

- Inter Variable is bundled at `frontend/fonts/InterVariable.woff2` and preloaded.
- The unmodified SIL OFL licence sits beside the font file.
- The font and licence come from the PlayerCard reference, which credits The Inter Project Authors.
- The app makes no remote font requests.
- Fallback stack: `"Inter", "Segoe UI", system-ui, sans-serif`.

### Type scale

| Role | Size | Weight | Other |
|---|---|---|---|
| Page title, capture state | 28px | 750 | Line height 1.15, tracking -0.03em |
| Dialog title, setup title | 22px | 700 | Line height 1.2, tracking -0.02em |
| Clips empty-state title | 18px | 650 | |
| Card title | 16px | 650 | Line height 1.35, tracking -0.01em |
| Hero summary value | 16px | 650 | Tabular figures |
| Row title | 14px | 650 | Line height 1.4 |
| Body | 14px | 400 | Line height 1.5 |
| Control | 13px | 600 | 14px on 40px buttons |
| Description, hint | 13px | 400 | `--dim` |
| Label, chip, tab count | 12px to 13px | 500 to 600 | |
| Key cap | 11px | 600 | Tabular figures |

- Allowed sizes: 11, 12, 13, 14, 15, 16, 18, 22, 28px. The editor's time and export figures use 16px and 20px.
- No half-pixel sizes.
- Durations, sizes and counts use tabular figures.
- Use sentence case everywhere.
- Below 700px width the page title drops to 24px.

## Radii, spacing and sizes

### Radii

| Name | Value | Used by |
|---|---|---|
| control | 9px (`--r-control`) | 36px buttons, inputs, selects |
| large control | 10px | 40px buttons |
| tile | 12px | Tiles, thumbnails, tooltips, menus |
| panel | 14px | Inline notices, capture-choice cards, track popover |
| card | 16px (`--r-card`) | Cards |
| dialog | 18px | Dialogs |
| hero | 20px | Capture hero, setup panels |

No rounded primary pills.

### Spacing

| Name | Value | Used by |
|---|---|---|
| gutter | 28px (`--gutter`) | Content side padding |
| compact gutter | 22px | Content side padding at 900px width and below |
| narrow gutter | 17px | Content side padding at 700px width and below |
| card padding | 20px | Cards, setting rows (horizontal) |
| content maximum | 1120px | Every page section |

### Button sizes

| Size | Height | Radius | Padding | Font | Use |
|---|---|---|---|---|---|
| Page action | 40px | 10px | 0 16px | 14px/600 | Hero, page headings, footer, dialog action rows, viewer action row, **Export video** |
| Row action | 36px | 9px | 0 14px | 13px/600 | Inside rows and toolbars |

| Variant | Background | Text | Edge |
|---|---|---|---|
| Default | `--surface` at 70% (`#19222eb3`) | `--text` | 1px `--line-strong` |
| Primary | `--ui-accent` | `--on-accent` | None, plus the swept outline |
| Danger | Transparent | `--red` | `--red` at 50% |
| Danger in dialog | `--danger` | `#fff` | None |
| Text button | Transparent | `--dim` | None |
| Icon only | Transparent | `--dim` | None, 32px square |

### Button rules

- Square icon buttons match the height of the row they sit in.
- Give every button in a row the same treatment. A bordered button never sits beside a bare text or bare icon button.
- Use borderless text buttons only for a lone link at the end of a bar: **View all**, **All settings**, **Hide to tray**.
- Use borderless icon buttons only where a whole group of them repeats: clip cards, transport.
- Pressed buttons scale to 0.97.
- Disabled buttons drop to 45% opacity. A disabled primary button turns into a flat `--surface` button with `--dim` text.

### Inputs

| Property | Value |
|---|---|
| Height | 36px |
| Radius | 9px |
| Padding | 0 12px |
| Background | `--well` (text inputs), `--canvas` (selects) |
| Text | `--text`, 13px |
| Edge | 1px `--line-strong`, `--line-hover` on hover |
| Focus | `--focus` edge plus a 3px ring of `--ui-accent-text` at 22% |

## Elevation

- Depth is a tone step and a 1px line.
- Only floating elements carry a shadow: the expanded rail, dialogs, menus, popovers, the toast, tooltips, the app-wide error card.
- Dialogs sit over a dimmed page blurred 8px.

## Layout

### Window

- Default size: 1040×760 logical pixels.
- Minimum size: 840×620.
- Native sizing fits and centres the window within its monitor work area.
- Closing the window hides it to the tray.
- Help exposes an explicit **Quit**.

### Titlebar

- Height: 38px, custom drawn.
- Native saucer drag, double-click maximize and window controls.
- Carries the logo at 22px: a faceted hexagon gem with a replay arrow. It is inline SVG. The gem takes the theme accent (`--ui-accent`, lightened at the top, darkened at the bottom and under-edge) and the arrow takes `--on-accent`, so it matches every theme. The exe, taskbar and tray icon stay blue.
- Shows the connection status at the right, before the window controls.

### Rail

- Width: 56px, permanent.
- Expands to 188px over the content when the pointer enters it or a rail item takes keyboard focus.
- Collapses as soon as the pointer leaves.
- A mouse click on a rail item never holds the rail open, because only `:focus-visible` counts as focus.
- A mouse click on a rail item moves focus to the content, so keys scroll the page.
- Expansion never shifts the content.
- Current item: a `--surface` fill with a 1px edge, white label, accent icon (the site's settings-nav state).

### Content area

- Gutter: 28px, 22px at compact width.
- Maximum width: 1120px. Every page uses the full width.
- Only the content reader scrolls. The titlebar, rail and action footer stay in place.
- The footer is at least 54px high and holds the status text and the page actions.
- **Apply** stays reachable at minimum window size.
- Controls in hidden settings categories retain their edits.

### Capture page

- No separate page title.
- Opens with the hero card.
- Above the hero, only when the clips drive has 5% or less free: an amber notice. Title **Clips drive is almost full**. Text: "Drive C: has 4% free (20.9 GB). Free up space so new replays can save." Buttons **Open storage settings** (Windows Settings, System, Storage) and **Dismiss**. Dismiss hides it until a new replay is saved and the window opens again. It leaves on its own once space is freed.
- Under the hero: underline tabs **Latest replays** and **Latest edits**, with **View all** at the right end. Each shows 8 clips, so the tabs carry no totals. The Clips page tabs do.
- Under the tabs: a grid of up to eight clip cards.

### Clips page

- Title row with **Open folder**.
- The same two tabs as Capture.
- Toolbar: search field, then game chips, with the pager on the right.
- Grid: 24 clips a page, columns at least 204px wide.
- The pager repeats under the grid.

### Settings page

- Title, then underline tabs **Capture**, **Audio**, **App**.
- One card per category.
- Each row has the label and hint on the left and the control on a shared right edge.
- Video encoding is a disclosure row inside the Capture card.

### Guided setup page

- Title, then the three-step indicator, then full-width cards.
- Step 2 sets the three capture choices side by side as cards, then one rows card.

### Help page

- Cards in two columns.
- The reset action sits in a full-width card at the end.

## Components

### Guided setup

- Guided setup is the first-run view. The Capture rail item routes to it until setup succeeds.
- Structure: a heading, then a three-step indicator: Install, Capture, Start.
- Step marks are numbered 24px circles.
- Current step: filled accent blue.
- Finished steps: a green check.
- Unreachable steps: disabled.
- The footer carries **Back** and the forward action: **Continue**, **Set up and start capture** or **Try again**.
- A new user with nothing installed reaches a running capture in two clicks: the install button, then **Set up and start capture**.
- **All settings** leads to Settings, whose unconfigured action is **Set up and start**.

Step 1, Install:

- One restrained panel on the capture-panel surface.
- A status dot: grey missing, warning outdated or FFmpeg missing, blue working, green ready, red failed.
- A title naming the state.
- A two-row list in a well: **OBS Studio** and **FFmpeg**. Each row has a mark (ring pending, spinning accent ring working, green check ready, red cross failed) and one dim line of detail: version, stage, or the reason.
- Setup checks both when the step opens. It opens on step 2 only when both are present.
- One primary action installs what is missing through winget: **Install OBS Studio and FFmpeg**, **Install OBS Studio**, **Update OBS Studio**, or **Install FFmpeg**. OBS Studio installs first, then FFmpeg, so two winget runs never overlap.
- When winget is missing the primary action is **Download OBS Studio**.
- Secondary actions while OBS Studio is missing: **Download from obsproject.com** and **Locate obs64.exe**.
- Secondary actions once OBS Studio is ready and FFmpeg is missing: **Download from ffmpeg.org** and **Locate ffmpeg.exe**. They also show when winget is missing.
- A failed OBS Studio install still starts the FFmpeg install.
- OBS Studio installed by hand while step 1 waits: setup moves to step 2 on its own when FFmpeg is present.
- Installing hides the actions. The rows show the progress. On step 2 the footer reports an FFmpeg install that still runs or has failed.
- OBS Studio is required. FFmpeg is not: **Continue** is available as soon as OBS Studio is ready, and an FFmpeg install finishes in the background.
- A failed FFmpeg install shows the reason in the panel and keeps **Install FFmpeg** and **Continue**.

Step 2, Capture:

- Essentials only.
- Three capture-choice cards (radio inputs) side by side: **Game or display (Hybrid)**, **Fullscreen games only**, **Entire display**.
- **Game or display (Hybrid)** carries a Recommended tag in accent text.
- A rows card follows: Display, Replay length, Quality, Record microphone, Clips folder, Start with Windows.
- The Display row is hidden for **Fullscreen games only**.
- One dim line names the detected encoder and states that other defaults are editable in Settings.
- These controls write straight into the Settings form, so both stay in sync.

Step 3, Start:

- A task list: Close OBS (only when OBS was open), Create the OBSReplay profile, Open OBS in the tray, Start replay capture.
- Row states: pending, then a spinning accent ring, then a green check.
- A failing stage shows a red cross. The error appears in an inline notice above the list.
- While setup runs, a note says that a Windows network prompt for OBS can be answered either way.
- Success reads "Replay capture is running" with the save shortcut as a key cap, holds for about a second, then opens the Capture page. No click is needed.
- The empty replay list on the Capture page names the save shortcut while capture runs.

### Capture hero

- The hero is the site's identity card at app scale: 20px radius, `--card` fill.
- Top: the state in 28px with its sentence.
- One row of actions with exactly one primary for the state.
- While capturing, the primary is **Save replay**, with the shortcut inside the button.
- When stopped, the primary is **Start capture**.
- When OBS is disconnected, **Reconnect** and **Open OBS** sit beside **Start capture**.
- When the capture source is broken, the primary is **Repair OBS setup**.
- **Stop capture** and a settings icon button complete the row.
- Below the actions: a well tile with four cells: replay length, capture, video, microphone.
- Below the tile: the clips folder and **Open folder** on one line.
- The hero displays actual native readiness.
- **Save replay** is available only for an owned, connected, ready, active replay buffer.
- Source failures expose the exact issue and a **Repair OBS setup** route.
- Stopping active capture stays possible during a source failure.
- Until the first state arrives, the four summary values are skeleton bars at their final size.
- The hero edge is a static 1px line unless the replay buffer is recording.
- While the replay buffer records, the hero edge sweeps and the live dot pulses.

### Clips tabs

- Clips and the Capture page's Latest section each have **Replays** and **Edited exports** tabs.
- Tabs are the site's underline tabs. One underline slides to the selected tab.
- Counts sit on the label's baseline, in a quiet colour.
- Arrow, Home and End keys move between tabs.
- The app remembers the selected tab.
- Latest shows up to eight cards of the selected tab, newest first.
- Clips shows every clip of that tab, 24 per page.
- Each tab has its own empty state.
- **View all** opens Clips on the tab selected in Latest.

### Clips toolbar and pager

- The toolbar shows whenever the selected tab has clips.
- Search field: 32px high and 132px wide at rest; it widens to 270px while it has the keyboard or holds text. A search icon inside its left edge, placeholder "Search clips", a clear button once it has text.
- Search matches every typed word against the file name, the game and the saved date. The date matches as the card shows it, or as weekday, month name, year, local numeric date or `2026-11-15`. It filters as you type and keeps the field focused.
- Ctrl+F on the Clips page focuses the field. Escape clears it.
- While a search runs, chips keep their order and show how many matches each game has. A game with none is disabled.
- No matches: the title "No replays match “text”", one line saying what search looks at, and **Clear search**.
- One chip per game: **All** first, then games by clip count.
- Chips appear only when there are two or more games.
- Game names drop build suffixes from the executable name. "FortniteClient-Win64-Shipping" reads "Fortnite".
- The pager shows previous, a range such as "1–24 of 31", next.
- The pager appears only when there is more than one page.
- A date menu sits after the search field: **Any time**, **Today**, **Last 7 days**, **Last 30 days**, **This year**. It is the dropdown component at 32px.
- A date choice that leaves nothing shows "No replays from the last 7 days" and **Show any time**.

### Day headings

- On Clips, cards sit under day headings that span the grid: **Today**, **Yesterday**, then the date ("Friday, November 13").
- A heading is a divider: a 4px marker, the day at 16px/700, a 1px `--line-strong` rule to the far edge, then the count ("3 clips") in the quiet colour. Today's marker is the accent colour; other days use `--line-hover`.
- A date from another year shows its year.
- A short age follows the date in the quiet colour: "3d ago", "2w ago", "1m ago", "1y ago". Today and Yesterday have none.
- Under a heading a card shows the time alone. Latest on Capture has no headings and shows date and time.

### Selecting clips

- A 24px check box sits on the thumbnail's top left corner. It appears with the pointer or focus, like the "more" button.
- Shift+click or Ctrl+click on a card selects it, and the same click on a selected card deselects it. A plain click plays the clip and keeps the selection.
- While anything is selected, every check box stays visible. A selected card keeps its colour, gets a filled check box and grows to 104% over 240ms. It has no edge of its own. Every other card turns fully grey, picture, name and time alike, at half strength; pointing at one brings it to 80%.
- A bar floats over the foot of the page on the dropdown surface: "3 selected", **Select all 31**, **Copy**, **Delete** (danger), **Clear selection**. Every button is labelled and the same height.
- **Delete** asks first: "Delete 3 clips?".
- **Clear selection** or Escape ends select mode. Changing page, filter, tab or opening a clip does not.
- Latest on Capture has no check boxes.
- A card dragged 8px starts a file drag out of the window. A selected card carries the whole selection.

### Clip cards

- A card is a 16:9 thumbnail with one line under it.
- Thumbnail radius: 12px.
- The file size is a chip at the bottom left of the thumbnail.
- The clip length ("2m 15s") is a chip at the bottom right, shown once the thumbnail is decoded.
- The line under the thumbnail has the game at the left edge and the saved time at the right edge.
- The file name is hidden at rest.
- With the pointer or keyboard focus on the card, the file name fades in on a second line, in the gap between rows. Nothing moves.
- A clip name has no tooltip.
- While the pointer or keyboard focus is on a card, the thumbnail lifts 3px.
- The thumbnail outline lights and sweeps (the site's game-card edge).
- A white play disc appears over the thumbnail.
- One "more" button (three dots) appears on the thumbnail's top right corner. It opens the clip menu under itself.
- A right-click anywhere on a card opens the same menu at the pointer.
- Clip menu rows, each an icon and a label: **Play**, **Edit clip**, **Rename**, a rule, **Open in video player**, **Show in folder**, a rule, **Delete** in red.
- The menu uses the dropdown list's surface: `--surface`, 12px radius, 1px `--line-strong` edge. It flips to stay inside the window.
- The card keeps its hover look while its menu is open.
- Menu keys: arrows, Home and End move; Enter chooses; Escape or Tab closes and returns focus to the button. The row under the pointer takes focus, so there is one highlight.
- A press outside, scrolling, resizing or the window losing focus closes the menu.
- **Delete** asks first. Holding Shift while choosing it skips the question.
- **Rename** opens a dialog with the name in a field and the extension after it. Characters Windows does not allow are dropped as typed. **Rename** stays disabled until the name changes. A name already used in that folder is refused with a message.
- Focus handed back by a closed viewer does not keep the hover state showing.
- A thumbnail still being decoded shows the site's skeleton sweep. The frame then fades in.
- Thumbnails decode one visible clip at a time.
- Thumbnails keep a bounded in-memory cache and a copy on disk, so a reopened app shows them at once.
- Clicking the thumbnail or name opens the replay viewer.

### Replay viewer

- The viewer is a dialog.
- It contains the video, the editor's transport and the editor's timeline.
- Transport: time, frame step, play, volume, fullscreen.
- Timeline: ruler, filmstrip, waveform, playhead.
- Timeline input: wheel zooms, Shift+wheel scrolls, drag scrubs.
- The viewer has none of the editor's editing tools.
- Action row: **Delete** on the left; **Show in folder**, **Open file** and the primary **Edit clip** on the right.
- All action-row buttons are labelled and the same height.
- **Delete** pauses playback and requires a named permanent-deletion confirmation.
- The confirmation focuses **Cancel** first.
- Cancelling the confirmation resumes video that was playing.
- **Show in folder** pauses playback.
- **Open file** closes the viewer.
- The header moves the window when dragged, and maximizes on a double click, like the title bar the viewer covers.
- Escape or **Close** stops playback and restores focus.
- Missing files leave history without user action. A deleted video's open viewer closes.
- The frame-step and Play buttons have no tooltips. Their accessible names remain.

### Clip editor window

- The editor is its own window: 1200×820 default, 900×640 minimum.
- It has the same 38px titlebar, canvas colour and control styling as the main window.
- Titlebar: a compact Scale menu (75%, 100%, 125%, 150%) and a Preview quality menu sit left of the clip name.
- Preview quality options: Full (original video), Half, Quarter. Each option lists its height.
- Ctrl+wheel never rescales the UI.
- Transient feedback appears as the site's light toast over the preview.
- Closing with unexported edits or a running export asks first, with **Keep editing** focused.

Layout from top to bottom:

1. A black preview stage.
2. A transport row.
3. An edit toolbar.
4. A canvas timeline.
5. A non-modal export bar.

### Clip editor transport

- Shows the time and the "In export" position.
- Buttons in order: previous cut, frame back, Play, frame forward, next cut.
- Frame buttons repeat while held.
- Play is filled accent, 44px, without an outline.
- Shift+click on Play restarts from the first kept section. A double click does nothing extra, so quick play and pause taps never jump back.
- A Preview cuts switch follows the buttons.
- One master volume slider, 50% by default.
- An Audio tracks button opens a popover with a check box, a slider and mute per recorded track. A ticked track goes into the export.
- Track rows use the file's own track names.
- The frame-step and Play buttons have no tooltips. Their accessible names remain.

### Clip editor toolbar

- **Split**.
- **Delete section** or **Restore section**, with key hints.
- **Before** and **After** trims.
- Undo and redo.
- **Fit timeline**, shown only while zoomed.
- **Shortcuts**.

### Clip editor timeline

- Drawn on a canvas with colours from `Theme.timeline()`.
- Lanes: ruler, filmstrip, waveform.
- A white playhead (`--playhead`).
- Draggable cut handles.
- An overview scrollbar, shown only while zoomed.
- Kept sections show the filmstrip in rounded blocks.
- Removed sections are dimmed with a thin red-pink hatch and a "Removed" label.
- Selection is a light-blue outline.
- A playhead inside a removed section dims the preview and shows a red "Removed section" flag.
- A pill in the preview's corner shows progress while a Half or Quarter preview is prepared. The current picture stays until the preview is ready.

### Clip editor export bar

- Export length is a menu button that selects the time format.
- A 10-step quality slider with plain-language labels from "Smallest · 720p" to "Original".
- A **Max size** row under the slider: five chips, 10, 25, 50, 100 and 500, then "MB". A chosen chip dims the slider, and the label reads "Fit to 25 MB", "Already fits" or "Too long". The estimate reads "Up to 25 MB". Moving the slider or pressing the chip again ends the choice. A size the edit cannot meet is dimmed and explains why in a toast.
- A tooltip explains Original versus smaller files.
- The estimated file size and codec.
- **Export video**, 40px.
- **Export video** first asks for a name in a 560px dialog. A checkbox, **Delete original clip after exporting**, sits at the bottom left in line with **Cancel**. It starts unticked every time.
- With the box ticked, the original is deleted once the export is saved. A failed or canceled export keeps it. The result line adds "Original deleted", and the editor then asks to be closed.
- Export-bar regions have fixed widths, so changing quality or edits never shifts controls.
- Export progress, success and failure replace the quality controls inline.
- Success offers **Show in folder** and **Play video**. Play video opens the export in the main window's viewer. File Explorer does not open on its own.
- A missing FFmpeg shows a warning-coloured notice with **Install FFmpeg** (primary, shown when winget exists), **Get FFmpeg** and **Locate ffmpeg.exe**. Editing still works.
- **Install FFmpeg** installs through winget. The notice then reads "Installing FFmpeg…" with a spinner and one line per stage, and hides its buttons. A failure replaces the text with the reason and brings the buttons back. On success the notice gives way to the export controls.

### Clip editor keyboard

| Key | Action |
|---|---|
| Space, K | Play or pause |
| ←, → | Step (Shift ×5) |
| `,` `.` | Frame back, frame forward |
| ↑, ↓ | Previous cut, next cut |
| S | Split |
| Delete | Delete section |
| I, O | Trim before, trim after |
| Ctrl+Z, Ctrl+Y | Undo, redo |
| Ctrl+A | Select all |
| Esc | Clear selection or close a menu |
| `+` `−` `0` | Zoom in, zoom out, fit |
| Ctrl+E | Export |
| `?` | Shortcuts |
| M | Mute |

### Export name dialog

- **Export video** first opens a small dialog with one field for the file name.
- The placeholder is the default name.
- An empty field keeps the default name.
- The field removes characters Windows forbids as the user types.
- A name already taken gets " (2)" appended.

### Settings and the Apply model

- Settings has one explicit **Apply** model.
- Dirty, pending, applied and failed states remain distinct.
- **Apply** submits full settings values.
- Failed edits are retained.
- Repair forces the existing setup operation even without dirty changes.
- Capture and audio **Apply** explains the automatic OBS restart and that the replay buffer starts again from empty.
- Pending status follows these stages: pause capture, close, configuration, reopen.
- Success confirms that capture resumed.
- App-only changes keep capture running.
- A profile update from a new app version shows nothing here. It is applied at launch while OBS is closed.
- Quality labels are Very high, High, Balanced, Low and Very low.
- Encoder parameters stay out of the quality control.
- Setting row: minimum height 68px, padding 13px 20px, 1px `--line` divider.

### Hotkeys

- A hotkey conflict shows a **Change shortcut** action.
- **Change shortcut** leads to Settings, **App**, and the affected **Change** button.
- Recording a shortcut suspends native shortcuts.
- Recording supports Escape and reset.
- Recording expires before the native guard.
- The default save shortcut is Alt+Z.

### Switches

- Switches are the site's 42×24px toggle with an 18px thumb.
- On state: accent blue.
- On/off text sits beside the switch.
- Each switch is a semantic checkbox input with a switch role and visible state text.
- Every field has a real label.

### Tabs

- Underline tabs: 14px/600, `--dim` at rest, `--text` when selected.
- A 2px `--ui-accent-text` underline marks the selected tab.
- Category tabs support arrow, Home and End keys.
- Rail and window controls support keyboard access.

### Selects and disclosures

- Selects use the site's chevron at the right edge.
- Disclosures use a chevron at the right edge that turns over when open.

### Tooltips

- Every tooltip in both windows uses one custom component, `tooltip.js`.
- Style: the site's hover card. Near-black (`--tip`), 12px radius, 1px `--line-strong` edge, deep shadow, no arrow.
- Maximum width: 290px.
- Optional bold title and key caps.
- A `title` written as "Label  Key" (two spaces) shows the shortcut as key caps.
- The component strips native `title` attributes, so native tooltips never show.
- Tooltips show for pointer hover and for keyboard focus (`:focus-visible`).
- Pointer down, Escape, scroll and window blur hide the tooltip at once.

### Dialogs

- Width: up to 440px. Radius 18px. Background `--canvas`. Edge 1px `--line-strong`.
- Backdrop: `--scrim` at 60% with an 8px blur.
- Title: 22px/700.
- Action row: right-aligned, 40px buttons, 8px gap.
- Destructive confirmations focus **Cancel** first.
- The update dialog is 540px wide with a close button in its header. Release notes sit in a `--well` box (12px radius, 1px `--line`) that scrolls on its own, at most 300px tall.
- Update dialog actions: **Skip this version** and the primary **Update now**. Focus starts on the dialog, so Enter does not start an update.
- During a download the action row becomes a 6px progress bar (`--ui-accent` on `--surface`) with one status line and **Cancel**. While it verifies and installs the bar slides and nothing can be closed.
- One line under the notes carries the state: dim for the restart note, `--warning` when an editor blocks the update, `--red` for a failure.

### Toast

- The site's light toast: `--toast` background, `--toast-text` text, 10px radius, 13px/600.
- In the editor the toast sits over the preview, centred at the bottom.
- In the main window it sits centred above the footer, 12px radius, with an optional underlined action and a close button. It leaves after 9 seconds. Used for "Updated to 1.2.3".

### Error card

- The app-wide error is a card that floats over the top of the page.
- It never pushes the page down.
- Actions: **Dismiss**, plus **Change shortcut** for a hotkey conflict.
- Maximum width: 720px. Radius 14px. Edge `--red` at 40%.
- Setup errors stay inline in the step they belong to.

## Motion

Motion follows the PlayerCard site. Shared values: `--fast` 130ms, `--ui` 180ms, `--ease` `cubic-bezier(.2,.8,.2,1)`.

### Timed transitions

| What | Duration | Easing | When |
|---|---|---|---|
| Control feedback (colour, edge, 0.97 press scale) | 130ms | `--ease` | Hover, press |
| Rail expand and collapse | 150ms | `--ease` | Pointer enters or leaves, keyboard focus |
| Rail labels | 120ms | `--ease` | With the rail |
| Switch track | 160ms | `--ease` | Toggle |
| Switch thumb | 180ms | `--ease` | Toggle |
| Disclosure chevron | 180ms | `--ease` | Open, close |
| Tab underline slide | 240ms | `--ease` | Tab change |
| Navigation cascade | 260ms, 6px rise, 40ms apart, at most four steps | `--ease` | Explicit navigation, tab change |
| Window-open cascade | 480ms, 10px rise, 60ms apart, up to seven steps | `cubic-bezier(.16,1,.3,1)` | Launch, tray reopen. Starts two frames after the window is revealed |
| Clip card cascade | 200ms, 4px rise, 24ms apart, first 12 cards. On window open: 340ms, 8px, 34ms apart | `--ease` | Opening Clips or Capture, tray reopen, list tab, filter or page change |
| Dialog entry | 240ms, 8px rise from 0.97 scale | `--ease` | Open |
| Dialog backdrop | 260ms in, 160ms out | `--ease` in, ease-in out | Open, close |
| Dialog exit | 150ms | ease-in | Close |
| Menus and popovers | 160ms | `--ease` | Open |
| Tooltip first-hover delay | 450ms | n/a | First hover |
| Tooltip delay between controls | 60ms | n/a | Within 400ms of the last tooltip, or keyboard focus |
| Tooltip in | 180ms | `--ease` | Show |
| Tooltip out | 90ms | ease-out | Hide |
| Clip thumbnail lift (3px) | 240ms | `--ease` | Hover, focus |
| Clip outline | 300ms in, 140ms out | ease-out | Hover, focus |
| Clip play disc | 200ms | `--ease` | Hover, focus |
| Clip thumbnail frame fade-in | 220ms | `--ease` | Thumbnail decoded |
| Error card entry | 220ms, 8px drop | `--ease` | Error shown |
| Theme cross-fade | 240ms | `--ease` | Theme change |
| Step and task marks | 220ms | `--ease` | State change |

### Continuous motion

| What | Period | When |
|---|---|---|
| Swept outline on a primary button | 4.8s a turn | Enabled primary buttons |
| Swept outline on the capture hero | 7s a turn | Replay buffer recording |
| Swept outline on a clip thumbnail | 4.8s a turn | Hovered or focused |
| Live dot pulse | 2.4s | Recording |
| Skeleton sweep | 1.3s | Value or thumbnail not loaded |
| Task spinner | 700ms a turn | Task in progress |

### Motion rules

- The swept outline is the signature. Use one per surface.
- The **Save replay** button inside a live hero holds still.
- Play buttons are filled accent without an outline.
- Nothing else moves on its own, except spinners that mark work in progress.
- Entrances occur on initial presentation, explicit navigation and tray reopening.
- Routine polling does not replay entrances.
- Mouse-wheel scrolling of the page eases over a few frames. Touchpads and keyboard scrolling are left alone.
- Wheel easing follows the Animations setting.

## States and behaviours

### Animations setting

- The persisted Animations setting (`animations_enabled`) controls app motion.
- The setting is independent of the Windows motion preference, matching the reference's explicit policy.
- Turning the setting off through **Apply** cancels effects and leaves the final static state.
- `html[data-motion="off"]` disables every animation and transition.
- Hiding the window or a trusted interaction cancels running entry effects.

### Hide to tray and reopen

1. The host tells the page that the window is hiding.
2. The page holds the current view invisible.
3. The WebView paints one frame in that state.
4. 180ms later the host hides and suspends the WebView.
5. On reopen the window shows the held state, never the old page.
6. The window-open cascade starts from the held state.
7. Polling waits 420ms, so the first frames belong to the animation.

### Initial presentation

- The page waits only for bounded font readiness (450ms at most) and two animation frames.
- The page then acknowledges `ui_ready`.
- Prepared real elements stay held until the native reveal, then progress together.
- No fake placeholders or delays wait for OBS startup.

### Capture summary loading

- Until the first state arrives, the four capture summary values are skeleton bars at their final size.
- The values never show dashes.

### Focus

- Focus ring: 2px `--focus` outline, 3px offset, on `:focus-visible` only.
- A mouse click on a rail item hands focus to the content.

## Do

- Take patterns from the PlayerCard site's own stylesheet before inventing one.
- Keep one button height and one radius per row.
- Keep replay readiness, **Save replay**, shortcut recovery and **Apply** discoverable.
- Keep hints to one short line. A hint that wraps at the default window size is too long.
- Give a rail click back to the page: move focus to the content, so the rail closes when the pointer leaves and keys scroll the page.
- Distinguish actual buffer activity from configured replay duration.
- Distinguish actual game capture from foreground app naming.
- Keep the microphone off by default.
- Keep whole-display capture explicit.
- Preserve dirty settings across navigation and failures.
- Verify in isolated, headless contexts. See [validation](validation.md).

## Don't

- Don't move the user's mouse or send desktop keyboard input during verification.
- Don't import XP, gold rewards, artwork, invented metrics, cursor effects or PlayerCard's application logic.
- Don't claim that fixture screenshots prove native recording.
- Don't claim that bridge queue acceptance confirms a save.
- Don't load runtime assets from the network.
- Don't hide errors behind generic success messages.
- Don't name a colour outside the `:root` token block and `themes.css`.
- Don't show native `title` tooltips.
