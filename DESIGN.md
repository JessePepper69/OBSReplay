---
name: OBSReplay
description: A compact OBS replay companion in the PlayerCard desktop design language.
colors:
  canvas: "#10151d"
  surface: "#19222e"
  text: "#f5f7fc"
  muted: "#c0ccdd"
  dim: "#b6c4d8"
  line: "#344254"
  green: "#70dfad"
  red: "#ee8293"
  gray: "#9fabbe"
  warning: "#d3bd8b"
  ui-accent: "#0074d9"
  ui-accent-text: "#6bb6ff"
  control-border: "#738dac"
typography:
  title:
    fontFamily: "Inter, Segoe UI, sans-serif"
    fontSize: "24px"
    fontWeight: 650
    lineHeight: 1.3
    letterSpacing: "-0.6px"
  body:
    fontFamily: "Inter, Segoe UI, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: 1.5
  control:
    fontFamily: "Inter, Segoe UI, sans-serif"
    fontSize: "13px"
    fontWeight: 600
  state:
    fontFamily: "Inter, Segoe UI, sans-serif"
    fontSize: "23px"
    fontWeight: 600
    lineHeight: 1.4
rounded:
  control: "7px"
  panel: "10px"
  switch: "14px"
spacing:
  gutter: "26px"
  compact-gutter: "22px"
  row: "15px"
components:
  primary-button:
    backgroundColor: "{colors.ui-accent}"
    textColor: "{colors.text}"
    rounded: "{rounded.control}"
    padding: "6px 12px"
    height: "34px"
  input:
    backgroundColor: "#a1bfd00b"
    textColor: "{colors.text}"
    rounded: "{rounded.control}"
    padding: "7px 10px"
    height: "36px"
---

# Design System: OBSReplay

## Overview

The user's PlayerCard project is the explicit visual authority. OBSReplay adopts its navy canvas, Inter typography, blue primary actions, steel-blue controls, compact icon rail, ruled settings rows, switches and quick prepared-element motion. This supersedes the earlier neutral-black concept and its skill-generated direction. Generic palette or gradient bans do not override the user's chosen reference.

This remains a task-focused OBS companion. Capture readiness and Save replay lead. OBS stays open to perform capture; OBSReplay supplies configuration, shortcuts and tray controls. The design does not import PlayerCard's XP, reward colors, game artwork or activity statistics.

## Colors

Use the frontmatter tokens extracted from frontend/app.css. Canvas and surface match PlayerCard's established navy tones. Blue denotes primary actions and navigation selection; capture readiness uses semantic green. Warnings/errors retain text as well as color.

Secondary controls deliberately use PlayerCard's shallow steel-blue gradient, restrained edge and inset highlight. These functional control surfaces are part of the supplied design language. No extra page gradients, grain asset, background animation or cursor effects are introduced.

## Typography

Inter Variable is bundled locally at frontend/fonts/InterVariable.woff2 and preloaded. The unmodified SIL OFL license is beside it. The font and license were copied from the user's PlayerCard reference, which credits The Inter Project Authors. No remote font requests occur.

Use a compact 12/13/14/15/16/23/24px hierarchy, with weights 400–650. Page headings use the reference's 24px/650 treatment. Controls and settings titles are 13–14px; descriptions are 12–13px. Changing durations and file measures use tabular figures. Sentence case throughout.

## Layout

Default window: 1040×760 logical pixels; minimum 840×620. The 38px custom titlebar has native saucer drag, double-click maximize and window controls. Closing hides to tray; Help exposes explicit Quit.

The rail occupies 56px permanently and expands to 176px over content on pointer hover or keyboard focus. Expansion never shifts the content. Capture and Clips sit at the top; Settings and Help sit at the bottom. Icons always have accessible names; labels appear inside the expanded rail.

Content has a 26px gutter, reducing to 22px at compact width. Capture has one restrained state/action surface, a flat four-column settings summary, destination and latest-file rows. Settings uses Capture / Audio / App category tabs, aligned copy on the left and controls on the right. Paths and core setup controls come first; video encoding is a disclosure. The first setup shows a staged Start with Windows switch near the setup instruction. Startup is enabled by default but changes are persisted only through configuration/Apply.

Only the content reader scrolls; the titlebar, rail and action footer remain available. The Apply button stays reachable at minimum size. Narrow effective widths stack path controls and preserve vertical scrolling rather than shrink text. Controls in hidden categories retain their edits.

## Elevation & Depth

Use quiet blue-gray rules for sections and field borders. Secondary buttons have the reference's 1px inset highlight. The expanded rail and confirmation dialog have restrained dark shadows. The capture surface has one quiet border; avoid introducing a stack of generic cards.

## Shapes

Controls and rail selections use the reference's modest control radius. Capture/confirmation surfaces use the panel radius. Buttons are at least 34px high, Save replay 40px. Switch tracks are 42×24px inside a larger input hit region; their on/off text remains visible. No rounded primary pills.

## Components

- Capture displays actual native readiness. Save is available only for an owned, connected, ready, active replay buffer. Source failures expose the exact issue and a Repair OBS setup route; stopping active capture remains possible.
- Settings has one explicit Apply model. Dirty, pending, applied and failed states remain distinct. Full settings values are submitted; failed edits are retained. Repair forces the existing setup operation even without dirty changes.
- Hotkey conflicts have a Change shortcut action leading to Settings → App and the affected Change button. Recording suspends native shortcuts, supports Escape/reset, and expires before the native guard.
- Switches use semantic checkbox inputs with switch roles and visible state text. Every field has a real label. Category tabs support arrow/Home/End keys; rail and window controls support keyboard access.
- Motion follows PlayerCard: 130ms control feedback, 160ms rail/switch/detail transitions, 220ms prepared-element entries with 4px travel and 30ms stagger capped at 270ms, 160ms dialog entry and 110ms exit. Entrances occur on initial presentation, explicit navigation and tray reopening; routine polling does not replay them.
- The persisted Animations setting controls app motion independently of Windows, matching the reference's explicit policy. Turning it off through Apply cancels effects and leaves the final static state. Hiding or trusted interaction cancels running entry effects.
- Initial presentation waits only for bounded font readiness and two animation frames, then acknowledges ui_ready. Prepared real elements remain visible at partial opacity until the native reveal and progress together; no fake placeholders or delays wait for OBS startup.

## Do's and Don'ts

- Do extend the user's PlayerCard language rather than return to the rejected black/white concept.
- Do keep replay readiness, Save, shortcut recovery and Apply discoverable.
- Do distinguish actual buffer activity from configured replay duration and actual game capture from foreground app naming.
- Do keep microphone off by default and whole-display capture explicit.
- Do preserve dirty settings across navigation and failures.
- Do verify in isolated/headless contexts; do not move the user's mouse or send desktop keyboard input.
- Don't import XP, gold rewards, artwork, invented metrics, cursor effects or PlayerCard's application logic.
- Don't claim fixture screenshots prove native recording or that bridge queue acceptance confirms a save.
- Don't load runtime assets from the network or hide errors behind generic success messages.
