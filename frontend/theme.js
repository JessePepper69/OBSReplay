"use strict";

// Colour themes. Loaded in <head> without defer, so the chosen theme is on <html> before the first paint.
// The saved choice lives in the app's settings; a copy in localStorage lets every window (main, editors)
// apply it at once and follow a change made in another window. Colours themselves are in app.css (Original)
// and themes.css (the rest); this file only picks one and hands the timeline canvases their colours.
(() => {
  /*THEMES*/const THEMES = [
    { id: "original", name: "Original", scheme: "dark", swatch: ["#090d13","#19222e","#6bb6ff"] },
    { id: "mono", name: "Mono", scheme: "dark", swatch: ["#0a0a0a","#1e1e1e","#ededed"] },
    { id: "light", name: "Light", scheme: "light", swatch: ["#f2f4f7","#ffffff","#0068c6"] },
    { id: "rose", name: "Rose", scheme: "dark", swatch: ["#100b0d","#281e22","#f48eb6"] },
    { id: "amethyst", name: "Amethyst", scheme: "dark", swatch: ["#0d0c11","#21202b","#b6a1f5"] },
    { id: "ember", name: "Ember", scheme: "dark", swatch: ["#0f0c0a","#27201b","#fb9363"] }
  ];/*END*/
  const KEY = "obsreplay.theme";
  const root = document.documentElement;
  const listeners = new Set();
  const find = id => THEMES.find(theme => theme.id === id) || THEMES[0];
  let current = THEMES[0].id;
  try { current = find(localStorage.getItem(KEY)).id; } catch {}

  // A change of theme cross-fades the whole window (240ms) instead of snapping; the first paint and Animations off do not.
  function apply(id, animate = true) {
    if (animate && document.startViewTransition && !document.hidden && root.dataset.motion !== "off") { document.startViewTransition(() => paint(id)); return; }
    paint(id);
  }
  function paint(id) {
    const theme = find(id);
    current = theme.id;
    if (theme.id === THEMES[0].id) delete root.dataset.theme; else root.dataset.theme = theme.id;
    root.dataset.scheme = theme.scheme;
    document.querySelector('meta[name="theme-color"]')?.setAttribute("content", theme.swatch[0]);
    // The native window paints its own ground behind the page (seen while it opens or resizes); keep it in step.
    try { Promise.resolve(window.saucer?.exposed?.theme_changed?.(current)).catch(() => {}); } catch {}
    for (const listener of listeners) listener(current);
  }
  // Shows a theme in this window only (the Settings picker before Apply).
  function preview(id) { if (find(id).id !== current) apply(id); }
  // Records the saved theme for other windows and the next launch, without changing what this window shows.
  function save(id) { try { if (localStorage.getItem(KEY) !== find(id).id) localStorage.setItem(KEY, find(id).id); } catch {} }
  // Makes a theme the saved one for every window, this one included.
  function set(id) {
    save(id);
    if (find(id).id !== current) apply(find(id).id);
  }
  window.addEventListener("storage", event => { if (event.key === KEY && find(event.newValue).id !== current) apply(event.newValue); });

  const token = name => getComputedStyle(root).getPropertyValue(`--${name}`).trim();
  // Any CSS colour to [r, g, b]: the browser resolves it on a probe element, so tokens need not be six-digit hex.
  const probe = document.createElement("i");
  function channels(colour) {
    probe.style.color = "";
    probe.style.color = colour;
    if (!probe.isConnected) root.append(probe);
    const parts = getComputedStyle(probe).color.match(/[\d.]+/g);
    probe.remove();
    return parts && parts.length >= 3 ? parts.slice(0, 3).map(Number) : [128, 128, 128];
  }
  // amount = share of `to` in the result, 0 to 1.
  function mix(from, to, amount) {
    const a = channels(from), b = channels(to);
    return `rgb(${a.map((value, index) => Math.round(value + (b[index] - value) * amount)).join(",")})`;
  }
  const alpha = (hex, opacity) => `rgba(${channels(hex).join(",")},${opacity})`;
  // Colours for the editor and viewer timelines, which are drawn on a canvas and cannot read CSS variables themselves.
  function timeline() {
    const c = name => token(name) || "#808080";
    const well = c("well"), red = c("red"), text = c("text"), dim = c("dim"), playhead = c("playhead");
    return {
      lane: well, tick: c("line-hover"), label: dim, labelStrong: c("muted"), loading: dim,
      wave: c("wave"), waveBg: c("wave-bg"), waveSelected: c("wave-selected"), waveSelectedBg: c("wave-selected-bg"), laneEdge: c("lane-edge"),
      selectedVeil: alpha(c("ui-accent-text"), .16),
      removedVeil: alpha(c("page"), .78), removedHatch: alpha(red, .34), removedText: mix(red, text, .45),
      removedWave: mix(well, red, .42), removedWaveBg: mix(well, red, .07), removedEdge: mix(well, red, .42),
      playhead, handle: mix(playhead, dim, .18), hoverLine: alpha(text, .35),
      overviewTrack: c("surface"), overviewRemoved: mix(c("surface"), red, .3), overviewIdle: c("line-strong"), overviewThumb: c("line-hover"), overviewActive: dim,
      flag: c("raise"), flagText: text
    };
  }

  apply(current, false);
  window.Theme = { list: THEMES, get: () => current, set, save, preview, known: id => THEMES.some(theme => theme.id === id), token, timeline, onChange: listener => { listeners.add(listener); } };
})();
