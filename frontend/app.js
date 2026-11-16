"use strict";

(() => {
  const $ = id => document.getElementById(id);
  const fields = ["obs_path", "clips_dir", "clip_seconds", "capture", "monitor_id", "output_height", "fps", "encoder", "codec", "quality", "mic_enabled", "mic_device", "desktop_device", "noise", "separate_tracks", "sort_by_app", "save_sound", "start_at_login", "save_hotkey", "show_hotkey", "replay_enabled", "animations_enabled", "theme"];
  const numbers = new Set(["clip_seconds", "output_height", "fps", "quality"]);
  const defaults = { obs_path: "", clips_dir: "", clip_seconds: 120, capture: "auto", monitor_id: "", output_height: 1080, fps: 60, encoder: "auto", codec: "h264", quality: 18, mic_enabled: true, mic_device: "default", desktop_device: "default", noise: "off", separate_tracks: false, sort_by_app: false, save_sound: true, start_at_login: true, save_hotkey: "Alt+Z", show_hotkey: "Alt+X", replay_enabled: false, animations_enabled: true, theme: "original" };
  const state = { snapshot: null, tab: "capture", dirty: false, loaded: false, pending: null, recording: null, dismissedError: "", localError: "", optionsKey: "", clipsKey: "", native: false, initialRoute: false };
  let nativeChain = Promise.resolve();
  let pollTimer;
  let polling = false;
  let recorderStarting = false;
  let recorderTimer;
  let presented = false;
  // The first page (Capture, or guided setup on a new install) is known only after the app has checked OBS.
  // Until then no page is drawn, so a new user never sees the main app flash before setup.
  let markRouted;
  const routed = new Promise(resolve => { markRouted = resolve; });
  document.documentElement.dataset.routing = "";
  routed.then(() => { delete document.documentElement.dataset.routing; });
  let dialogClosing = false;
  let category = "capture";
  let debugTimer = 0, debugTicks = 0, debugInfo = null;
  // Low space on the clips drive. Dismissing the warning hides it until a new replay has been saved and the window is
  // opened again: "the newest replay when the window last opened" is compared with the one at the time of dismissal.
  const diskKey = "obsreplay.diskDismissed";
  let diskSeenClip = null, diskResample = true;
  const newestReplay = () => (state.snapshot?.clips || []).find(clip => !clip.edited)?.path || "";
  function renderDiskNotice() {
    const disk = state.snapshot?.disk;
    const low = Boolean(disk && disk.total > 0 && disk.percent <= 5);
    let dismissed = null;
    try { dismissed = localStorage.getItem(diskKey); } catch {}
    $("disk-notice").hidden = !low || (dismissed !== null && dismissed === diskSeenClip);
    if (!low) return;
    const percent = disk.percent < 1 ? disk.percent.toFixed(1) : String(Math.floor(disk.percent));
    $("disk-text").textContent = `Drive ${disk.drive} has ${percent}% free (${(disk.free / 1024 ** 3).toFixed(1)} GB). Free up space so new replays can save.`;
  }
  // Lines for the session log from this page: navigation and script errors.
  const uiLog = (level, text) => { try { Promise.resolve(window.saucer?.exposed?.ui_log?.(level, String(text))).catch(() => {}); } catch {} };
  window.addEventListener("error", event => uiLog("error", `Script error: ${event.message} at ${String(event.filename || "").split("/").pop()}:${event.lineno}`));
  window.addEventListener("unhandledrejection", event => uiLog("error", `Unhandled rejection: ${event.reason?.message || event.reason}`));
  const entranceAnimations = new Set();
  let preparedElements = [];
  let fadeFrame = 0;
  const readerFade = document.createElement("div");
  readerFade.className = "reader-fade";
  readerFade.setAttribute("aria-hidden", "true");
  $("main").after(readerFade);

  function scheduleFade() {
    if (fadeFrame || document.hidden) return;
    fadeFrame = requestAnimationFrame(() => {
      fadeFrame = 0;
      const reader = $("main");
      const remaining = Math.max(0, reader.scrollHeight - reader.clientHeight - reader.scrollTop);
      const footerHeight = document.querySelector(".app-footer").offsetHeight;
      readerFade.style.bottom = `${footerHeight}px`;
      readerFade.style.opacity = String(Math.min(1, remaining / 28));
    });
  }

  function arrangeSettings() {
    const form = $("settings-form");
    const groups = {};
    for (const name of ["capture", "audio", "app", "debug"]) {
      const panel = document.createElement("div");
      panel.id = `settings-${name}`;
      panel.className = "settings-category";
      panel.setAttribute("role", "tabpanel");
      panel.setAttribute("aria-labelledby", `category-${name}`);
      panel.hidden = name !== "capture";
      groups[name] = panel;
    }
    const hints = { obs_path: "Your installed copy of OBS.", clips_dir: "Where saved replays go.", monitor_id: "Recorded when no game is.", clip_seconds: "How far back a save reaches.", quality: "Higher keeps more detail and takes more space.", desktop_device: "Game and system sound.", mic_device: "Used when Record microphone is on.", noise: "Cuts background noise." };
    function moveField(name, group) {
      const input = $(name);
      const row = input.closest(".field");
      const label = row.querySelector("label");
      const control = row.querySelector(".path-input") || input;
      const copy = document.createElement("div");
      copy.className = "setting-copy";
      copy.append(label);
      if (name === "capture") copy.append($("capture-hint"));
      else if (hints[name]) { const hint = document.createElement("p"); hint.textContent = hints[name]; copy.append(hint); }
      control.classList.add("setting-control");
      row.className = "setting-row entrance-item";
      row.replaceChildren(copy, control);
      groups[group].append(row);
    }
    for (const name of ["obs_path", "clips_dir", "capture", "monitor_id", "clip_seconds", "quality"]) moveField(name, "capture");
    const video = $("fps").closest("details");
    video.open = false;
    video.className = "video-details entrance-item";
    video.querySelector("summary").textContent = "Video encoding";
    groups.capture.append(video);
    moveField("desktop_device", "audio");
    groups.audio.append($("mic_enabled").closest(".check-row"));
    moveField("mic_device", "audio");
    moveField("noise", "audio");
    groups.audio.append($("separate_tracks").closest(".check-row"));
    for (const name of ["save_hotkey", "show_hotkey"]) {
      const input = $(name);
      const row = input.closest(".hotkey-row");
      const label = row.querySelector("label");
      let copy = label.parentElement;
      if (copy === row) { copy = document.createElement("div"); copy.append(label); }
      copy.className = "setting-copy";
      const controls = document.createElement("div");
      controls.className = "hotkey-control";
      controls.append(input, ...row.querySelectorAll("button"));
      row.className = "setting-row hotkey-row entrance-item";
      row.replaceChildren(copy, controls);
      groups.app.append(row);
    }
    groups.app.append($("hotkey-message"));
    const themeRow = document.createElement("div");
    themeRow.className = "setting-row theme-row entrance-item";
    themeRow.innerHTML = '<div class="setting-copy"><strong id="theme-label">Theme</strong></div><input type="hidden" id="theme" name="theme" value="original"><div class="theme-picker" id="theme-picker" role="radiogroup" aria-labelledby="theme-label"></div>';
    for (const theme of Theme.list) {
      const option = document.createElement("button");
      option.type = "button";
      option.setAttribute("role", "radio");
      option.dataset.themeId = theme.id;
      const swatch = document.createElement("span");
      swatch.className = "theme-swatch";
      swatch.style.background = theme.swatch[0];
      for (const colour of theme.swatch.slice(1)) { const part = document.createElement("i"); part.style.background = colour; swatch.append(part); }
      option.append(swatch, theme.name);
      themeRow.querySelector(".theme-picker").append(option);
    }
    groups.app.append(themeRow);
    const animationRow = document.createElement("label");
    animationRow.className = "check-row";
    animationRow.innerHTML = '<span><strong>Animations</strong><small>Page, panel and control transitions.</small></span><input type="checkbox" id="animations_enabled" name="animations_enabled" checked>';
    groups.app.append(animationRow);
    for (const name of ["start_at_login", "replay_enabled", "sort_by_app", "save_sound"]) groups.app.append($(name).closest(".check-row"));
    const updateRow = document.createElement("div");
    updateRow.className = "setting-row entrance-item";
    updateRow.id = "update-row";
    updateRow.innerHTML = '<div class="setting-copy"><strong>Updates</strong><small id="update-status" aria-live="polite">OBSReplay checks GitHub for new versions.</small></div><button type="button" id="update-check">Check now</button>';
    const updateAuto = document.createElement("label");
    updateAuto.className = "check-row";
    updateAuto.innerHTML = '<span><strong>Check for updates automatically</strong><small>Once a day. Nothing installs until you choose Update now.</small></span><input type="checkbox" id="update-auto" checked>';
    groups.app.append(updateRow, updateAuto);
    const startup = document.createElement("label");
    startup.className = "check-row";
    startup.id = "setup-startup-row";
    startup.innerHTML = '<span><strong>Start with Windows</strong><small>Keep replay controls available after you sign in.</small></span><input type="checkbox" id="setup-start-at-login" checked>';
    groups.capture.append(startup);
    const explanation = $("apply-explanation");
    explanation.textContent = "Capture and audio changes restart OBS. The replay buffer starts again from empty.";
    groups.debug.innerHTML = "<div class=\"debug-usage entrance-item\" id=\"debug-usage\"><div class=\"debug-usage-head\"><strong>Resource use</strong></div><table class=\"usage-table\"><thead><tr><th scope=\"col\"><span class=\"sr-only\">Measure</span></th><th scope=\"col\">OBSReplay</th><th scope=\"col\">OBS Studio</th><th scope=\"col\">Both</th></tr></thead><tbody><tr data-usage=\"cpu\"><th scope=\"row\">CPU</th><td>—</td><td>—</td><td>—</td></tr><tr data-usage=\"memory\"><th scope=\"row\">Memory</th><td>—</td><td>—</td><td>—</td></tr><tr data-usage=\"gpu\"><th scope=\"row\">GPU</th><td>—</td><td>—</td><td>—</td></tr><tr data-usage=\"gpu_encode\"><th scope=\"row\">GPU video encoder</th><td>—</td><td>—</td><td>—</td></tr><tr data-usage=\"gpu_memory\"><th scope=\"row\">GPU memory</th><td>—</td><td>—</td><td>—</td></tr><tr data-usage=\"processes\"><th scope=\"row\">Processes</th><td>—</td><td>—</td><td>—</td></tr></tbody></table><div class=\"hero-cells usage-cells\"><div><span>OBS frame rate</span><strong id=\"debug-fps\">—</strong></div><div><span>Frame render time</span><strong id=\"debug-render\">—</strong></div><div><span>Missed frames</span><strong id=\"debug-missed\">—</strong></div><div><span>Skipped frames</span><strong id=\"debug-skipped\">—</strong></div><div><span id=\"debug-disk-label\">Clips drive</span><strong id=\"debug-disk\">—</strong></div></div></div><div class=\"setting-row entrance-item\"><div class=\"setting-copy\"><strong>Version</strong><small id=\"debug-version\">—</small></div><button type=\"button\" id=\"debug-copy\">Copy</button></div><div class=\"setting-row entrance-item\"><div class=\"setting-copy\"><strong>Session log</strong><small id=\"debug-export-status\">Everything OBSReplay did since it opened. Export it and attach the file when you report a problem.</small></div><div class=\"inline-actions\"><button type=\"button\" id=\"debug-open-logs\">Open logs folder</button><button type=\"button\" id=\"debug-export\">Export log…</button></div></div><div class=\"debug-log-row entrance-item\"><strong id=\"debug-log-title\">Latest log lines</strong><pre class=\"debug-log\" id=\"debug-log\" tabindex=\"0\" aria-labelledby=\"debug-log-title\"></pre></div>";
    form.replaceChildren(groups.capture, groups.audio, groups.app, groups.debug, explanation);
    for (const row of form.querySelectorAll(".check-row")) {
      const input = row.querySelector("input");
      const copy = row.querySelector("span");
      copy.className = "setting-copy";
      const controls = document.createElement("span");
      controls.className = "switch-control";
      const text = document.createElement("span");
      text.className = "switch-label";
      text.dataset.switchLabel = input.id;
      text.setAttribute("aria-hidden", "true");
      const track = document.createElement("span");
      track.className = "switch-track";
      track.setAttribute("aria-hidden", "true");
      input.setAttribute("role", "switch");
      controls.append(text, input, track);
      row.className = "setting-row entrance-item";
      row.replaceChildren(copy, controls);
    }
    const setupCopy = document.createElement("div");
    const setupTitle = document.createElement("strong");
    setupTitle.textContent = "Replay capture isn’t set up yet";
    const setupHint = document.createElement("p");
    setupHint.textContent = "Pick your settings, then Set up and start.";
    setupCopy.append(setupTitle, setupHint);
    startup.classList.add("setup-startup");
    startup.querySelector("small").remove();
    setupCopy.append(startup);
    const webviewHelp = $("setup-intro").querySelector('[data-help="webview2"]');
    if (webviewHelp) $("help-links").append(webviewHelp);
    const guided = document.createElement("button");
    guided.type = "button";
    guided.id = "open-guided-setup";
    guided.textContent = "Guided setup";
    $("setup-intro").replaceChildren(setupCopy, guided);
    for (const item of document.querySelectorAll("#capture-view>*,#clips-view>*,#settings-view>.view-heading,#settings-view>.settings-tabs,#setup-intro,#help-view>.view-heading,#help-view .help-content>.card,#setup-view>.view-heading,#setup-view>.setup-steps,[data-step-panel]>.setup-panel,[data-step-panel]>.setup-hint")) item.classList.add("entrance-item");
  }

  // Holds the current view's blocks invisible, ready for the opening cascade.
  function prepareEntrance() {
    preparedElements = entranceTargets($(state.tab + "-view")).slice(0, 24);
    for (const element of preparedElements) { element.style.opacity = "0"; element.style.transform = "translateY(10px)"; }
  }
  let heldForReopen = false;
  function cancelEntrance() {
    heldForReopen = false;
    for (const animation of entranceAnimations) animation.cancel();
    entranceAnimations.clear();
    for (const element of preparedElements) { element.style.removeProperty("opacity"); element.style.removeProperty("transform"); }
    preparedElements = [];
  }
  function motionEnabled() { return settings().animations_enabled !== false; }
  function syncMotion() {
    document.documentElement.dataset.motion = motionEnabled() ? "on" : "off";
    if (!motionEnabled()) cancelEntrance();
  }
  function entranceTargets(view) { return [...view.querySelectorAll(".entrance-item")].filter(element => element.getClientRects().length && !element.closest("[hidden]")); }
  function playEntrance(view, opening = false) {
    cancelEntrance();
    if (!motionEnabled() || document.hidden) return;
    // Navigation: blocks rise in order (260ms, 6px, four 40ms steps). Opening the window: the same cascade, slower and softer
    // (480ms, 10px, seven 60ms steps), from elements already held invisible so nothing shows before it starts.
    const duration = opening ? 480 : 260, travel = opening ? 10 : 6, gap = opening ? 60 : 40, steps = opening ? 7 : 4;
    entranceTargets(view).slice(0, 24).forEach((element, index) => {
      const animation = element.animate([{ opacity: 0, transform: `translateY(${travel}px)` }, { opacity: 1, transform: "translateY(0)" }], { duration, delay: Math.min(index, steps) * gap, easing: opening ? "cubic-bezier(.16,1,.3,1)" : "cubic-bezier(.2,.8,.2,1)", fill: "backwards" });
      entranceAnimations.add(animation);
      animation.finished.catch(() => {}).finally(() => entranceAnimations.delete(animation));
    });
    // The clip cards on the page come in one after another, behind the blocks that hold them.
    const list = view.id === "clips-view" ? $("clips-list") : view.id === "capture-view" ? $("latest-content") : null;
    if (list) cascade(list, opening ? 200 : 90, opening);
  }
  async function presentUI() {
    // The window opens on the right page. A slow check still opens it, on an empty page, after 2.5 seconds.
    await Promise.race([routed, new Promise(resolve => setTimeout(resolve, 2500))]);
    await Promise.race([document.fonts.ready, new Promise(resolve => setTimeout(resolve, 450))]);
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    if (motionEnabled() && typeof api()?.ui_ready === "function") prepareEntrance();
    try { if (typeof api()?.ui_ready === "function") await nativeCall("ui_ready"); }
    catch (error) { showError(error); }
    presented = true;
    // Two frames after the window is revealed, so the cascade starts on a painted window and drops no frames.
    requestAnimationFrame(() => requestAnimationFrame(() => playEntrance($(state.tab + "-view"), true)));
    syncTabIndicators();
  }
  function showCategory(name, animate = true) {
    cancelRecording();
    const changed = category !== name;
    category = name;
    for (const button of document.querySelectorAll("[data-category]")) {
      const selected = button.dataset.category === name;
      button.setAttribute("aria-selected", String(selected));
      button.tabIndex = selected ? 0 : -1;
      $("settings-" + button.dataset.category).hidden = !selected;
    }
    $("setup-intro").hidden = configured() || name !== "capture";
    $("refresh-devices").hidden = name !== "audio";
    $("apply-explanation").hidden = name === "debug";
    $("settings-actions").hidden = state.tab !== "settings" || name === "debug";
    if (changed) uiLog("debug", `Settings tab: ${name}`);
    scheduleDebug();
    $("main").scrollTop = 0;
    if (changed && presented && animate) playEntrance($("settings-" + name));
    syncTabIndicators();
    scheduleFade();
  }

  function api() { return window.saucer?.exposed; }
  function nativeCall(name, ...args) {
    const call = nativeChain.then(() => {
      if (typeof api()?.[name] !== "function") throw new Error("OBSReplay isn’t connected to its background service. Close and reopen OBSReplay.");
      return api()[name](...args);
    });
    nativeChain = call.catch(() => {});
    return call;
  }
  function unpack(value) {
    if (typeof value !== "string") return value;
    try { return JSON.parse(value); } catch { return value; }
  }
  function errorText(value) {
    if (!value) return "";
    if (typeof value === "string") return value;
    return value.message || JSON.stringify(value);
  }
  function showError(error) {
    state.localError = errorText(error);
    if (state.localError) uiLog("warn", `Error shown: ${state.localError}`);
    state.dismissedError = "";
    renderFeedback();
  }
  function configured() { return state.snapshot?.integration === true; }
  function needsRepair() { return configured() && state.snapshot?.capture_ready === false && Boolean(state.snapshot?.capture_issue); }
  function settings() { return { ...defaults, ...(state.snapshot?.settings || {}) }; }
  function busy() { return Boolean(state.pending || state.snapshot?.busy); }
  function formatDuration(seconds) {
    const value = Number(seconds);
    if (!Number.isFinite(value)) return "—";
    return value < 60 ? `${value} seconds` : `${Number((value / 60).toFixed(1))} ${value === 60 ? "minute" : "minutes"}`;
  }
  function formatBytes(bytes) {
    const value = Number(bytes);
    if (!Number.isFinite(value) || value < 0) return "";
    if (value < 1024 * 1024) return `${new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 }).format(value / 1024)} KB`;
    return `${new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 }).format(value / (1024 * 1024))} MB`;
  }
  function formatTime(value) {
    if (!value) return "";
    const numeric = Number(value);
    const date = new Date(Number.isFinite(numeric) ? numeric * (numeric < 1e12 ? 1000 : 1) : value);
    return Number.isNaN(date.getTime()) ? String(value) : new Intl.DateTimeFormat(undefined, { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" }).format(date);
  }
  function setOptions(id, items, prefix = []) {
    const select = $(id);
    const previous = select.value;
    const all = [...prefix];
    for (const item of items || []) {
      const value = String(item.id ?? item.value ?? "");
      if (!value) continue;
      const index = all.findIndex(option => option.value === value);
      if (index >= 0) all[index] = { value, label: item.label || value };
      else all.push({ value, label: item.label || value });
    }
    const wanted = previous || String(settings()[id] ?? "");
    if (wanted && !all.some(item => item.value === wanted)) all.push({ value: wanted, label: `${wanted} (configured)` });
    if (!all.length) all.push({ value: "", label: "No devices available" });
    select.replaceChildren(...all.map(item => {
      const option = document.createElement("option");
      option.value = item.value;
      option.textContent = item.label;
      return option;
    }));
    if (all.some(item => item.value === wanted)) select.value = wanted;
  }
  function renderOptions() {
    const options = state.snapshot?.options || {};
    const key = JSON.stringify(options);
    if (key === state.optionsKey) return;
    state.optionsKey = key;
    setOptions("monitor_id", options.monitors || []);
    setOptions("mic_device", options.audio_inputs || [], [{ value: "default", label: "System default" }]);
    setOptions("desktop_device", options.audio_outputs || [], [{ value: "default", label: "System default" }, { value: "off", label: "Off" }]);
    setOptions("encoder", options.encoders || [], [{ value: "auto", label: "Automatic" }]);
    updateCodecs();
  }
  function fillForm(value) {
    const current = { ...defaults, ...value };
    for (const name of fields) {
      const input = $(name);
      if (input.type === "checkbox") input.checked = Boolean(current[name]);
      else {
        let text = String(current[name] ?? "");
        // A fresh setup has no display chosen yet; show the primary display (listed first) instead of a blank entry.
        if (name === "monitor_id" && !text && input.options.length && input.options[0].value) text = input.options[0].value;
        if (input.tagName === "SELECT" && ![...input.options].some(option => option.value === text)) {
          const option = document.createElement("option");
          option.value = text;
          option.textContent = `${text || "Default"} (configured)`;
          input.append(option);
        }
        input.value = text;
      }
    }
    if (!Theme.known($("theme").value)) $("theme").value = defaults.theme;
    state.dirty = false;
    syncThemePicker();
    // Real settings show their theme in this window; the placeholder defaults used before the first state do not.
    if (state.snapshot) Theme.preview($("theme").value);
    showPathEnds();
    $("setup-start-at-login").checked = $("start_at_login").checked;
    updateCodecs();
    updateDependencies();
  }
  function showPathEnds() {
    for (const id of ["obs_path", "clips_dir", "setup-folder"]) { const input = $(id); if (document.activeElement !== input) input.scrollLeft = input.scrollWidth; }
  }
  function syncThemePicker() {
    const chosen = $("theme").value || defaults.theme;
    for (const option of $("theme-picker").children) {
      const selected = option.dataset.themeId === chosen;
      option.setAttribute("aria-checked", String(selected));
      option.tabIndex = selected ? 0 : -1;
    }
  }
  function chooseTheme(id, focus = false) {
    $("theme").value = id;
    syncThemePicker();
    Theme.preview(id);
    if (focus) $("theme-picker").querySelector('[aria-checked="true"]')?.focus();
    markDirty();
    // A theme needs no Apply: it is saved at once, so editors and the next launch use it too.
    // While another action runs the choice waits for Apply, as before.
    if (state.snapshot?.settings && typeof api()?.set_theme === "function") Promise.resolve(api().set_theme(id)).then(() => {
      state.snapshot.settings.theme = id;
      Theme.save(id);
      markDirty();
    }).catch(() => {});
  }
  function formValue() {
    const value = { ...settings() };
    for (const name of fields) {
      const input = $(name);
      value[name] = input.type === "checkbox" ? input.checked : numbers.has(name) ? Number(input.value) : input.value.trim();
    }
    return value;
  }
  const captureNotes = {
    auto: "A fullscreen game while one runs, your display otherwise.",
    game: "Fullscreen games only.",
    desktop: "Your whole display, all the time."
  };
  const captureLabels = { auto: "Game or display (Hybrid)", game: "Fullscreen games", desktop: "Entire display" };
  function updateDependencies() {
    const mode = $("capture").value;
    $("capture-hint").textContent = captureNotes[mode] || captureNotes.auto;
    $("monitor_id").disabled = mode === "game" || busy();
    $("mic_device").disabled = !$("mic_enabled").checked || busy();
    $("noise").disabled = !$("mic_enabled").checked || busy();
    syncSetupControls();
    for (const label of document.querySelectorAll("[data-switch-label]")) label.textContent = $(label.dataset.switchLabel).checked ? "On" : "Off";
  }
  function updateCodecs(chooseSupported = false) {
    const selected = state.snapshot?.options?.encoders?.find(encoder => encoder.id === $("encoder").value);
    const supported = selected?.codecs;
    for (const option of $("codec").options) option.disabled = Array.isArray(supported) && !supported.includes(option.value);
    if (chooseSupported && $("codec").selectedOptions[0]?.disabled) {
      const first = [...$("codec").options].find(option => !option.disabled);
      if (first) $("codec").value = first.value;
    }
  }
  function markDirty() {
    state.dirty = JSON.stringify(formValue()) !== JSON.stringify(settings());
    updateDependencies();
    renderFeedback();
  }
  function showTab(tab) {
    cancelRecording();
    if (tab === "capture" && state.snapshot && (!configured() || setup.flow === "running")) tab = "setup";
    const changed = state.tab !== tab;
    state.tab = tab;
    if (changed) uiLog("debug", `Page: ${tab}`);
    scheduleDebug();
    for (const name of ["setup", "capture", "clips", "settings", "help"]) $(name + "-view").hidden = name !== tab;
    for (const button of document.querySelectorAll("[data-tab]")) {
      if (button.dataset.tab === tab || (tab === "setup" && button.dataset.tab === "capture")) button.setAttribute("aria-current", "page");
      else button.removeAttribute("aria-current");
    }
    if (tab === "setup") { renderSetup(); if (setup.step === "obs") pollFfmpeg(true); }
    if (tab === "help") $("help-button").setAttribute("aria-current", "page");
    else $("help-button").removeAttribute("aria-current");
    $("main").scrollTop = 0;
    if (changed && presented) playEntrance($(tab + "-view"));
    syncTabIndicators();
    showPathEnds();
    scheduleFade();
    renderFeedback();
  }
  // Errors with one obvious remedy get a button that applies it and tries again, so nobody has to hunt for the setting.
  const errorFixes = [
    { match: /No microphone is connected/, label: "Turn off microphone", apply() { $("mic_enabled").checked = false; } },
    { match: /selected microphone is disconnected/, label: "Use default microphone", apply() { $("mic_device").value = "default"; } },
    { match: /No speakers or headphones are active/, label: "Turn off desktop audio", apply() { $("desktop_device").value = "off"; } },
    { match: /selected desktop audio device is disconnected/, label: "Use default device", apply() { $("desktop_device").value = "default"; } },
    { match: /selected monitor is disconnected/, label: "Use main display", apply() { $("monitor_id").value = $("monitor_id").options[0]?.value || ""; } }
  ];
  const errorFix = text => errorFixes.find(fix => fix.match.test(text || "")) || null;
  function applyErrorFix(text) {
    const fix = errorFix(text);
    if (!fix || busy()) return;
    fix.apply();
    markDirty();
    syncSetupControls();
    syncDropdowns();
    state.dismissedError = text; state.localError = "";
    if (!configured() || state.tab === "setup") runSetup(formValue());
    else { renderFeedback(); $("settings-form").requestSubmit(); }
  }
  function renderFeedback() {
    const snapshot = state.snapshot;
    const backendError = [errorText(snapshot?.error), errorText(snapshot?.hotkey_error)].filter(Boolean).join("\n");
    const error = state.localError || backendError;
    const visibleError = error && error !== state.dismissedError;
    $("error-notice").hidden = !visibleError || state.tab === "setup";
    $("error-text").textContent = visibleError ? error : "";
    $("change-conflicting-shortcut").hidden = !errorText(snapshot?.hotkey_error) || !visibleError;
    $("change-conflicting-shortcut").disabled = !state.native || busy();
    const noticeFix = visibleError ? errorFix(error) : null;
    $("error-fix").hidden = !noticeFix;
    $("error-fix").textContent = noticeFix?.label || "";
    $("error-fix").dataset.error = noticeFix ? error : "";
    $("error-fix").disabled = !state.native || busy();
    $("settings-actions").hidden = state.tab !== "settings" || category === "debug";
    $("footer-hide").hidden = state.tab === "settings" || state.tab === "setup";
    const pending = busy();
    for (const button of document.querySelectorAll("#replay-player .viewer-actions button")) button.disabled = !state.native || pending;
    $("apply-settings").textContent = pending && ["setup", "apply"].includes(state.pending?.action || snapshot?.operation) ? "Applying…" : needsRepair() ? "Repair OBS setup" : snapshot?.compression_update ? "Apply improved compression" : configured() ? "Apply changes" : "Set up and start";
    $("apply-settings").disabled = !state.native || pending || (!state.dirty && configured() && !needsRepair() && !snapshot?.compression_update);
    $("discard-settings").disabled = !state.dirty || pending;
    if (!state.native) $("footer-status").textContent = "Not connected. Reopen OBSReplay.";
    else if (pending) $("footer-status").textContent = snapshot?.message || "Working…";
    else if (state.tab === "settings" && state.dirty) $("footer-status").textContent = "Unapplied changes";
    else if (state.tab === "setup") $("footer-status").textContent = setupFooterText();
    else $("footer-status").textContent = snapshot?.message || (configured() ? "Settings saved" : "Set up replay capture to get started");
    for (const element of document.querySelectorAll("[data-action], [data-help], #footer-hide, #hide-window, #quit-app, #reset-integration, #refresh-devices, #pick-obs, #pick-folder, #setup-start-at-login")) element.disabled = !state.native || pending;
    for (const element of $("settings-form").elements) if (element.id !== "apply-settings") element.disabled = !state.native || pending;
    if (state.native && !pending) updateDependencies();
    const canControl = Boolean(snapshot?.connected && snapshot?.owned);
    const needsAttention = canControl && snapshot?.capture_ready === false;
    $("save-button").disabled = !state.native || pending || !canControl || !snapshot?.replay_active || needsAttention;
    $("save-label").textContent = state.pending?.action === "save" || snapshot?.operation === "save" && snapshot?.busy ? "Saving…" : "Save replay";
    $("capture-repair").disabled = !state.native || pending;
    $("toggle-capture").disabled = !state.native || pending || !configured() || Boolean(snapshot?.connected && !snapshot?.owned) || Boolean(needsAttention && !snapshot?.replay_active);
    renderSetup();
    syncDropdowns();
    scheduleFade();
  }
  function renderCapture() {
    renderDiskNotice();
    const snapshot = state.snapshot || {};
    const value = settings();
    const active = Boolean(snapshot.replay_active && snapshot.connected && snapshot.owned);
    $("connection-label").textContent = !state.native ? "Not connected" : operating("setup") ? "Setting up…" : operating("install_obs") ? "Installing OBS Studio…" : !configured() && !snapshot.connected ? "Not set up yet" : snapshot.connected ? snapshot.owned ? "OBS connected" : "Other OBS session" : snapshot.busy ? "Connecting to OBS…" : "OBS disconnected";
    $("connection-dot").className = `state-dot${snapshot.connected && snapshot.owned ? " ready" : ""}`;
    let title = "Capture stopped";
    let description = "Start capture to keep your recent moments ready to save.";
    if (!state.native) { title = "Open the desktop app"; description = "This page needs the OBSReplay app. Open OBSReplay.exe to control capture."; }
    else if (!configured()) { title = "Ready to set up"; description = "Configure a dedicated OBS profile to get started."; }
    else if (snapshot.connected && !snapshot.owned) { title = "Different OBS session"; description = "Switch to the OBSReplay profile and collection before controlling capture."; }
    else if (active) { title = "Capturing"; description = `Save up to the last ${formatDuration(value.clip_seconds)} with your replay shortcut.`; }
    else if (!snapshot.connected) { title = snapshot.busy ? "Connecting…" : "OBS disconnected"; description = "Start capture or reconnect to your dedicated OBS session."; }
    if (snapshot.connected && snapshot.owned && snapshot.capture_ready === false) {
      title = "Capture needs attention";
      description = `${snapshot.capture_issue || "The dedicated capture source is not ready."}`;
    }
    if (snapshot.busy && ["start", "stop"].includes(snapshot.operation)) title = snapshot.operation === "start" ? "Starting capture…" : "Stopping capture…";
    $("capture-state").textContent = title;
    $("capture-description").textContent = description;
    $("capture-mark").className = `capture-mark${snapshot.connected && (!snapshot.owned || snapshot.capture_ready === false) ? " warning" : active ? " active" : ""}`;
    $("capture-hero").classList.toggle("live", active);
    // One primary per state: Save while capturing, Repair when the capture source is broken, otherwise Start.
    $("capture-repair").hidden = !needsRepair();
    $("save-button").hidden = !active || needsRepair();
    $("toggle-capture").classList.toggle("primary", !active && !needsRepair());
    $("toggle-capture").textContent = active ? "Stop capture" : "Start capture";
    $("save-shortcut").textContent = value.save_hotkey;
    $("summary-length").textContent = formatDuration(value.clip_seconds);
    $("summary-mode").textContent = captureLabels[value.capture] || captureLabels.auto;
    $("summary-video").textContent = `${value.output_height ? `${value.output_height}p` : "Original"} / ${value.fps} fps`;
    $("summary-mic").textContent = value.mic_enabled ? "On" : "Off";
    $("capture-folder").textContent = value.clips_dir || "Choose a destination in Settings.";
    $("connection-actions").hidden = Boolean(snapshot.connected && snapshot.owned);
    $("setup-intro").hidden = configured() || category !== "capture";
    $("setup-startup-row").hidden = configured();
    $("help-status").textContent = snapshot.status ? `${snapshot.status}${snapshot.message ? `: ${snapshot.message}` : ""}` : $("connection-label").textContent;
  }
  const icons = {
    edit: '<svg viewBox="0 0 20 20" aria-hidden="true"><circle cx="5.5" cy="6" r="2"/><circle cx="5.5" cy="14" r="2"/><path d="M7.3 7.2 16 14M7.3 12.8 16 6"/></svg>',
    open: '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M11.5 4H16v4.5M16 4l-6.5 6.5M14 11.5v3a1.5 1.5 0 0 1-1.5 1.5h-7A1.5 1.5 0 0 1 4 14.5v-7A1.5 1.5 0 0 1 5.5 6h3"/></svg>',
    folder: '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h3l1.6 2h6.4A1.5 1.5 0 0 1 17 8.5v6a1.5 1.5 0 0 1-1.5 1.5h-11A1.5 1.5 0 0 1 3 14.5z"/></svg>',
    trash: '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M4.5 6h11M8 6V4.5h4V6M6 6l.6 9a1.5 1.5 0 0 0 1.5 1.4h3.8a1.5 1.5 0 0 0 1.5-1.4L14 6M8.5 9v4.5M11.5 9v4.5"/></svg>'
  };
  // Quick delete on a clip card. A click asks first; Shift+click deletes at once.
  function deleteButton(clip) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "icon-only delete-clip";
    button.setAttribute("aria-label", `Delete ${clip.name || "replay"}`);
    button.dataset.tipTitle = "Delete";
    button.dataset.tip = "Shift+click deletes without asking";
    button.innerHTML = icons.trash;
    button.addEventListener("click", async event => {
      if (busy()) return;
      const name = clip.name || (clip.edited ? "this video" : "this replay");
      if (!event.shiftKey && !await confirmAction(clip.edited ? "Delete edited video?" : "Delete replay?", `Permanently delete “${name}” from your computer? This cannot be undone.`, "Delete", true)) return;
      // Nothing may still be reading the file when Windows deletes it.
      if (playerClip?.path === clip.path) { ReplayViewer.release(); playerClip = null; closePlayer(); }
      dispatch("delete_clip", { path: clip.path });
    });
    return button;
  }
  function iconAction(label, icon, action, clip) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "icon-only";
    button.setAttribute("aria-label", `${label}: ${clip.name || "replay"}`);
    button.title = label;
    button.innerHTML = icon;
    button.addEventListener("click", () => dispatch(action, { path: clip.path }));
    return button;
  }
  // Clip details as separate pieces (app, saved time, size) so they align and wrap cleanly.
  function fillMeta(container, clip, size = true) {
    container.replaceChildren();
    for (const [text, name] of [[clip.app ? gameName(clip.app) : "", "clip-app"], [formatTime(clip.saved_at), ""], [size ? formatBytes(clip.bytes) : "", ""]]) {
      if (!text) continue;
      const piece = document.createElement("span");
      if (name) piece.className = name;
      piece.textContent = text;
      container.append(piece);
    }
  }
  // Segmented tabs share one selected fill that slides to the chosen tab.
  function syncTabIndicators() {
    for (const tabs of document.querySelectorAll(".settings-tabs")) {
      const selected = tabs.querySelector('[aria-selected="true"]');
      if (!selected || !selected.offsetWidth) continue;
      tabs.style.setProperty("--tab-x", `${selected.offsetLeft}px`);
      tabs.style.setProperty("--tab-w", `${selected.offsetWidth}px`);
      if (!("indicator" in tabs.dataset)) requestAnimationFrame(() => { tabs.dataset.indicator = ""; });
    }
  }
  function animateDialog(dialog, opening) {
    if (!motionEnabled()) return Promise.resolve();
    const animation = opening
      ? dialog.animate([{ opacity: 0, transform: "translateY(8px) scale(.97)" }, { opacity: 1, transform: "translateY(0) scale(1)" }], { duration: 240, easing: "cubic-bezier(.2,.8,.2,1)" })
      : dialog.animate([{ opacity: 1, transform: "translateY(0) scale(1)" }, { opacity: 0, transform: "translateY(5px) scale(.98)" }], { duration: 150, easing: "ease-in", fill: "forwards" });
    entranceAnimations.add(animation);
    return animation.finished.catch(() => {}).finally(() => entranceAnimations.delete(animation));
  }
  async function openEditor(clip) {
    try { await nativeCall("open_editor", clip.path); }
    catch (error) { showError(error); }
  }
  function editButton(clip) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "icon-only edit-clip";
    button.setAttribute("aria-label", `Edit ${clip.name || "replay"}`);
    button.dataset.tipTitle = "Edit";
    button.dataset.tip = "Cut sections out and export a new video";
    button.innerHTML = icons.edit;
    button.addEventListener("click", () => openEditor(clip));
    return button;
  }
  const thumbnailCache = new Map();
  const thumbnailJobs = new Map();
  let thumbnailRunning = false;
  let cancelThumbnail = null;
  let playerClip = null;
  let playerReturnFocus = null;
  const clipKey = clip => JSON.stringify([clip.path, clip.bytes, clip.saved_at]);
  const clipLengths = new Map();
  function formatLength(seconds) {
    const total = Math.max(1, Math.round(seconds)), hours = Math.floor(total / 3600), minutes = Math.floor(total / 60) % 60, rest = total % 60;
    return hours ? `${hours}h ${minutes}m` : minutes ? `${minutes}m ${rest}s` : `${rest}s`;
  }
  function paintThumbnail(image, data, cached = false) {
    if (!image.isConnected) return;
    const length = clipLengths.get(image.clipKey), chip = image.parentElement.querySelector(".clip-length");
    if (chip && length) { chip.textContent = formatLength(length); chip.hidden = false; }
    // A frame already in the cache appears at once; only a newly decoded one fades in.
    if (cached) image.style.animation = "none";
    if (data) { image.src = data; image.hidden = false; image.parentElement.dataset.thumbnail = "ready"; }
    else image.parentElement.dataset.thumbnail = "unavailable";
  }
  const thumbnailObserver = new IntersectionObserver(entries => {
    for (const entry of entries) if (entry.isIntersecting) {
      thumbnailObserver.unobserve(entry.target);
      const { clip, image } = entry.target.replayThumbnail;
      const key = clipKey(clip);
      if (thumbnailCache.has(key)) paintThumbnail(image, thumbnailCache.get(key), true);
      else {
        const job = thumbnailJobs.get(key) || { clip, targets: [] };
        job.targets = job.targets.filter(target => target.isConnected);
        job.targets.push(image);
        thumbnailJobs.set(key, job);
      }
    }
    drainThumbnails();
  }, { root: $("main"), rootMargin: "100px" });
  async function takeThumbnail(clip) {
    const url = await nativeCall("media_url", clip.path);
    if (document.hidden || $("replay-player").open) throw new DOMException("Preview deferred", "AbortError");
    return new Promise((resolve, reject) => {
      const video = document.createElement("video");
      video.muted = true;
      video.preload = "auto";
      video.crossOrigin = "anonymous";
      let finished = false;
      const done = (data, error) => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        video.onloadeddata = video.onseeked = video.onerror = null;
        video.removeAttribute("src"); video.load();
        cancelThumbnail = null;
        error ? reject(error) : resolve(data);
      };
      const timer = setTimeout(() => done(null), 8000);
      cancelThumbnail = () => done(null, new DOMException("Preview deferred", "AbortError"));
      const capture = () => {
        try {
          if (!video.videoWidth || !video.videoHeight) return done(null);
          const canvas = document.createElement("canvas"); canvas.width = 320; canvas.height = 180;
          const context = canvas.getContext("2d");
          context.fillStyle = "#080c12"; context.fillRect(0, 0, 320, 180);
          const scale = Math.min(320 / video.videoWidth, 180 / video.videoHeight);
          const width = video.videoWidth * scale, height = video.videoHeight * scale;
          context.drawImage(video, (320 - width) / 2, (180 - height) / 2, width, height);
          done(canvas.toDataURL("image/jpeg", .78));
        } catch { done(null); }
      };
      video.onloadeddata = () => {
        video.onloadeddata = null;
        if (Number.isFinite(video.duration)) clipLengths.set(clipKey(clip), video.duration);
        const position = Math.min(.75, Number.isFinite(video.duration) ? video.duration * .1 : 0);
        if (position > .01) { video.onseeked = capture; video.currentTime = position; }
        else capture();
      };
      video.onerror = () => done(null);
      video.src = url; video.load();
    });
  }
  async function drainThumbnails() {
    if (thumbnailRunning || document.hidden || $("replay-player").open) return;
    thumbnailRunning = true;
    try {
      for (const [key, job] of thumbnailJobs) {
        if (document.hidden || $("replay-player").open) break;
        job.targets = job.targets.filter(image => image.isConnected);
        if (!job.targets.length) { thumbnailJobs.delete(key); continue; }
        try {
          const data = await takeThumbnail(job.clip);
          const stillKnown = state.snapshot?.clips?.some(clip => clipKey(clip) === key);
          if (stillKnown) {
            thumbnailCache.set(key, data);
            while (thumbnailCache.size > 120) thumbnailCache.delete(thumbnailCache.keys().next().value);
            for (const image of job.targets) paintThumbnail(image, data);
          }
          thumbnailJobs.delete(key);
        } catch (error) {
          if (error.name === "AbortError") break;
          for (const image of job.targets) paintThumbnail(image, null);
          thumbnailCache.set(key, null);
          thumbnailJobs.delete(key);
        }
      }
    } finally { thumbnailRunning = false; }
  }
  // The replay viewer itself (video, transport, timeline) lives in viewer.js; this is the glue around it.
  function stopPlayer() {
    ReplayViewer.release();
    // An empty request makes a waveform decode still running for the closed clip stale, so FFmpeg stops.
    if (typeof api()?.clip_waveform === "function") Promise.resolve(api().clip_waveform("")).catch(() => {});
    playerClip = null;
    drainThumbnails();
  }
  let playerClosing = false;
  function closePlayer() {
    const dialog = $("replay-player");
    if (!dialog.open || playerClosing) return;
    if (!motionEnabled() || document.hidden) { dialog.close(); return; }
    playerClosing = true;
    ReplayViewer.pause();
    dialog.classList.add("closing");
    animateDialog(dialog, false).then(() => { dialog.classList.remove("closing"); for (const animation of dialog.getAnimations()) animation.cancel(); playerClosing = false; if (dialog.open) dialog.close(); });
  }
  function watchReplay(clip) {
    const alreadyOpen = $("replay-player").open;
    cancelThumbnail?.();
    if (!alreadyOpen) playerReturnFocus = document.activeElement;
    playerClip = clip;
    fillMeta($("player-detail"), clip);
    ReplayViewer.open(clip, {
      mediaUrl: path => nativeCall("media_url", path),
      // Decoding the waveform can take a moment, so it stays out of the serialized native queue.
      waveform: path => typeof api()?.clip_waveform === "function" ? api().clip_waveform(path) : Promise.reject(new Error("unavailable")),
      fps: settings().fps
    });
    if (!alreadyOpen) animateDialog($("replay-player"), true);
  }
  function clipRow(clip, compact = false) {
    const row = document.createElement("div");
    row.className = "clip-card";
    const preview = document.createElement("button");
    preview.type = "button"; preview.className = "clip-preview";
    preview.setAttribute("aria-label", `Watch ${clip.name || "replay"}`);
    preview.addEventListener("click", () => watchReplay(clip));
    const thumbnail = document.createElement("span"); thumbnail.className = "clip-thumbnail";
    const image = document.createElement("img"); image.alt = ""; image.hidden = true;
    const play = document.createElement("span"); play.className = "clip-play";
    play.innerHTML = '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="m7 4 9 6-9 6z"/></svg>';
    thumbnail.append(image, play);
    const size = formatBytes(clip.bytes);
    if (size) { const chip = document.createElement("span"); chip.className = "clip-chip"; chip.textContent = size; thumbnail.append(chip); }
    const length = document.createElement("span"); length.className = "clip-chip clip-length"; length.hidden = true; thumbnail.append(length);
    image.clipKey = clipKey(clip);
    thumbnail.replayThumbnail = { clip, image };
    thumbnailObserver.observe(thumbnail);
    const name = document.createElement("span");
    name.className = "clip-title";
    name.textContent = clip.name || clip.path?.split(/[\\/]/).pop() || "Saved replay";
    const meta = document.createElement("span");
    meta.className = "clip-meta";
    fillMeta(meta, clip, false);
    const foot = document.createElement("div");
    foot.className = "clip-foot";
    const actions = document.createElement("div");
    actions.className = "clip-actions";
    actions.append(editButton(clip), iconAction("Open in your video player", icons.open, "open_clip", clip), iconAction("Show in folder", icons.folder, "reveal_clip", clip), deleteButton(clip));
    preview.append(thumbnail);
    foot.append(meta, name);
    row.append(preview, foot, actions);
    return row;
  }
  function renderClips() {
    const clips = Array.isArray(state.snapshot?.clips) ? state.snapshot.clips : [];
    // The empty state names the save shortcut while capture runs, so it follows the capture state too.
    const key = JSON.stringify(clips.some(clip => !clip.edited) ? clips : [clips, capturing(), settings().save_hotkey, settings().clip_seconds]);
    if (key === state.clipsKey) return;
    state.clipsKey = key;
    thumbnailObserver.disconnect();
    const known = new Set(clips.map(clipKey));
    for (const key of thumbnailCache.keys()) if (!known.has(key)) thumbnailCache.delete(key);
    for (const key of thumbnailJobs.keys()) if (!known.has(key)) thumbnailJobs.delete(key);
    for (const key of clipLengths.keys()) if (!known.has(key)) clipLengths.delete(key);
    if (playerClip && !clips.some(clip => clip.path === playerClip.path)) {
      closePlayer();
      showError("This replay was deleted or moved. Your recent clips have been updated.");
    }
    renderClipLists(clips);
  }
  // Recorded replays and edited exports are separate lists, on Clips and in Latest on Capture.
  const clipTabs = { clips: "replays", latest: "replays" };
  try {
    const saved = JSON.parse(localStorage.getItem("obsreplay.clipTabs") || "{}");
    for (const list of Object.keys(clipTabs)) if (["replays", "edits"].includes(saved[list])) clipTabs[list] = saved[list];
  } catch {}
  // Clips page: one game at a time if wanted, 24 clips a page.
  const pageSize = 24;
  const clipView = { app: null, page: 0 };
  // The recorded app is an executable name; drop build suffixes so it reads as the game ("FortniteClient-Win64-Shipping" is Fortnite).
  function gameName(app) {
    const name = String(app || "").replace(/\.exe$/i, "").replace(/[-_ ]?(Win64|Win32|WinGDK|x64|x86)([-_ ]?Shipping)?$/i, "").replace(/[-_ ]?Shipping$/i, "").replace(/([a-z])Client$/, "$1").replace(/[-_]+/g, " ").trim();
    return name || app || "Other";
  }
  const pagerIcons = { back: '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="m12 5-5 5 5 5"/></svg>', next: '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="m8 5 5 5-5 5"/></svg>' };
  function renderPager(container, page, pages, first, last, total) {
    container.hidden = pages < 2;
    container.replaceChildren();
    if (pages < 2) return;
    const step = (label, icon, target, disabled) => {
      const button = document.createElement("button");
      button.type = "button"; button.className = "icon-button"; button.innerHTML = icon; button.disabled = disabled;
      button.setAttribute("aria-label", label);
      button.addEventListener("click", () => showClipPage(target));
      return button;
    };
    const status = document.createElement("span");
    status.textContent = `${first}–${last} of ${total}`;
    container.append(step("Previous page", pagerIcons.back, page - 1, page === 0), status, step("Next page", pagerIcons.next, page + 1, page >= pages - 1));
  }
  function renderClipFilters(items) {
    const counts = new Map();
    for (const clip of items) { const key = clip.app || ""; counts.set(key, (counts.get(key) || 0) + 1); }
    if (clipView.app !== null && !counts.has(clipView.app)) clipView.app = null;
    const filters = $("clips-filters");
    filters.replaceChildren();
    if (counts.size > 1) for (const [key, count] of [[null, items.length], ...[...counts].sort((a, b) => b[1] - a[1])]) {
      const button = document.createElement("button");
      button.type = "button";
      button.setAttribute("aria-pressed", String(clipView.app === key));
      const total = document.createElement("span");
      total.textContent = String(count);
      button.append(key === null ? "All" : gameName(key), total);
      button.addEventListener("click", () => { if (clipView.app === key) return; clipView.app = key; clipView.page = 0; renderClipLists(); cascade($("clips-list")); });
      filters.append(button);
    }
    return clipView.app === null ? items : items.filter(clip => (clip.app || "") === clipView.app);
  }
  function showClipPage(page) {
    clipView.page = page;
    renderClipLists();
    $("main").scrollTop = 0;
    cascade($("clips-list"));
  }
  function cascade(container, wait = 0, opening = false) {
    if (!presented || !motionEnabled()) return;
    [...container.children].slice(0, 12).forEach((row, index) => {
      const animation = row.animate([{ opacity: 0, transform: `translateY(${opening ? 8 : 4}px)` }, { opacity: 1, transform: "translateY(0)" }], { duration: opening ? 340 : 200, delay: wait + index * (opening ? 34 : 24), easing: "cubic-bezier(.2,.8,.2,1)", fill: "backwards" });
      entranceAnimations.add(animation);
      animation.finished.catch(() => {}).finally(() => entranceAnimations.delete(animation));
    });
  }
  const capturing = () => Boolean(state.snapshot?.replay_active && state.snapshot?.connected && state.snapshot?.owned);
  const emptyCopy = {
    replays: { title: "No replays saved yet", text: "Start capture, then press your save shortcut. Open your clips folder to find older files.", short: "No replays saved yet. Start capture, then press your save shortcut." },
    edits: { title: "No edited exports yet", text: "Select Edit on a replay, cut it down, then Export video. Exports are saved in an Edited Exports folder beside the original.", short: "No edited exports yet. Select Edit on a replay, then Export video." }
  };
  function renderClipLists(clips = Array.isArray(state.snapshot?.clips) ? state.snapshot.clips : []) {
    thumbnailObserver.disconnect();
    const groups = { replays: clips.filter(clip => !clip.edited), edits: clips.filter(clip => clip.edited) };
    for (const button of document.querySelectorAll("[data-clip-tab]")) {
      const selected = clipTabs[button.dataset.clipList] === button.dataset.clipTab;
      button.setAttribute("aria-selected", String(selected));
      button.tabIndex = selected ? 0 : -1;
      const count = groups[button.dataset.clipTab].length;
      const total = button.querySelector(".tab-count");
      if (total) total.textContent = count ? String(count) : "";
    }
    for (let [list, container, limit, compact] of [["clips", $("clips-list"), pageSize, false], ["latest", $("latest-content"), 8, true]]) {
      const tab = clipTabs[list];
      let items = groups[tab], start = 0;
      if (!compact) {
        const all = items.length;
        items = renderClipFilters(items);
        const pages = Math.max(1, Math.ceil(items.length / pageSize));
        clipView.page = Math.min(Math.max(0, clipView.page), pages - 1);
        start = clipView.page * pageSize;
        const last = Math.min(items.length, start + pageSize);
        for (const pager of [$("clips-pager-top"), $("clips-pager-bottom")]) renderPager(pager, clipView.page, pages, start + 1, last, items.length);
        $("clips-toolbar").hidden = !all || ($("clips-filters").childElementCount === 0 && pages < 2);
        limit = pageSize;
      }
      container.setAttribute("aria-labelledby", `${list}-tab-${tab}`);
      container.replaceChildren();
      container.className = items.length ? "clips-grid" : "clips-list";
      if (items.length) for (const clip of items.slice(start, start + limit)) container.append(clipRow(clip, compact));
      else {
        const empty = document.createElement("div");
        empty.className = "clips-empty";
        const title = document.createElement("h2"); title.textContent = emptyCopy[tab].title;
        const description = document.createElement("p"); description.textContent = tab === "replays" && capturing() ? `Capture is running. Press ${settings().save_hotkey} while you play to save the last ${formatDuration(settings().clip_seconds)}.` : emptyCopy[tab].text;
        empty.append(title, description);
        container.append(empty);
      }
    }
    syncTabIndicators();
    scheduleFade();
  }
  function showClipTab(list, tab, focus = false) {
    if (!["replays", "edits"].includes(tab)) return;
    const changed = clipTabs[list] !== tab;
    clipTabs[list] = tab;
    if (changed && list === "clips") { clipView.app = null; clipView.page = 0; }
    try { localStorage.setItem("obsreplay.clipTabs", JSON.stringify(clipTabs)); } catch {}
    renderClipLists();
    if (focus) $(`${list}-tab-${tab}`).focus();
    const container = list === "clips" ? $("clips-list") : $("latest-content");
    if (changed) cascade(container);
  }
  async function dispatch(action, payload = {}) {
    if (busy()) return;
    cancelRecording();
    state.localError = "";
    state.dismissedError = "";
    state.pending = { action, revision: state.snapshot?.revision, started: Date.now() };
    renderFeedback();
    try {
      const result = unpack(await nativeCall("dispatch", action, JSON.stringify(payload)));
      if (result === false || result?.error || result?.ok === false) throw new Error(errorText(result?.error) || "OBSReplay couldn’t start that action. Try again.");
      state.pending.accepted = true;
      schedulePoll(0);
    } catch (error) {
      state.pending = null;
      showError(error);
      renderFeedback();
    }
  }
  async function poll() {
    clearTimeout(pollTimer);
    if (polling) { schedulePoll(500); return; }
    if (document.hidden && !state.pending) return;
    polling = true;
    try {
      const snapshot = unpack(await nativeCall("snapshot"));
      if (!snapshot || typeof snapshot !== "object" || !snapshot.settings) throw new Error("OBSReplay couldn’t read its state. Reopen the app if this continues.");
      if (state.native && state.loaded && !state.pending && snapshot.revision === state.snapshot?.revision && snapshot.hotkey_error === state.snapshot?.hotkey_error) {
        polling = false;
        schedulePoll(snapshot.busy ? 500 : 1000);
        return;
      }
      if (!state.native && typeof api().app_info === "function") Promise.resolve(api().app_info()).then(value => { const info = unpack(value); $("app-version").textContent = `v${info.version}`; $("app-version").title = `OBSReplay ${info.full}`; }).catch(() => {});
      if (!state.native) readUpdate();
      if (!state.native && typeof api().installed === "function") api().installed().then(value => { $("uninstall-row").hidden = !value; }).catch(() => {});
      state.native = true;
      const previousObs = state.snapshot?.settings?.obs_path;
      const lackedObs = state.snapshot ? !state.snapshot.obs_supported : false;
      state.snapshot = snapshot;
      if (diskResample) { diskResample = false; diskSeenClip = newestReplay(); }
      if (state.loaded && previousObs !== snapshot.settings.obs_path) $("obs_path").value = snapshot.settings.obs_path || "";
      syncMotion();
      renderOptions();
      if (!state.loaded) { fillForm(snapshot.settings); state.loaded = true; }
      else if (!state.dirty && !state.pending && state.recording === null) fillForm(snapshot.settings);
      if (state.pending?.accepted) {
        const changed = snapshot.revision !== state.pending.revision;
        if (snapshot.busy) state.pending.sawBusy = true;
        if (!snapshot.busy && (changed || state.pending.sawBusy)) {
          const action = state.pending.action;
          state.pending = null;
          if (action === "setup") {
            setup.flow = snapshot.error ? "failed" : "done";
            // A finished setup shows its checks for a moment, then opens the Capture page on its own.
            if (!snapshot.error && state.tab === "setup") setTimeout(finishSetup, 1100);
          }
          if (action === "install_obs") {
            // FFmpeg follows OBS, also when the OBS install failed, and finishes in the background while setup moves on.
            installFfmpegIfWanted();
            if (!snapshot.error && snapshot.obs_supported && setup.step === "obs") goStep("capture");
          }
          if (action === "locate_obs" && !snapshot.error && snapshot.obs_supported && setup.step === "obs" && state.ffmpeg?.status === "ready") goStep("capture");
          if (!snapshot.error && ["setup", "apply", "install_obs", "locate_obs"].includes(action)) fillForm(snapshot.settings);
        } else if (Date.now() - state.pending.started > 120000) {
          state.pending = null;
          showError("OBSReplay hasn’t heard back from OBS. Check that OBS is open, then try again. A replay save is never retried on its own.");
        }
      }
      // OBS Studio installed by hand while step 1 waits: carry on without a click when nothing else is missing.
      if (lackedObs && snapshot.obs_supported && !state.pending && state.tab === "setup" && setup.step === "obs" && state.ffmpeg?.status === "ready") goStep("capture");
      if (!state.initialRoute && !snapshot.busy) {
        state.initialRoute = true;
        if (!configured()) {
          // Setup opens on step 1 unless OBS Studio and FFmpeg are both already there.
          const tool = await settledFfmpeg();
          setup.step = snapshot.obs_supported && (!tool || tool.status === "ready") ? "capture" : "obs";
          showTab("setup");
          if (setup.step === "obs") pollFfmpeg();
        }
        markRouted();
      }
      // The saved theme is shared with every window; a choice still waiting for Apply stays a preview here.
      // The saved theme is always recorded for the other windows, even if this form still has unapplied edits.
      Theme.save(settings().theme);
      if (!state.dirty && !state.pending) Theme.set(settings().theme);
      document.documentElement.dataset.loaded = "";
      renderCapture();
      renderClips();
      renderFeedback();
    } catch (error) {
      state.native = false;
      markRouted();
      document.documentElement.dataset.loaded = "";
      if (state.loaded || state.pending) showError(error);
      state.pending = null;
      renderCapture();
      renderFeedback();
    }
    polling = false;
    schedulePoll(state.pending || state.snapshot?.busy ? 500 : 1000);
  }
  function schedulePoll(delay) { clearTimeout(pollTimer); if (!document.hidden || state.pending) pollTimer = setTimeout(poll, delay); }
  async function confirmAction(title, message, label, destructive = false) {
    $("confirm-title").textContent = title;
    $("confirm-text").textContent = message;
    $("confirm-action").textContent = label;
    $("confirm-action").classList.toggle("danger", destructive);
    $("confirm-action").classList.toggle("primary", !destructive);
    $("confirm-dialog").returnValue = "cancel";
    dialogClosing = false;
    $("confirm-dialog").showModal();
    if (destructive) $("confirm-dialog").querySelector('[value="cancel"]').focus();
    animateDialog($("confirm-dialog"), true);
    return new Promise(resolve => $("confirm-dialog").addEventListener("close", () => resolve($("confirm-dialog").returnValue === "confirm"), { once: true }));
  }
  function closeDialog(value) {
    if (dialogClosing) return;
    const dialog = $("confirm-dialog");
    if (!motionEnabled()) { dialog.close(value); return; }
    dialogClosing = true;
    dialog.classList.add("closing");
    animateDialog(dialog, false).then(() => { dialog.classList.remove("closing"); for (const animation of dialog.getAnimations()) animation.cancel(); if (dialog.open) dialog.close(value); dialogClosing = false; });
  }
  async function nativeWindowAction(action) {
    try { await nativeCall(action); } catch (error) { showError(error); }
  }
  function cancelRecording() {
    if (!state.recording) return;
    clearTimeout(recorderTimer);
    $(state.recording.id).value = state.recording.previous;
    $(state.recording.id).classList.remove("recording");
    state.recording = null;
    $("hotkey-message").textContent = "Shortcut change canceled.";
    nativeCall("hotkey_recording", false).catch(showError);
  }
  // Guided setup: install OBS if needed, choose the essentials, then set up and start in one step.
  const setup = { step: null, flow: null, closing: false, wantFfmpeg: false };
  const setupSteps = ["obs", "capture", "start"];
  const setupTasks = ["closing", "configuring", "launching", "starting"];
  const checkIcon = '<svg viewBox="0 0 12 12"><path d="m2.5 6.2 2.3 2.3 4.7-5"/></svg>';
  const crossIcon = '<svg viewBox="0 0 12 12"><path d="m3 3 6 6m0-6-6 6"/></svg>';
  const setupBindings = [["setup-monitor", "monitor_id"], ["setup-length", "clip_seconds"], ["setup-quality", "quality"], ["setup-mic", "mic_enabled"], ["setup-mic-device", "mic_device"], ["setup-folder", "clips_dir"], ["setup-start-login", "start_at_login"]];
  function operating(name) { return state.pending?.action === name || Boolean(state.snapshot?.busy && state.snapshot?.operation === name); }
  function mirrorOptions(source, target) {
    const key = [...source.options].map(option => `${option.value}\u0000${option.textContent}`).join("\u0001");
    if (target.dataset.mirror !== key) {
      target.replaceChildren(...[...source.options].map(option => option.cloneNode(true)));
      target.dataset.mirror = key;
    }
    if (target.value !== source.value) target.value = source.value;
  }
  function syncSetupControls() {
    for (const [id, field] of setupBindings) {
      const control = $(id), source = $(field);
      if (control.type === "checkbox") control.checked = source.checked;
      else if (control.tagName === "SELECT") mirrorOptions(source, control);
      else if (control.value !== source.value && document.activeElement !== control) control.value = source.value;
    }
    for (const radio of document.querySelectorAll('[name="setup-capture"]')) radio.checked = radio.value === $("capture").value;
    $("setup-monitor-row").hidden = $("capture").value === "game";
    // The device choice sits beside the switch and shows only while the microphone is recorded.
    const micChoice = $("setup-mic-device").closest(".dropdown");
    if (micChoice) micChoice.hidden = !$("mic_enabled").checked;
    const auto = state.snapshot?.options?.encoders?.[0]?.label || "";
    const encoder = auto.replace(/^Automatic · /, "");
    $("setup-defaults").textContent = `${encoder && !/CPU/.test(encoder) ? `Encoding uses ${encoder} automatically. ` : encoder ? "No supported GPU encoder was found, so encoding uses the CPU. " : ""}You can change frame rate, audio devices and shortcuts in Settings.`;
  }
  // FFmpeg, as the native side found it. Step 1 of setup shows it beside OBS Studio and installs whichever is missing.
  let ffmpegTimer = 0;
  const ffmpegBusy = () => state.ffmpeg?.status === "checking" || Boolean(state.ffmpeg?.install?.stage);
  async function readFfmpeg(refresh = false) {
    if (typeof api()?.ffmpeg_state !== "function") return null;
    try { return unpack(await api().ffmpeg_state(refresh)); } catch { return null; }
  }
  async function pollFfmpeg(refresh = false) {
    clearTimeout(ffmpegTimer);
    const next = await readFfmpeg(refresh);
    // Looking again while FFmpeg is known to be missing reports "checking" for a moment. The page keeps showing
    // "missing" with its install button until the new answer is in, and polls quickly meanwhile.
    const rechecking = next?.status === "checking" && ["missing", "invalid"].includes(state.ffmpeg?.status);
    if (rechecking) next.status = state.ffmpeg.status;
    if (next && JSON.stringify(next) !== JSON.stringify(state.ffmpeg)) { state.ffmpeg = next; renderFeedback(); }
    // An install asked for while a check was still running starts as soon as the answer is known.
    if (!rechecking) installFfmpegIfWanted();
    const watching = state.tab === "setup" && setup.step === "obs" && !document.hidden;
    if (ffmpegBusy() || rechecking || watching) ffmpegTimer = setTimeout(pollFfmpeg, ffmpegBusy() || rechecking ? 400 : 3000);
  }
  // The first definite answer (found or missing), waited for briefly so setup can open on the right step.
  async function settledFfmpeg() {
    for (let attempt = 0; attempt < 20; ++attempt) {
      const tool = await readFfmpeg(attempt === 0);
      if (!tool) return null;
      state.ffmpeg = tool;
      if (tool.status !== "checking") return tool;
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    return state.ffmpeg;
  }
  // One button installs what is missing: OBS Studio first, then FFmpeg, so two winget runs never overlap.
  function installMissing() {
    setup.wantFfmpeg = true;
    if (!state.snapshot?.obs_supported && state.snapshot?.winget) dispatch("install_obs");
    else installFfmpegIfWanted();
  }
  function installFfmpegIfWanted() {
    const tool = state.ffmpeg;
    if (!setup.wantFfmpeg || !tool || tool.status === "checking") return;
    // Already there, already installing, or not installable here: the request is settled.
    if (tool.status !== "missing" && tool.status !== "invalid" || tool.install?.stage || !tool.install?.available) { if (tool.status === "ready" || !tool.install?.available) setup.wantFfmpeg = false; return; }
    setup.wantFfmpeg = false;
    // Read the state at once and again shortly after, so progress shows without waiting for the slow idle poll.
    api().install_ffmpeg().then(() => { pollFfmpeg(); setTimeout(pollFfmpeg, 350); }).catch(showError);
  }
  function setTask(item, status) {
    if (item.dataset.state === status) return;
    item.dataset.state = status;
    item.querySelector(".task-mark").innerHTML = status === "done" ? checkIcon : status === "failed" ? crossIcon : "";
  }
  function goStep(step) {
    if (setup.step === step) return;
    setup.step = step;
    // Moving between steps dismisses an error from the previous one; a new action shows its own.
    const shown = [state.localError, errorText(state.snapshot?.error)].find(Boolean);
    if (shown) { state.dismissedError = shown; state.localError = ""; }
    renderFeedback();
    $("main").scrollTop = 0;
    if (step === "obs") pollFfmpeg(true);
    const panel = document.querySelector(`[data-step-panel="${step}"]`);
    if (presented && panel) playEntrance(panel);
    const focusTarget = step === "capture" ? document.querySelector('[name="setup-capture"]:checked') : step === "obs" && !$("install-obs").hidden ? $("install-obs") : null;
    focusTarget?.focus({ preventScroll: true });
  }
  function runSetup(value) {
    if (busy()) return;
    if (!value.obs_path) value.obs_path = state.snapshot?.settings?.obs_path || "";
    if (!value.clips_dir) { goStep("capture"); showTab("setup"); showError("Choose a clips folder for your replays."); $("setup-folder").focus(); return; }
    if (value.save_hotkey.toLowerCase() === value.show_hotkey.toLowerCase()) { showError("Save replay and Show / hide window need different shortcuts."); return; }
    setup.flow = "running";
    setup.closing = false;
    for (const item of $("setup-tasks").children) setTask(item, "pending");
    showTab("setup");
    goStep("start");
    dispatch("setup", { settings: value, start: true });
  }
  function setupFooterText() {
    const snapshot = state.snapshot || {};
    if (busy()) return snapshot.message || "Working…";
    if (setup.step === "obs") return snapshot.obs_supported ? `OBS Studio ${snapshot.obs_version} is ready` : "OBS Studio is needed for capture";
    if (setup.step === "capture") return state.ffmpeg?.install?.stage ? "Step 2 of 3. FFmpeg is installing in the background." : state.ffmpeg?.install?.error ? "Step 2 of 3. FFmpeg wasn’t installed. Go back to Install to try again." : "Step 2 of 3";
    if (setup.flow === "done") return "Replay capture is running";
    return snapshot.message || "";
  }
  function renderSetup() {
    const snapshot = state.snapshot;
    const visible = state.tab === "setup";
    $("setup-actions").hidden = !visible;
    if (!visible || !snapshot) return;
    if (!setup.step) setup.step = snapshot.obs_supported && state.ffmpeg?.status !== "missing" ? "capture" : "obs";
    const pending = busy();
    const installing = operating("install_obs");
    const running = operating("setup");
    const obsReady = Boolean(snapshot.obs_supported);
    const failedError = [state.localError, errorText(snapshot.error)].find(Boolean) || "";
    // Steps
    const reached = { obs: true, capture: obsReady, start: setup.flow !== null };
    const complete = { obs: obsReady, capture: setup.flow !== null, start: setup.flow === "done" };
    for (const button of document.querySelectorAll("[data-setup-step]")) {
      const name = button.dataset.setupStep;
      const current = name === setup.step;
      if (current) button.setAttribute("aria-current", "step"); else button.removeAttribute("aria-current");
      const finished = complete[name] && (!current || setup.flow === "done");
      button.dataset.state = finished ? "done" : "";
      const mark = button.querySelector(".step-mark");
      const markup = finished ? checkIcon : String(setupSteps.indexOf(name) + 1);
      if (mark.dataset.markup !== markup) { mark.innerHTML = markup; mark.dataset.markup = markup; }
      button.disabled = !state.native || pending || !reached[name] || (setup.flow === "done" && name !== "start");
    }
    for (const panel of document.querySelectorAll("[data-step-panel]")) panel.hidden = panel.dataset.stepPanel !== setup.step;
    // Step 1: OBS Studio and FFmpeg
    const version = snapshot.obs_version ? `OBS Studio ${snapshot.obs_version}` : "OBS Studio";
    const tool = state.ffmpeg || { status: typeof api()?.ffmpeg_state === "function" ? "checking" : "ready", install: {} };
    const ffmpegReady = tool.status === "ready";
    const ffmpegInstalling = Boolean(tool.install?.stage);
    const ffmpegMissing = tool.status === "missing" || tool.status === "invalid";
    const canInstallFfmpeg = ffmpegMissing && Boolean(tool.install?.available);
    const stageText = { preparing: "Getting ready…", downloading: "Downloading…", installing: "Installing…", verifying: "Checking…" };
    const obsRow = $("requirements").querySelector('[data-requirement="obs"]'), ffmpegRow = $("requirements").querySelector('[data-requirement="ffmpeg"]');
    setTask(obsRow, installing ? "active" : obsReady ? "done" : failedError ? "failed" : "pending");
    $("requirement-obs").textContent = installing ? (snapshot.install_stage === "installing" ? "Installing. Windows asks for permission." : stageText[snapshot.install_stage] || stageText.preparing) : obsReady ? `Version ${snapshot.obs_version} is ready` : snapshot.obs_found ? `Version ${snapshot.obs_version} is too old` : "Not installed";
    setTask(ffmpegRow, ffmpegInstalling ? "active" : ffmpegReady ? "done" : tool.install?.error ? "failed" : "pending");
    $("requirement-ffmpeg").textContent = ffmpegInstalling ? stageText[tool.install.stage] || stageText.preparing : ffmpegReady ? "Ready" : tool.status === "checking" ? "Checking…" : tool.install?.error ? "Not installed. You can add it later from the editor." : "Not installed. Exporting edited clips needs it.";
    let obsTitle, obsText, mark = "";
    if (installing) {
      obsTitle = snapshot.obs_found ? "Updating OBS Studio…" : "Installing OBS Studio…";
      obsText = snapshot.install_stage === "installing" ? "Approve the Windows permission prompt if it appears. Setup continues on its own." : "This usually takes a minute or two. You can leave this window open.";
      mark = "working";
    } else if (obsReady && ffmpegInstalling) {
      obsTitle = "Installing FFmpeg…";
      obsText = "You can continue. FFmpeg finishes in the background.";
      mark = "working";
    } else if (obsReady && ffmpegMissing) {
      obsTitle = tool.install?.error ? "FFmpeg wasn’t installed" : "FFmpeg is missing";
      obsText = tool.install?.error || "Capture works without it. You need it to export clips you trim in the editor.";
      mark = "warning";
    } else if (obsReady) {
      obsTitle = ffmpegReady ? "Everything is installed" : `${version} is ready`;
      obsText = snapshot.obs_running ? "OBS is open right now. It closes and reopens on its own when you start." : "OBSReplay opens it in the tray whenever capture runs.";
      mark = "active";
    } else if (snapshot.obs_found) {
      obsTitle = `${version} needs an update`;
      obsText = "OBSReplay needs OBS Studio 31 or newer. Updating keeps your existing OBS profiles and scenes.";
      mark = "warning";
    } else {
      obsTitle = canInstallFfmpeg && snapshot.winget ? "Install OBS Studio and FFmpeg" : "Install OBS Studio";
      obsText = snapshot.winget ? "OBSReplay uses OBS Studio to capture and encode your replays. Installing takes a minute or two, and Windows asks for permission once." :"Automatic install isn’t available on this PC. Download OBS Studio from obsproject.com and run its installer; setup continues as soon as it’s installed.";
    }
    if (!installing && !obsReady && failedError) mark = "failed";
    $("obs-title").textContent = obsTitle;
    $("obs-text").textContent = obsText;
    $("obs-mark").className = `capture-mark${mark ? ` ${mark}` : ""}`;
    $("obs-path").hidden = !obsReady || installing;
    $("obs-path").textContent = snapshot.settings?.obs_path || "";
    const needObs = !obsReady;
    $("obs-actions").hidden = installing || (!needObs && (ffmpegInstalling || !ffmpegMissing));
    $("install-obs").hidden = needObs ? !snapshot.winget : !canInstallFfmpeg;
    $("install-obs").textContent = !needObs ? "Install FFmpeg" : `${snapshot.obs_found ? "Update OBS Studio" : "Install OBS Studio"}${canInstallFfmpeg ? " and FFmpeg" : ""}`;
    $("download-obs").hidden = $("locate-obs").hidden = !needObs;
    $("download-obs").className = snapshot.winget ? "" : "primary";
    $("download-obs").textContent = snapshot.winget ? "Download from obsproject.com" : "Download OBS Studio";
    // Manual ways to add FFmpeg, offered once OBS Studio is in so step 1 never shows five buttons.
    $("download-ffmpeg").hidden = $("locate-ffmpeg").hidden = needObs || !ffmpegMissing;
    for (const id of ["install-obs", "download-obs", "locate-obs", "download-ffmpeg", "locate-ffmpeg"]) $(id).disabled = !state.native || pending;
    if (ffmpegInstalling) $("install-obs").disabled = true;
    // Step 2: capture essentials
    for (const element of document.querySelectorAll('[data-step-panel="capture"] input, [data-step-panel="capture"] select, [data-step-panel="capture"] button')) element.disabled = !state.native || pending;
    if (!pending) $("setup-monitor").disabled = $("capture").value === "game";
    // Step 3: start
    const stage = snapshot.setup_stage || "";
    if (running && stage === "closing") setup.closing = true;
    $("setup-tasks").querySelector('[data-task="closing"]').hidden = !setup.closing;
    const stageIndex = stage === "done" ? setupTasks.length : setupTasks.indexOf(stage);
    const tasks = [...$("setup-tasks").children];
    tasks.forEach((item, i) => {
      if (setup.flow === "done") setTask(item, "done");
      else if (setup.flow === "failed") setTask(item, i < stageIndex ? "done" : i === Math.max(stageIndex, 0) ? "failed" : "pending");
      else if (setup.flow === "running") setTask(item, stageIndex < 0 ? "pending" : i < stageIndex ? "done" : i === stageIndex ? "active" : "pending");
    });
    const value = settings();
    if (setup.flow === "done") {
      $("start-title").textContent = "Replay capture is running";
      $("start-text").innerHTML = "";
      const keys = document.createElement("kbd");
      keys.textContent = value.save_hotkey;
      $("start-text").append("Press ", keys, ` to save the last ${formatDuration(value.clip_seconds)}. Closing this window keeps OBSReplay running in the tray.`);
      $("start-text").classList.add("setup-done");
    } else {
      $("start-text").classList.remove("setup-done");
      $("start-title").textContent = setup.flow === "failed" ? "Setup needs attention" : "Setting up…";
      $("start-text").textContent = setup.flow === "failed" ? (configured() ? "Your settings were saved. Check the problem above, then try again." : "Nothing was started. Check the problem above, then try again.") : "OBS closes and reopens on its own. This takes a few seconds.";
    }
    $("start-mark").className = `capture-mark ${setup.flow === "done" ? "active" : setup.flow === "failed" ? "failed" : "working"}`;
    $("start-hint").hidden = setup.flow !== "done" && setup.flow !== "running";
    // Inline error
    const showSetupError = Boolean(failedError) && failedError !== state.dismissedError && !installing && !running;
    $("setup-error").hidden = !showSetupError;
    $("setup-error-title").textContent = setup.step === "obs" ? "OBS Studio isn’t ready" : "Setup couldn’t finish";
    $("setup-error-text").textContent = showSetupError ? failedError : "";
    const setupFix = showSetupError ? errorFix(failedError) : null;
    $("setup-error-fix").hidden = !setupFix;
    $("setup-error-fix").textContent = setupFix?.label || "";
    $("setup-error-fix").dataset.error = setupFix ? failedError : "";
    $("setup-error-fix").disabled = !state.native || pending;
    // Footer actions
    const back = $("setup-back"), next = $("setup-next");
    back.hidden = setup.step === "obs" || setup.flow === "done" || running;
    next.hidden = running || setup.flow === "done" || (setup.step === "obs" && !obsReady);
    back.disabled = next.disabled = !state.native || pending;
    if (setup.step === "obs") next.textContent = "Continue";
    else if (setup.step === "capture") next.textContent = "Set up and start capture";
    else next.textContent = "Try again";
  }
  function setupNext() {
    if (setup.step === "obs") goStep("capture");
    else if (setup.step === "capture" || setup.flow === "failed") runSetup(formValue());
    else finishSetup();
  }
  function finishSetup() {
    if (setup.flow !== "done") return;
    setup.flow = null; setup.step = null;
    if (state.tab === "setup") showTab("capture");
  }
  function setupBack() {
    if (setup.step === "capture") goStep("obs");
    else if (setup.step === "start") { setup.flow = null; goStep("capture"); }
  }
  function bindSetup() {
    $("setup-next").addEventListener("click", setupNext);
    for (const id of ["setup-error-fix", "error-fix"]) $(id).addEventListener("click", () => applyErrorFix($(id).dataset.error));
    $("setup-back").addEventListener("click", setupBack);
    for (const button of document.querySelectorAll("[data-setup-step]")) button.addEventListener("click", () => goStep(button.dataset.setupStep));
    $("install-obs").addEventListener("click", installMissing);
    window.addEventListener("focus", () => { if (state.tab === "setup" && setup.step === "obs") pollFfmpeg(true); });
    $("download-obs").addEventListener("click", () => dispatch("help", { topic: "obs" }));
    $("download-ffmpeg").addEventListener("click", () => dispatch("help", { topic: "ffmpeg" }));
    $("locate-ffmpeg").addEventListener("click", async () => {
      try { if (await nativeCall("pick_ffmpeg")) pollFfmpeg(); } catch (error) { showError(error); }
    });
    $("locate-obs").addEventListener("click", async () => {
      try {
        const path = await nativeCall("pick_obs", state.snapshot?.settings?.obs_path || "");
        if (path) dispatch("locate_obs", { path });
      } catch (error) { showError(error); }
    });
    $("setup-all-settings").addEventListener("click", () => { showTab("settings"); showCategory("capture", false); });
    $("open-guided-setup").addEventListener("click", () => showTab("setup"));
    for (const [id, field] of setupBindings) $(id).addEventListener("input", () => {
      const control = $(id), target = $(field);
      if (control.type === "checkbox") target.checked = control.checked; else target.value = control.value;
      if (field === "start_at_login") $("setup-start-at-login").checked = control.checked;
      markDirty();
      syncSetupControls();
      syncDropdowns();
    });
    for (const radio of document.querySelectorAll('[name="setup-capture"]')) radio.addEventListener("change", () => { $("capture").value = radio.value; markDirty(); renderSetup(); });
    $("setup-pick-folder").addEventListener("click", async () => {
      try {
        const result = await nativeCall("pick_folder", $("clips_dir").value);
        if (result) { $("clips_dir").value = result; markDirty(); showPathEnds(); }
      } catch (error) { showError(error); }
    });
  }

  // Dropdowns: each <select> keeps the value and its listeners; a button and a listbox replace the system menu.
  const dropdowns = [];
  let openDropdown = null;
  const chevron = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 9l6 6 6-6"/></svg>';
  function syncDropdowns() {
    for (const item of dropdowns) {
      const text = item.select.selectedOptions[0]?.textContent || "";
      if (item.label.textContent !== text) item.label.textContent = text;
      if (item.button.disabled !== item.select.disabled) item.button.disabled = item.select.disabled;
      if (item.select.disabled && openDropdown === item) closeDropdown();
    }
  }
  function closeDropdown(focus = false) {
    const item = openDropdown;
    if (!item) return;
    openDropdown = null;
    item.list.remove();
    item.button.setAttribute("aria-expanded", "false");
    item.button.removeAttribute("aria-activedescendant");
    if (focus) item.button.focus({ preventScroll: true });
  }
  function placeDropdown(item) {
    const box = item.button.getBoundingClientRect(), gap = 6, edge = 8;
    const wanted = Math.min(item.list.scrollHeight + 2, 320);
    const below = innerHeight - box.bottom - gap - edge, above = box.top - gap - edge;
    const up = below < Math.min(wanted, 160) && above > below;
    const height = Math.max(64, Math.min(wanted, up ? above : below));
    item.list.classList.toggle("above", up);
    // At least as wide as the button, wider when an option needs it, and kept inside the window from the button's right edge.
    Object.assign(item.list.style, { left: "0px", minWidth: `${box.width}px`, maxWidth: `${Math.min(460, innerWidth - edge * 2)}px`, maxHeight: `${height}px`, top: up ? "" : `${box.bottom + gap}px`, bottom: up ? `${innerHeight - box.top + gap}px` : "" });
    const width = item.list.offsetWidth;
    item.list.style.left = `${Math.max(edge, width > box.width ? Math.min(box.left, box.right - width) : box.left)}px`;
  }
  function activateOption(item, index, scroll = true) {
    const rows = [...item.list.children];
    if (!rows[index] || rows[index].getAttribute("aria-disabled") === "true") return;
    rows.forEach((row, i) => row.classList.toggle("active", i === index));
    item.active = index;
    item.button.setAttribute("aria-activedescendant", rows[index].id);
    if (scroll) rows[index].scrollIntoView({ block: "nearest" });
  }
  function moveOption(item, step, from = item.active) {
    const rows = [...item.list.children];
    for (let index = from + step; index >= 0 && index < rows.length; index += step) if (rows[index].getAttribute("aria-disabled") !== "true") { activateOption(item, index); return; }
  }
  function chooseOption(item, index) {
    const option = item.select.options[index];
    if (!option || option.disabled) return;
    closeDropdown(true);
    if (item.select.selectedIndex === index) return;
    item.select.selectedIndex = index;
    item.select.dispatchEvent(new Event("input", { bubbles: true }));
    item.select.dispatchEvent(new Event("change", { bubbles: true }));
    syncDropdowns();
  }
  function openDropdownFor(item) {
    closeDropdown();
    if (item.select.disabled || !item.select.options.length) return;
    item.list.replaceChildren(...[...item.select.options].map((option, index) => {
      const row = document.createElement("li");
      row.id = `${item.list.id}-${index}`;
      row.setAttribute("role", "option");
      row.setAttribute("aria-selected", String(index === item.select.selectedIndex));
      if (option.disabled) row.setAttribute("aria-disabled", "true");
      const text = document.createElement("span");
      text.textContent = option.textContent;
      row.append(text);
      row.insertAdjacentHTML("beforeend", checkIcon);
      row.addEventListener("pointermove", () => { if (item.active !== index) activateOption(item, index, false); });
      row.addEventListener("click", () => chooseOption(item, index));
      return row;
    }));
    document.body.append(item.list);
    openDropdown = item;
    item.button.setAttribute("aria-expanded", "true");
    placeDropdown(item);
    activateOption(item, Math.max(item.select.selectedIndex, 0));
  }
  function buildDropdowns() {
    for (const select of document.querySelectorAll("select")) {
      const wrap = document.createElement("div");
      wrap.className = "dropdown";
      if (select.classList.contains("setting-control")) { wrap.classList.add("setting-control"); select.classList.remove("setting-control"); }
      const button = document.createElement("button");
      button.type = "button";
      button.className = "dropdown-button";
      button.setAttribute("aria-haspopup", "listbox");
      button.setAttribute("aria-expanded", "false");
      const label = document.createElement("span");
      label.id = `${select.id}-value`;
      button.append(label);
      button.insertAdjacentHTML("beforeend", chevron);
      const list = document.createElement("ul");
      list.className = "dropdown-list";
      list.id = `${select.id}-list`;
      list.setAttribute("role", "listbox");
      const caption = document.querySelector(`label[for="${select.id}"]`);
      if (!caption && select.getAttribute("aria-label")) button.setAttribute("aria-label", select.getAttribute("aria-label"));
      if (caption) { caption.id ||= `${select.id}-label`; button.setAttribute("aria-labelledby", `${caption.id} ${label.id}`); list.setAttribute("aria-labelledby", caption.id); }
      button.setAttribute("aria-controls", list.id);
      select.before(wrap);
      wrap.append(select, button);
      select.tabIndex = -1;
      select.setAttribute("aria-hidden", "true");
      const item = { select, button, label, list, active: 0, typed: "", typedAt: 0 };
      dropdowns.push(item);
      // A label click lands on the unseen select; pass it to the button.
      select.addEventListener("focus", () => button.focus({ preventScroll: true }));
      select.addEventListener("change", syncDropdowns);
      list.addEventListener("pointerdown", event => event.preventDefault());
      button.addEventListener("click", () => { if (openDropdown === item) closeDropdown(); else openDropdownFor(item); });
      button.addEventListener("blur", () => { if (openDropdown === item) closeDropdown(); });
      button.addEventListener("keydown", event => {
        const open = openDropdown === item;
        if (event.key === "Escape" && open) { event.preventDefault(); event.stopPropagation(); closeDropdown(); return; }
        if (event.key === "Tab") { closeDropdown(); return; }
        if (["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) {
          event.preventDefault();
          if (!open) { openDropdownFor(item); return; }
          if (event.key === "ArrowDown") moveOption(item, 1);
          else if (event.key === "ArrowUp") moveOption(item, -1);
          else if (event.key === "Home") moveOption(item, 1, -1);
          else moveOption(item, -1, list.children.length);
          return;
        }
        if ((event.key === "Enter" || event.key === " ") && open) { event.preventDefault(); chooseOption(item, item.active); return; }
        if (event.key.length === 1 && event.key !== " " && !event.ctrlKey && !event.altKey && !event.metaKey) {
          // Typing jumps to the first option that starts with the typed letters.
          const now = Date.now();
          item.typed = (now - item.typedAt < 700 ? item.typed : "") + event.key.toLowerCase();
          item.typedAt = now;
          if (!open) openDropdownFor(item);
          const match = [...select.options].findIndex(option => !option.disabled && option.textContent.trim().toLowerCase().startsWith(item.typed));
          if (match >= 0) activateOption(item, match);
        }
      });
    }
    document.addEventListener("pointerdown", event => { if (openDropdown && !openDropdown.button.contains(event.target) && !openDropdown.list.contains(event.target)) closeDropdown(); }, true);
    $("main").addEventListener("scroll", () => closeDropdown(), { passive: true });
    window.addEventListener("resize", () => closeDropdown());
    window.addEventListener("blur", () => closeDropdown());
    syncDropdowns();
  }

  // Debug tab: what OBSReplay and OBS are using, refreshed every second while the tab is open, and the session log.
  const debugActive = () => state.tab === "settings" && category === "debug" && !document.hidden && typeof api()?.resources === "function";
  const megabytes = bytes => bytes == null ? "—" : bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(2)} GB` : `${Math.round(bytes / 1024 ** 2)} MB`;
  const percent = value => value == null ? "—" : `${Math.min(100, value).toFixed(1)}%`;
  function renderUsage(usage) {
    const app = usage.app, obs = usage.obs, running = Boolean(obs.running);
    const formats = { cpu: percent, memory: megabytes, gpu: percent, gpu_encode: percent, gpu_memory: megabytes, processes: value => value == null ? "—" : String(value) };
    for (const [key, format] of Object.entries(formats)) {
      const cells = $("debug-usage").querySelectorAll(`[data-usage="${key}"] td`);
      const both = app[key] == null && (!running || obs[key] == null) ? null : (app[key] || 0) + (running ? obs[key] || 0 : 0);
      cells[0].textContent = format(app[key]);
      // Memory and processes also say what the app holds once its window is hidden to the tray.
      if (usage.tray && (key === "memory" || key === "processes") && usage.tray[key] != null) {
        const note = document.createElement("small");
        note.className = "usage-note";
        // In the tray only OBSReplay.exe works; the interface's processes stay open but idle.
        note.textContent = key === "processes" ? `(1 active while in tray, ${Math.max(0, usage.tray[key] - 1)} idle)` : `(${format(usage.tray[key])} while in tray)`;
        cells[0].prepend(note);
      }
      cells[1].textContent = running ? format(obs[key]) : key === "cpu" ? "Not running" : "—";
      cells[2].textContent = format(both);
    }
    const stats = usage.obs_stats, number = value => Number.isFinite(Number(value)) ? Number(value) : 0;
    const share = (part, total) => `${number(part)}${number(total) ? ` (${(100 * number(part) / number(total)).toFixed(2)}%)` : ""}`;
    $("debug-fps").textContent = stats ? `${number(stats.activeFps).toFixed(1)} fps` : "—";
    $("debug-render").textContent = stats ? `${number(stats.averageFrameRenderTime).toFixed(2)} ms` : "—";
    $("debug-missed").textContent = stats ? share(stats.renderSkippedFrames, stats.renderTotalFrames) : "—";
    $("debug-skipped").textContent = stats ? share(stats.outputSkippedFrames, stats.outputTotalFrames) : "—";
    $("debug-disk-label").textContent = usage.disk?.drive ? `Clips drive (${usage.disk.drive.replace(/\\$/, "")})` : "Clips drive";
    $("debug-disk").textContent = usage.disk ? `${megabytes(usage.disk.free)} free` : "—";
  }
  async function debugTick() {
    clearTimeout(debugTimer);
    if (!debugActive()) return;
    let wait = 2000;
    try {
      if (!debugInfo && typeof api().app_info === "function") {
        debugInfo = unpack(await api().app_info());
        $("debug-version").textContent = `${debugInfo.version} · commit ${debugInfo.commit_short}${debugInfo.dirty ? " with local changes" : ""} · built ${debugInfo.built}`;
      }
      const usage = unpack(await api().resources());
      // CPU and GPU are measured between two readings, so the first one only starts the clock.
      if (usage.sampled && debugActive()) renderUsage(usage);
      if (debugTicks++ % 2 === 0 && typeof api().log_tail === "function") {
        const box = $("debug-log"), atEnd = box.scrollHeight - box.clientHeight - box.scrollTop < 8, text = String(await api().log_tail(200)).trimEnd();
        if (box.textContent !== text) { box.textContent = text; if (atEnd) box.scrollTop = box.scrollHeight; }
      }
      wait = usage.sampled ? 1000 : 350;
    } catch {}
    debugTimer = setTimeout(debugTick, wait);
  }
  function scheduleDebug() { clearTimeout(debugTimer); debugTimer = setTimeout(debugTick, 0); }

  // Updates. The native side checks GitHub and holds the state; this page shows it. A new version opens the dialog
  // once per run of the app: closing it asks again next launch, Skip hides that version for good.
  const update = { state: null, prompted: false, wanted: false, mode: "offer", timer: 0, notesKey: "", problem: "", toastTimer: 0, toastAction: null, toastWanted: false, closing: false };
  const updateReady = () => typeof api()?.update_state === "function";
  const megabyteCount = bytes => (bytes / 1024 ** 2).toFixed(1);
  const longDate = value => { const date = new Date(value); return Number.isNaN(date.getTime()) ? "" : new Intl.DateTimeFormat(undefined, { day: "numeric", month: "long", year: "numeric" }).format(date); };
  function showToast(text, actionLabel = "", action = null) {
    clearTimeout(update.toastTimer);
    $("toast-text").textContent = text;
    $("toast-action").hidden = !actionLabel;
    $("toast-action").textContent = actionLabel;
    update.toastAction = action;
    $("app-toast").hidden = false;
    update.toastTimer = setTimeout(() => { $("app-toast").hidden = true; }, 9000);
  }
  // Release notes are Markdown written on GitHub. They are drawn as plain elements: text is never read as HTML,
  // and only links to github.com are links.
  function renderNotes(box, text) {
    box.replaceChildren();
    const source = String(text || "").replace(/\r\n?/g, "\n").replace(/<!--[\s\S]*?-->/g, "").replace(/!\[([^\]]*)\]\([^)]*\)/g, "$1").replace(/<\/?[a-z][^>\n]*>/gi, "").trim();
    if (!source) { const empty = document.createElement("p"); empty.textContent = "This release has no notes."; box.append(empty); return; }
    const shortLink = url => { const pull = /\/(?:pull|issues)\/(\d+)$/.exec(url); if (pull) return `#${pull[1]}`; const compare = /\/compare\/(.+)$/.exec(url); return compare ? compare[1] : url.replace(/^https?:\/\//, ""); };
    const link = (label, url) => {
      if (!url.startsWith("https://github.com/")) return document.createTextNode(label === url ? url : `${label} (${url})`);
      const anchor = document.createElement("a");
      anchor.href = url;
      anchor.textContent = label;
      anchor.addEventListener("click", event => { event.preventDefault(); Promise.resolve(api()?.open_link?.(url)).catch(() => {}); });
      return anchor;
    };
    const inline = (parent, value) => {
      const pattern = /\*\*(.+?)\*\*|(`+)(.+?)\2|\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)|(https?:\/\/[^\s<>)\]]+)|\*(?=\S)(.+?)(?<=\S)\*/g;
      let last = 0, match;
      while ((match = pattern.exec(value))) {
        if (match.index > last) parent.append(value.slice(last, match.index));
        last = pattern.lastIndex;
        if (match[1] !== undefined) { const strong = document.createElement("strong"); inline(strong, match[1]); parent.append(strong); }
        else if (match[3] !== undefined) { const code = document.createElement("code"); code.textContent = match[3]; parent.append(code); }
        else if (match[5] !== undefined) parent.append(link(match[4], match[5]));
        else if (match[6] !== undefined) { const url = match[6].replace(/[.,;:!?]+$/, ""); parent.append(link(shortLink(url), url)); last -= match[6].length - url.length; pattern.lastIndex = last; }
        else { const em = document.createElement("em"); inline(em, match[7]); parent.append(em); }
      }
      if (last < value.length) parent.append(value.slice(last));
    };
    let paragraph = null, list = null, item = null;
    const lines = source.split("\n");
    for (let index = 0; index < lines.length; index++) {
      const line = lines[index];
      if (/^\s*```/.test(line)) {
        const code = [];
        for (index++; index < lines.length && !/^\s*```/.test(lines[index]); index++) code.push(lines[index]);
        const pre = document.createElement("pre"), inner = document.createElement("code");
        inner.textContent = code.join("\n");
        pre.append(inner); box.append(pre);
        paragraph = list = item = null;
        continue;
      }
      const heading = /^(#{1,6})\s+(.*?)\s*#*$/.exec(line), bullet = /^(\s*)[-*+]\s+(.*)$/.exec(line), numbered = /^(\s*)\d+[.)]\s+(.*)$/.exec(line), quote = /^>\s?(.*)$/.exec(line);
      if (!line.trim()) { paragraph = list = item = null; }
      else if (/^\s*(-{3,}|\*{3,}|_{3,})\s*$/.test(line)) { box.append(document.createElement("hr")); paragraph = list = item = null; }
      else if (heading) { const element = document.createElement(heading[1].length <= 2 ? "h3" : "h4"); inline(element, heading[2]); box.append(element); paragraph = list = item = null; }
      else if (bullet || numbered) {
        const found = bullet || numbered, tag = bullet ? "UL" : "OL";
        if (!list || list.tagName !== tag) { list = document.createElement(tag); box.append(list); }
        item = document.createElement("li");
        if (found[1].length >= 2) item.className = "nested";
        inline(item, found[2].replace(/^\[[ xX]\]\s+/, ""));
        list.append(item);
        paragraph = null;
      }
      else if (item && /^\s{2,}\S/.test(line)) { item.append(" "); inline(item, line.trim()); }
      else if (quote) { const block = document.createElement("blockquote"); inline(block, quote[1]); box.append(block); paragraph = list = item = null; }
      else {
        if (!paragraph) { paragraph = document.createElement("p"); box.append(paragraph); list = item = null; }
        else paragraph.append(" ");
        inline(paragraph, line.trim());
      }
    }
  }
  function renderUpdateRow(info) {
    const version = info.latest?.version, working = ["downloading", "verifying", "installing"].includes(info.status);
    let text = `You have version ${info.current}.`;
    if (info.status === "checking") text = "Checking for updates…";
    else if (working) text = `Updating to ${version}…`;
    else if (info.status === "error" && info.error_stage === "check") text = info.error;
    else if (info.available) text = `Version ${version} is available.${info.skipped ? " You skipped it." : ""}`;
    else if (info.checked_at) {
      const when = new Date(info.checked_at * 1000), today = when.toDateString() === new Date().toDateString();
      const time = new Intl.DateTimeFormat(undefined, { hour: "numeric", minute: "2-digit" }).format(when);
      text = `You have the latest version, ${info.current}. Last checked ${today ? `today at ${time}` : new Intl.DateTimeFormat(undefined, { day: "numeric", month: "long" }).format(when)}.`;
    }
    $("update-status").textContent = text;
    $("update-check").textContent = info.available ? "View update" : "Check now";
    $("update-check").disabled = info.status === "checking";
    $("update-auto").checked = info.enabled;
    document.querySelector('[data-switch-label="update-auto"]').textContent = info.enabled ? "On" : "Off";
  }
  function renderUpdateDialog(info) {
    const latest = info.latest || {}, installed = update.mode === "installed";
    const working = !installed && ["downloading", "verifying", "installing"].includes(info.status);
    const failed = !installed && ((info.status === "error" && info.error_stage === "download") || Boolean(update.problem));
    $("update-title").textContent = installed ? `OBSReplay ${info.current} is installed` : `OBSReplay ${latest.version} is available`;
    const released = longDate(latest.published);
    $("update-sub").textContent = installed ? (info.previous ? `Updated from ${info.previous}.` : "You have the latest version.") : `You have ${info.current}.${released ? ` Released ${released}.` : ""}`;
    const key = `${latest.version}\n${latest.notes}`;
    if (key !== update.notesKey) { update.notesKey = key; renderNotes($("update-notes"), latest.notes); $("update-notes").scrollTop = 0; }
    const note = $("update-note");
    note.className = "update-note" + (failed ? " problem" : info.blocked && !working && !installed ? " blocked" : "");
    note.textContent = installed || working ? "" : failed ? (update.problem || info.error) : info.blocked ? info.blocked : latest.downloadable ? "OBSReplay restarts to finish. OBS keeps capturing while it does." : "This release has no installer attached. Get it from the releases page.";
    $("update-progress").hidden = !working;
    $("update-actions").hidden = working;
    if (working) {
      const share = info.total > 0 ? Math.min(1, info.received / info.total) : 0, downloading = info.status === "downloading";
      $("update-bar").classList.toggle("busy", !downloading || !info.total);
      $("update-fill").style.width = downloading && info.total ? `${(share * 100).toFixed(1)}%` : "";
      if (downloading && info.total) $("update-bar").setAttribute("aria-valuenow", String(Math.round(share * 100))); else $("update-bar").removeAttribute("aria-valuenow");
      $("update-stage").textContent = downloading ? (info.total ? `Downloading ${megabyteCount(info.received)} of ${megabyteCount(info.total)} MB` : "Downloading…") : info.status === "verifying" ? "Checking the download…" : "Installing. OBSReplay reopens in a moment.";
      $("update-cancel").hidden = !downloading;
    }
    $("update-secondary").hidden = installed;
    $("update-secondary").textContent = failed ? "Open releases page" : "Skip this version";
    $("update-primary").textContent = installed ? "Done" : failed ? "Try again" : latest.downloadable ? "Update now" : "Open releases page";
    $("update-primary").disabled = !installed && !failed && Boolean(info.blocked) && Boolean(latest.downloadable);
    $("update-close").hidden = info.status === "installing" && !installed;
  }
  function renderUpdate() {
    const info = update.state;
    if (!info) return;
    renderUpdateRow(info);
    const dialog = $("update-dialog");
    if (dialog.open && !update.closing) {
      // The offer has nothing left to show once the version is gone (a later check found none).
      if (update.mode === "offer" && !info.available) closeUpdate(); else renderUpdateDialog(info);
    }
    if (info.just_updated) { update.toastWanted = true; Promise.resolve(api().update_seen?.()).catch(() => {}); }
    if (info.available && !info.skipped && !update.prompted) update.wanted = true;
    offerUpdate();
  }
  async function readUpdate() {
    if (!updateReady()) return null;
    try { update.state = unpack(await api().update_state()); } catch { return update.state; }
    renderUpdate();
    // Without the dialog open nothing else asks again while a check runs.
    if (update.state.status === "checking" && !$("update-dialog").open) setTimeout(readUpdate, 500);
    return update.state;
  }
  // The dialog and the toast wait for a quiet moment: a shown window, no other dialog, no setup, nothing in progress.
  function offerUpdate() {
    if (document.hidden || !presented || !state.initialRoute || state.tab === "setup" || state.recording || document.querySelector("dialog[open]")) return;
    if (update.toastWanted) {
      update.toastWanted = false;
      const info = update.state;
      showToast(`Updated to ${info.current}`, info.latest?.version === info.current && info.latest.notes ? "What’s new" : "", () => openUpdate("installed"));
    }
    if (!update.wanted || update.prompted || busy()) return;
    update.wanted = false;
    openUpdate("offer");
  }
  function openUpdate(mode) {
    const dialog = $("update-dialog");
    if (!update.state || dialog.open || (mode === "offer" && !update.state.available)) return;
    if (mode === "offer") update.prompted = true;
    update.mode = mode;
    update.problem = "";
    update.notesKey = "";
    renderUpdateDialog(update.state);
    dialog.showModal();
    // Focus starts on the dialog itself: Enter must not start an update nobody read about.
    dialog.focus({ preventScroll: true });
    animateDialog(dialog, true);
    uiLog("info", `Update dialog opened: mode=${mode} version=${update.state.latest?.version}`);
    clearInterval(update.timer);
    update.timer = setInterval(readUpdate, 350);
  }
  function closeUpdate() {
    const dialog = $("update-dialog");
    if (!dialog.open || update.closing) return;
    clearInterval(update.timer);
    const finish = () => { dialog.classList.remove("closing"); for (const animation of dialog.getAnimations()) animation.cancel(); if (dialog.open) dialog.close(); update.closing = false; $("main").focus({ preventScroll: true }); };
    if (!motionEnabled()) { finish(); return; }
    update.closing = true;
    dialog.classList.add("closing");
    animateDialog(dialog, false).then(finish);
  }
  // Closing while the file downloads stops the download. Once the installer runs there is nothing to close.
  function dismissUpdate() {
    const status = update.state?.status;
    if (update.mode === "offer" && status === "installing") return;
    if (update.mode === "offer" && (status === "downloading" || status === "verifying")) Promise.resolve(api().update_cancel()).catch(() => {});
    closeUpdate();
  }
  async function startUpdate() {
    update.problem = "";
    try { await api().update_start(); } catch (error) { update.problem = errorText(error) || "The update couldn’t start. Try again."; }
    readUpdate();
  }
  function bindUpdate() {
    $("update-close").addEventListener("click", dismissUpdate);
    $("update-dialog").addEventListener("cancel", event => { event.preventDefault(); dismissUpdate(); });
    $("update-cancel").addEventListener("click", () => Promise.resolve(api().update_cancel()).catch(() => {}));
    $("update-primary").addEventListener("click", () => {
      const info = update.state;
      if (update.mode === "installed") closeUpdate();
      else if (!info.latest?.downloadable && !update.problem && info.status !== "error") Promise.resolve(api().open_link(info.latest?.page || info.releases)).catch(showError);
      else startUpdate();
    });
    $("update-secondary").addEventListener("click", () => {
      const info = update.state;
      if ($("update-secondary").textContent === "Open releases page") { Promise.resolve(api().open_link(info.latest?.page || info.releases)).catch(showError); return; }
      Promise.resolve(api().update_skip(true)).then(readUpdate).catch(() => {});
      closeUpdate();
    });
    $("update-check").addEventListener("click", () => {
      if (!updateReady()) return;
      if (update.state?.available) { openUpdate("offer"); return; }
      Promise.resolve(api().update_check()).then(readUpdate).catch(showError);
    });
    $("update-auto").addEventListener("change", () => { if (updateReady()) Promise.resolve(api().update_enabled($("update-auto").checked)).then(readUpdate).catch(showError); });
    $("toast-close").addEventListener("click", () => { $("app-toast").hidden = true; });
    $("toast-action").addEventListener("click", () => { $("app-toast").hidden = true; update.toastAction?.(); });
    // Sent by the native side when the update state changed; "open" comes from the tray menu's update row.
    window.addEventListener("obsreplay:update", async event => { const info = await readUpdate(); if (event.detail === "open" && info?.available) openUpdate("offer"); });
    window.addEventListener("obsreplay:reopen", readUpdate);
    setInterval(offerUpdate, 1500);
  }

  arrangeSettings();
  bindUpdate();
  $("disk-storage").addEventListener("click", () => dispatch("help", { topic: "storage" }));
  $("disk-dismiss").addEventListener("click", () => {
    diskSeenClip = newestReplay();
    try { localStorage.setItem(diskKey, diskSeenClip); } catch {}
    renderDiskNotice();
  });
  $("debug-copy").addEventListener("click", async () => {
    if (!debugInfo) return;
    try { await navigator.clipboard.writeText(`OBSReplay ${debugInfo.full} (commit ${debugInfo.commit}, built ${debugInfo.built})`); $("debug-copy").textContent = "Copied"; setTimeout(() => { $("debug-copy").textContent = "Copy"; }, 1600); }
    catch (error) { showError("Windows didn’t allow copying. Select the version text instead."); }
  });
  $("debug-export").addEventListener("click", async () => {
    try { const saved = await nativeCall("export_log"); if (saved) $("debug-export-status").textContent = `Saved to ${saved}`; }
    catch (error) { showError(error); }
  });
  $("debug-open-logs").addEventListener("click", async () => { try { await nativeCall("open_logs"); } catch (error) { showError(error); } });
  {
    // Shown only for the installed copy; a portable run has nothing to uninstall.
    const row = document.createElement("div");
    row.className = "setting-row entrance-item";
    row.id = "uninstall-row";
    row.hidden = true;
    row.innerHTML = '<div class="setting-copy"><strong>Uninstall OBSReplay</strong><small>Removes the app, its shortcut, its startup entry and its OBS profile. Your clips stay.</small></div><button type="button" class="danger" id="uninstall-app">Uninstall…</button>';
    $("settings-app").append(row);
    $("uninstall-app").addEventListener("click", async () => { try { await nativeCall("uninstall"); } catch (error) { showError(error); } });
  }
  buildDropdowns();
  bindSetup();
  {
    const reader = $("main");
    let target = null, expected = 0, frame = 0;
    const step = () => {
      frame = 0;
      if (target === null) return;
      // Anything else that moved the page (a key, a jump to the top) ends the glide.
      if (Math.abs(reader.scrollTop - expected) > 2) { target = null; return; }
      const distance = target - reader.scrollTop;
      if (Math.abs(distance) < 1) { reader.scrollTop = target; target = null; return; }
      reader.scrollTop += Math.abs(distance * .3) < 1 ? Math.sign(distance) : distance * .3;
      expected = reader.scrollTop;
      frame = requestAnimationFrame(step);
    };
    reader.addEventListener("wheel", event => {
      if (event.target.closest?.(".debug-log") || event.defaultPrevented || event.ctrlKey || event.shiftKey || event.deltaX || event.deltaMode !== 0 || Math.abs(event.deltaY) < 50 || !motionEnabled()) { target = null; return; }
      event.preventDefault();
      const limit = reader.scrollHeight - reader.clientHeight;
      target = Math.max(0, Math.min(limit, (target ?? reader.scrollTop) + event.deltaY));
      expected = reader.scrollTop;
      if (!frame) frame = requestAnimationFrame(step);
    }, { passive: false });
  }
  const readerObserver = new ResizeObserver(() => { scheduleFade(); syncTabIndicators(); });
  readerObserver.observe($("main"));
  readerObserver.observe(document.querySelector(".app-footer"));
  for (const view of $("main").children) readerObserver.observe(view);
  for (const tab of document.querySelectorAll('.settings-tabs [role="tab"]')) readerObserver.observe(tab);
  $("main").addEventListener("scroll", scheduleFade, { passive: true });
  const leaveRail = event => { if (event.detail) $("main").focus({ preventScroll: true }); };
  for (const button of document.querySelectorAll("[data-tab]")) button.addEventListener("click", event => { showTab(button.dataset.tab); leaveRail(event); });
  for (const button of document.querySelectorAll("[data-category]")) {
    button.addEventListener("click", () => showCategory(button.dataset.category));
    button.addEventListener("keydown", event => {
      const names = ["capture", "audio", "app", "debug"];
      let index = names.indexOf(category);
      if (event.key === "ArrowRight") index = (index + 1) % names.length;
      else if (event.key === "ArrowLeft") index = (index + names.length - 1) % names.length;
      else if (event.key === "Home") index = 0;
      else if (event.key === "End") index = names.length - 1;
      else return;
      event.preventDefault();
      showCategory(names[index]);
      $("category-" + names[index]).focus();
    });
  }
  for (const button of document.querySelectorAll("[data-window]")) button.addEventListener("click", async event => {
    if (event.detail !== 0 || !window.saucer) return;
    try {
      if (button.dataset.window === "minimize") await window.saucer.minimize(true);
      else if (button.dataset.window === "maximize") await window.saucer.maximize(!(await window.saucer.maximized()));
      else await window.saucer.close();
    } catch (error) { showError(error); }
  });
  for (const button of document.querySelectorAll("[data-action]")) button.addEventListener("click", () => dispatch(button.dataset.action));
  for (const button of document.querySelectorAll("[data-help]")) button.addEventListener("click", () => dispatch("help", { topic: button.dataset.help }));
  $("help-button").addEventListener("click", event => { showTab("help"); leaveRail(event); });
  $("capture-settings").addEventListener("click", () => showTab("settings"));
  // Repair re-runs setup with the saved settings; unapplied edits are reviewed in Settings first.
  $("capture-repair").addEventListener("click", () => { if (state.dirty) showTab("settings"); else dispatch("setup", formValue()); });
  $("all-clips").addEventListener("click", () => { showClipTab("clips", clipTabs.latest); showTab("clips"); });
  for (const button of document.querySelectorAll("[data-clip-tab]")) {
    button.addEventListener("click", () => showClipTab(button.dataset.clipList, button.dataset.clipTab));
    button.addEventListener("keydown", event => {
      if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
      event.preventDefault();
      const next = event.key === "Home" ? "replays" : event.key === "End" ? "edits" : button.dataset.clipTab === "replays" ? "edits" : "replays";
      showClipTab(button.dataset.clipList, next, true);
    });
  }
  $("dismiss-error").addEventListener("click", () => { state.dismissedError = state.localError || [errorText(state.snapshot?.error), errorText(state.snapshot?.hotkey_error)].filter(Boolean).join("\n"); state.localError = ""; renderFeedback(); });
  $("change-conflicting-shortcut").addEventListener("click", () => {
    showTab("settings");
    showCategory("app");
    const error = errorText(state.snapshot?.hotkey_error);
    const name = /show/i.test(error) && !/save/i.test(error) ? "show_hotkey" : "save_hotkey";
    document.querySelector(`[data-record="${name}"]`).focus();
  });
  $("toggle-capture").addEventListener("click", async () => {
    if (state.snapshot?.replay_active && !await confirmAction("Stop replay capture?", "The unsaved replay buffer will be discarded. Replays already saved stay in your clips folder.", "Stop capture", true)) return;
    dispatch(state.snapshot?.replay_active ? "stop" : "start");
  });
  for (const option of $("theme-picker").children) {
    option.addEventListener("click", () => chooseTheme(option.dataset.themeId));
    option.addEventListener("keydown", event => {
      const options = [...$("theme-picker").children], index = options.indexOf(option);
      const next = event.key === "ArrowRight" || event.key === "ArrowDown" ? index + 1 : event.key === "ArrowLeft" || event.key === "ArrowUp" ? index - 1 : event.key === "Home" ? 0 : event.key === "End" ? options.length - 1 : null;
      if (next === null) return;
      event.preventDefault();
      chooseTheme(options[(next + options.length) % options.length].dataset.themeId, true);
    });
  }
  $("settings-form").addEventListener("input", markDirty);
  $("settings-form").addEventListener("change", markDirty);
  $("encoder").addEventListener("change", () => updateCodecs(true));
  $("setup-start-at-login").addEventListener("input", () => { $("start_at_login").checked = $("setup-start-at-login").checked; markDirty(); });
  $("start_at_login").addEventListener("input", () => { $("setup-start-at-login").checked = $("start_at_login").checked; });
  $("settings-form").addEventListener("submit", event => {
    event.preventDefault();
    const value = formValue();
    if (!value.obs_path && !state.snapshot?.obs_supported) { setup.step = "obs"; showTab("setup"); return; }
    if (!value.clips_dir) { showError("Choose a clips folder before configuring."); $("clips_dir").focus(); return; }
    if (value.save_hotkey.toLowerCase() === value.show_hotkey.toLowerCase()) { showError("Save replay and Show / hide window need different shortcuts."); $("save_hotkey").focus(); return; }
    if (!configured()) { runSetup(value); return; }
    dispatch(needsRepair() ? "setup" : "apply", value);
  });
  $("discard-settings").addEventListener("click", () => { cancelRecording(); fillForm(settings()); renderFeedback(); });
  for (const [buttonId, functionName, fieldId] of [["pick-folder", "pick_folder", "clips_dir"], ["pick-obs", "pick_obs", "obs_path"]]) $(buttonId).addEventListener("click", async () => {
    try {
      const result = await nativeCall(functionName, $(fieldId).value);
      if (result) { $(fieldId).value = result; markDirty(); showPathEnds(); }
    } catch (error) { showError(error); }
  });
  $("refresh-devices").addEventListener("click", () => dispatch("refresh_options"));
  for (const button of document.querySelectorAll("[data-record]")) button.addEventListener("click", async () => {
    if (recorderStarting) return;
    cancelRecording();
    const id = button.dataset.record;
    recorderStarting = true;
    try {
      await nativeCall("hotkey_recording", true);
      if (state.tab !== "settings" || !document.hasFocus()) { await nativeCall("hotkey_recording", false); return; }
      state.recording = { id, previous: $(id).value };
      recorderTimer = setTimeout(() => { cancelRecording(); $("hotkey-message").textContent = "Shortcut recording timed out. Select Change to try again."; }, 40000);
      $(id).value = "Press shortcut…";
      $(id).classList.add("recording");
      $(id).focus();
      $("hotkey-message").textContent = "Press a shortcut. Escape cancels.";
    } catch (error) { showError(error); }
    finally { recorderStarting = false; }
  });
  for (const button of document.querySelectorAll("[data-reset-key]")) button.addEventListener("click", () => { cancelRecording(); const id = button.dataset.resetKey; $(id).value = defaults[id]; markDirty(); });
  document.addEventListener("keydown", event => {
    if (!state.recording) return;
    event.preventDefault();
    event.stopPropagation();
    if (event.key === "Escape") { cancelRecording(); return; }
    if (["Control", "Alt", "Shift", "Meta"].includes(event.key)) return;
    const key = /^Key[A-Z]$/.test(event.code) ? event.code.slice(3) : /^Digit[0-9]$/.test(event.code) ? event.code.slice(5) : /^F([1-9]|1[0-9]|2[0-4])$/.test(event.key) ? event.key : null;
    if (event.metaKey || !key || !(event.ctrlKey || event.altKey || event.shiftKey)) { $("hotkey-message").textContent = "Use Ctrl, Alt, or Shift with a letter, number, or F key. Windows-key shortcuts are reserved."; return; }
    const shortcut = [event.ctrlKey && "Ctrl", event.altKey && "Alt", event.shiftKey && "Shift", key].filter(Boolean).join("+");
    const id = state.recording.id;
    $(id).value = shortcut;
    $(id).classList.remove("recording");
    state.recording = null;
    clearTimeout(recorderTimer);
    nativeCall("hotkey_recording", false).catch(showError);
    $("hotkey-message").textContent = `${shortcut} selected. Apply changes to register it.`;
    markDirty();
  }, true);
  $("footer-hide").addEventListener("click", () => nativeWindowAction("hide"));
  $("hide-window").addEventListener("click", () => nativeWindowAction("hide"));
  $("quit-app").addEventListener("click", async () => {
    let editors = "";
    try { editors = await nativeCall("editors_pending"); } catch {}
    if (await confirmAction("Quit OBSReplay?", `${editors ? `${editors} ` : ""}OBS and any active capture keep running. OBSReplay shortcuts will stop working until you reopen this app.`, "Quit OBSReplay", Boolean(editors))) nativeWindowAction("quit");
  });
  $("reset-integration").addEventListener("click", async () => { if (await confirmAction("Reset OBSReplay integration?", "Close OBS first. The dedicated profile and scene collection will be reset. Your saved clips remain.", "Reset integration")) dispatch("reset"); });
  $("confirm-dialog").querySelector("form").addEventListener("submit", event => { event.preventDefault(); closeDialog(event.submitter?.value || "cancel"); });
  $("confirm-dialog").addEventListener("cancel", event => { event.preventDefault(); closeDialog("cancel"); });
  $("player-close").addEventListener("click", closePlayer);
  $("replay-player").addEventListener("close", () => {
    if ($("replay-player").open) return;
    stopPlayer();
    if (playerReturnFocus?.isConnected) playerReturnFocus.focus();
    else $("main").focus();
  });
  $("replay-player").addEventListener("click", event => { if (event.target === $("replay-player")) closePlayer(); });
  $("replay-player").addEventListener("cancel", event => { event.preventDefault(); closePlayer(); });
  $("player-external").addEventListener("click", () => {
    if (!playerClip || busy()) return;
    const clip = playerClip;
    closePlayer(); dispatch("open_clip", { path: clip.path });
  });
  $("player-edit").addEventListener("click", () => {
    if (!playerClip) return;
    const clip = playerClip;
    closePlayer(); openEditor(clip);
  });
  $("player-reveal").addEventListener("click", () => {
    if (!playerClip || busy()) return;
    ReplayViewer.pause();
    dispatch("reveal_clip", { path: playerClip.path });
  });
  $("player-delete").addEventListener("click", async () => {
    if (!playerClip || busy()) return;
    const clip = playerClip;
    const wasPlaying = ReplayViewer.isPlaying();
    ReplayViewer.pause();
    const confirmed = await confirmAction("Delete replay?", `Permanently delete “${clip.name || "this replay"}” from your computer? This cannot be undone.`, "Delete replay", true);
    // A confirmed delete goes through even if the player was closed while the confirmation was closing.
    // The video is released first so nothing is still reading the file, and the viewer no longer counts as showing it.
    if (confirmed) { ReplayViewer.release(); playerClip = null; closePlayer(); dispatch("delete_clip", { path: clip.path }); return; }
    if (playerClip?.path !== clip.path || !$("replay-player").open) return;
    else if (wasPlaying && !document.hidden) ReplayViewer.play();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) { cancelThumbnail?.(); ReplayViewer.pause(); }
    else drainThumbnails();
  });
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) {
      diskResample = true;
      scheduleDebug();
      // Work that can wait does, so the opening cascade has the first frames to itself.
      schedulePoll(heldForReopen ? 420 : 0); scheduleFade();
      // If the reopen signal never comes, do not leave the page blank.
      if (heldForReopen) setTimeout(() => { if (heldForReopen) cancelEntrance(); }, 900);
    } else { if (!heldForReopen) cancelEntrance(); cancelAnimationFrame(fadeFrame); fadeFrame = 0; if (!state.pending) clearTimeout(pollTimer); }
  });
  // Sent just before the window hides to the tray: the last frame painted is the cascade's starting state,
  // so reopening never flashes the old page before animating.
  window.addEventListener("obsreplay:hiding", () => {
    cancelEntrance();
    if (!presented || !motionEnabled()) return;
    prepareEntrance();
    heldForReopen = true;
  });
  // "Play video" in an editor: open the Clips page on the right list and play that file in the viewer.
  window.addEventListener("obsreplay:play", async event => {
    const path = String(event.detail || "");
    const find = () => (state.snapshot?.clips || []).find(clip => clip.path === path);
    // The export joined the list a moment ago; wait until this window has read it.
    for (let attempt = 0; attempt < 16 && !find(); attempt++) { schedulePoll(0); await new Promise(resolve => setTimeout(resolve, 200)); }
    const clip = find();
    if (!clip) { showError("That video isn’t in your clips any more."); return; }
    if ($("confirm-dialog").open) closeDialog("cancel");
    const tab = clip.edited ? "edits" : "replays";
    showTab("clips");
    clipTabs.clips = tab;
    clipView.app = null;
    clipView.page = Math.max(0, Math.floor((state.snapshot.clips.filter(item => Boolean(item.edited) === Boolean(clip.edited)).findIndex(item => item.path === path)) / pageSize));
    try { localStorage.setItem("obsreplay.clipTabs", JSON.stringify(clipTabs)); } catch {}
    renderClipLists();
    watchReplay(clip);
  });
  window.addEventListener("obsreplay:reopen", () => { diskResample = true; });
  // Sent by the native side when a replay was saved or an error appeared: read the state now.
  window.addEventListener("obsreplay:state", () => schedulePoll(0));
  window.addEventListener("obsreplay:reopen", () => {
    if (!presented || !heldForReopen) { schedulePoll(0); cancelEntrance(); return; }
    requestAnimationFrame(() => requestAnimationFrame(() => { if (heldForReopen) playEntrance($(state.tab + "-view"), true); }));
  });
  for (const type of ["pointerdown", "keydown"]) document.addEventListener(type, event => { if (event.isTrusted) cancelEntrance(); }, true);
  for (const details of document.querySelectorAll("details")) details.addEventListener("toggle", () => {
    if (!details.open || !motionEnabled() || !presented || document.hidden) return;
    const content = details.querySelector(".details-content");
    if (!content) return;
    const animation = content.animate([{ opacity: .4, transform: "translateY(3px)" }, { opacity: 1, transform: "translateY(0)" }], { duration: 160, easing: "cubic-bezier(.2,.8,.2,1)" });
    entranceAnimations.add(animation);
    animation.finished.catch(() => {}).finally(() => entranceAnimations.delete(animation));
  });
  window.addEventListener("blur", cancelRecording);
  fillForm(defaults);
  renderCapture();
  renderClips();
  renderFeedback();
  schedulePoll(0);
  presentUI();
})();
