"use strict";

(() => {
  const $ = id => document.getElementById(id);
  const fields = ["obs_path", "clips_dir", "clip_seconds", "capture", "monitor_id", "output_height", "fps", "encoder", "codec", "quality", "mic_enabled", "mic_device", "desktop_device", "noise", "separate_tracks", "sort_by_app", "save_sound", "start_at_login", "save_hotkey", "show_hotkey", "replay_enabled", "animations_enabled"];
  const numbers = new Set(["clip_seconds", "output_height", "fps", "quality"]);
  const defaults = { obs_path: "", clips_dir: "", clip_seconds: 120, capture: "game", monitor_id: "", output_height: 1080, fps: 60, encoder: "auto", codec: "h264", quality: 20, mic_enabled: false, mic_device: "default", desktop_device: "default", noise: "off", separate_tracks: false, sort_by_app: false, save_sound: true, start_at_login: true, save_hotkey: "Ctrl+Shift+F10", show_hotkey: "Alt+X", replay_enabled: false, animations_enabled: true };
  const state = { snapshot: null, tab: "capture", dirty: false, loaded: false, pending: null, recording: null, dismissedError: "", localError: "", optionsKey: "", clipsKey: "", native: false, initialRoute: false };
  let nativeChain = Promise.resolve();
  let pollTimer;
  let polling = false;
  let recorderStarting = false;
  let recorderTimer;
  let presented = false;
  let dialogClosing = false;
  let category = "capture";
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
    for (const name of ["capture", "audio", "app"]) {
      const panel = document.createElement("div");
      panel.id = `settings-${name}`;
      panel.className = "settings-category";
      panel.setAttribute("role", "tabpanel");
      panel.setAttribute("aria-labelledby", `category-${name}`);
      panel.hidden = name !== "capture";
      groups[name] = panel;
    }
    const hints = { obs_path: "Use an installed copy of OBS Studio.", clips_dir: "Where your saved replays go.", monitor_id: "Used for entire-display capture.", clip_seconds: "The maximum history kept in memory.", quality: "Higher quality keeps more detail and uses more storage.", desktop_device: "Include sound from your output device.", mic_device: "Choose the microphone to record.", noise: "Reduce background microphone noise." };
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
    groups.app.append($("hotkey-help"), $("hotkey-message"));
    const animationRow = document.createElement("label");
    animationRow.className = "check-row";
    animationRow.innerHTML = '<span><strong>Animations</strong><small>Smooth page, panel, and control transitions.</small></span><input type="checkbox" id="animations_enabled" name="animations_enabled" checked>';
    groups.app.append(animationRow);
    for (const name of ["start_at_login", "replay_enabled", "sort_by_app", "save_sound"]) groups.app.append($(name).closest(".check-row"));
    const startup = document.createElement("label");
    startup.className = "check-row";
    startup.id = "setup-startup-row";
    startup.innerHTML = '<span><strong>Start with Windows</strong><small>Keep replay controls available after you sign in.</small></span><input type="checkbox" id="setup-start-at-login" checked>';
    groups.capture.append(startup);
    const explanation = $("apply-explanation");
    explanation.textContent = "Capture and audio changes restart OBS. Your current replay is saved first.";
    form.replaceChildren(groups.capture, groups.audio, groups.app, explanation);
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
    setupTitle.textContent = "Configure OBS to get started";
    const setupHint = document.createElement("p");
    setupHint.textContent = "Close OBS once for initial setup, then select Configure OBS. Later changes restart it automatically.";
    setupCopy.append(setupTitle, setupHint);
    startup.classList.add("setup-startup");
    startup.querySelector("small").remove();
    setupCopy.append(startup);
    const obsHelp = $("setup-intro").querySelector('[data-help="obs"]');
    const webviewHelp = $("setup-intro").querySelector('[data-help="webview2"]');
    if (webviewHelp) $("help-view").querySelector(".inline-actions").append(webviewHelp);
    $("setup-intro").replaceChildren(setupCopy, obsHelp);
    for (const item of document.querySelectorAll("#capture-view>*,#clips-view>*,#settings-view>.view-heading,#settings-view>.settings-tabs,#setup-intro,#help-view>.view-heading,#help-view>.help-content")) item.classList.add("entrance-item");
  }

  function cancelEntrance() {
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
  function playEntrance(view) {
    cancelEntrance();
    if (!motionEnabled() || document.hidden) return;
    entranceTargets(view).forEach((element, index) => {
      const animation = element.animate([{ opacity: .35, transform: "translateY(4px)" }, { opacity: 1, transform: "translateY(0)" }], { duration: 220, delay: Math.min(index * 30, 270), easing: "cubic-bezier(.2,.8,.2,1)", fill: "backwards" });
      entranceAnimations.add(animation);
      animation.finished.catch(() => {}).finally(() => entranceAnimations.delete(animation));
    });
  }
  async function presentUI() {
    await Promise.race([document.fonts.ready, new Promise(resolve => setTimeout(resolve, 450))]);
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const view = $(state.tab + "-view");
    if (motionEnabled() && typeof api()?.ui_ready === "function") {
      preparedElements = entranceTargets(view);
      for (const element of preparedElements) { element.style.opacity = ".35"; element.style.transform = "translateY(4px)"; }
    }
    try { if (typeof api()?.ui_ready === "function") await nativeCall("ui_ready"); }
    catch (error) { showError(error); }
    presented = true;
    playEntrance($(state.tab + "-view"));
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
    $("main").scrollTop = 0;
    if (changed && presented && animate) playEntrance($("settings-" + name));
    scheduleFade();
  }

  function api() { return window.saucer?.exposed; }
  function nativeCall(name, ...args) {
    const call = nativeChain.then(() => {
      if (typeof api()?.[name] !== "function") throw new Error("The native controller is unavailable. Open OBSReplay.exe to use capture controls.");
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
        const text = String(current[name] ?? "");
        if (input.tagName === "SELECT" && ![...input.options].some(option => option.value === text)) {
          const option = document.createElement("option");
          option.value = text;
          option.textContent = `${text || "Default"} (configured)`;
          input.append(option);
        }
        input.value = text;
      }
    }
    state.dirty = false;
    $("setup-start-at-login").checked = $("start_at_login").checked;
    updateCodecs();
    updateDependencies();
  }
  function formValue() {
    const value = { ...settings() };
    for (const name of fields) {
      const input = $(name);
      value[name] = input.type === "checkbox" ? input.checked : numbers.has(name) ? Number(input.value) : input.value.trim();
    }
    return value;
  }
  function updateDependencies() {
    const desktop = $("capture").value === "desktop";
    $("capture-hint").textContent = desktop ? "Entire-display capture includes other windows and notifications. It never uses a game hook." : "Game mode captures compatible fullscreen games. It never records the desktop as a fallback.";
    $("monitor_id").disabled = !desktop || busy();
    $("mic_device").disabled = !$("mic_enabled").checked || busy();
    $("noise").disabled = !$("mic_enabled").checked || busy();
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
    const changed = state.tab !== tab;
    state.tab = tab;
    for (const name of ["capture", "clips", "settings", "help"]) $(name + "-view").hidden = name !== tab;
    for (const button of document.querySelectorAll("[data-tab]")) {
      if (button.dataset.tab === tab) button.setAttribute("aria-current", "page");
      else button.removeAttribute("aria-current");
    }
    if (tab === "help") $("help-button").setAttribute("aria-current", "page");
    else $("help-button").removeAttribute("aria-current");
    $("main").scrollTop = 0;
    if (changed && presented) playEntrance($(tab + "-view"));
    scheduleFade();
    renderFeedback();
  }
  function renderFeedback() {
    const snapshot = state.snapshot;
    const backendError = [errorText(snapshot?.error), errorText(snapshot?.hotkey_error)].filter(Boolean).join("\n");
    const error = state.localError || backendError;
    const visibleError = error && error !== state.dismissedError;
    $("error-notice").hidden = !visibleError;
    $("error-text").textContent = visibleError ? error : "";
    $("change-conflicting-shortcut").hidden = !errorText(snapshot?.hotkey_error) || !visibleError;
    $("change-conflicting-shortcut").disabled = !state.native || busy();
    $("settings-actions").hidden = state.tab !== "settings";
    $("footer-hide").hidden = state.tab === "settings";
    const pending = busy();
    for (const button of document.querySelectorAll(".player-actions button")) button.disabled = !state.native || pending;
    $("apply-settings").textContent = pending && ["setup", "apply"].includes(state.pending?.action || snapshot?.operation) ? "Applying…" : needsRepair() ? "Repair OBS setup" : snapshot?.compression_update ? "Apply improved compression" : configured() ? "Apply changes" : "Configure OBS";
    $("apply-settings").disabled = !state.native || pending || (!state.dirty && configured() && !needsRepair() && !snapshot?.compression_update);
    $("discard-settings").disabled = !state.dirty || pending;
    if (!state.native) $("footer-status").textContent = "Native controller unavailable";
    else if (pending) $("footer-status").textContent = snapshot?.message || "Working…";
    else if (state.tab === "settings" && state.dirty) $("footer-status").textContent = "Unapplied changes";
    else $("footer-status").textContent = snapshot?.message || (configured() ? "Settings saved" : "Configure OBS to get started");
    for (const element of document.querySelectorAll("[data-action], [data-help], #footer-hide, #hide-window, #quit-app, #reset-integration, #refresh-devices, #pick-obs, #pick-folder, #setup-start-at-login")) element.disabled = !state.native || pending;
    for (const element of $("settings-form").elements) if (element.id !== "apply-settings") element.disabled = !state.native || pending;
    if (state.native && !pending) updateDependencies();
    const canControl = Boolean(snapshot?.connected && snapshot?.owned);
    const needsAttention = canControl && snapshot?.capture_ready === false;
    $("save-button").disabled = !state.native || pending || !canControl || !snapshot?.replay_active || needsAttention;
    $("save-button").lastChild.textContent = state.pending?.action === "save" || snapshot?.operation === "save" && snapshot?.busy ? "Saving…" : "Save replay";
    $("toggle-capture").disabled = !state.native || pending || !configured() || Boolean(snapshot?.connected && !snapshot?.owned) || Boolean(needsAttention && !snapshot?.replay_active);
    scheduleFade();
  }
  function renderCapture() {
    const snapshot = state.snapshot || {};
    const value = settings();
    const active = Boolean(snapshot.replay_active && snapshot.connected && snapshot.owned);
    $("connection-label").textContent = !state.native ? "Controller unavailable" : snapshot.connected ? snapshot.owned ? "OBS connected" : "Other OBS session" : snapshot.busy ? "Connecting to OBS…" : "OBS disconnected";
    $("connection-dot").className = `state-dot${snapshot.connected && snapshot.owned ? " ready" : ""}`;
    let title = "Capture stopped";
    let description = "Start capture to keep your recent moments ready to save.";
    if (!state.native) { title = "Open the desktop app"; description = "Capture controls need the native OBSReplay controller."; }
    else if (!configured()) { title = "Ready to set up"; description = "Configure a dedicated OBS profile to get started."; }
    else if (snapshot.connected && !snapshot.owned) { title = "Different OBS session"; description = "Switch to the OBSReplay profile and collection before controlling capture."; }
    else if (active) { title = "Capturing"; description = `Save up to the last ${formatDuration(value.clip_seconds)} with your replay shortcut.`; }
    else if (!snapshot.connected) { title = snapshot.busy ? "Connecting…" : "OBS disconnected"; description = "Start capture or reconnect to your dedicated OBS session."; }
    if (snapshot.connected && snapshot.owned && snapshot.capture_ready === false) {
      title = "Capture needs attention";
      description = `${snapshot.capture_issue || "The dedicated capture source is not ready."} Choose Repair OBS setup in Settings.`;
    }
    if (snapshot.busy && ["start", "stop"].includes(snapshot.operation)) title = snapshot.operation === "start" ? "Starting capture…" : "Stopping capture…";
    $("capture-state").textContent = title;
    $("capture-description").textContent = description;
    $("capture-mark").className = `capture-mark${snapshot.connected && (!snapshot.owned || snapshot.capture_ready === false) ? " warning" : active ? " active" : ""}`;
    $("toggle-capture").textContent = active ? "Stop capture" : "Start capture";
    $("save-shortcut").textContent = value.save_hotkey;
    $("summary-length").textContent = formatDuration(value.clip_seconds);
    $("summary-mode").textContent = value.capture === "desktop" ? "Entire display" : "Fullscreen game";
    $("summary-video").textContent = `${value.output_height ? `${value.output_height}p` : "Original"} / ${value.fps} fps`;
    $("summary-mic").textContent = value.mic_enabled ? "On" : "Off";
    $("capture-folder").textContent = value.clips_dir || "Choose a destination in Settings.";
    $("capture-note").textContent = value.capture === "desktop" ? "Entire-display capture includes other windows and notifications." : "Game mode captures compatible fullscreen games. It never records the desktop as a fallback.";
    $("connection-actions").hidden = Boolean(snapshot.connected && snapshot.owned);
    $("setup-intro").hidden = configured() || category !== "capture";
    $("setup-startup-row").hidden = configured();
    $("settings-subtitle").textContent = configured() ? "Capture, audio, and app preferences." : "Choose your replay capture settings.";
    $("help-status").textContent = snapshot.status ? `${snapshot.status}${snapshot.message ? `: ${snapshot.message}` : ""}` : $("connection-label").textContent;
  }
  function actionButton(label, action, path) {
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = label;
    button.addEventListener("click", () => dispatch(action, { path }));
    return button;
  }
  const thumbnailCache = new Map();
  const thumbnailJobs = new Map();
  let thumbnailRunning = false;
  let cancelThumbnail = null;
  let playerClip = null;
  let playerRequest = 0;
  let playerReturnFocus = null;
  const clipKey = clip => JSON.stringify([clip.path, clip.bytes, clip.saved_at]);
  function paintThumbnail(image, data) {
    if (!image.isConnected) return;
    if (data) { image.src = data; image.hidden = false; image.parentElement.dataset.thumbnail = "ready"; }
    else image.parentElement.dataset.thumbnail = "unavailable";
  }
  const thumbnailObserver = new IntersectionObserver(entries => {
    for (const entry of entries) if (entry.isIntersecting) {
      thumbnailObserver.unobserve(entry.target);
      const { clip, image } = entry.target.replayThumbnail;
      const key = clipKey(clip);
      if (thumbnailCache.has(key)) paintThumbnail(image, thumbnailCache.get(key));
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
            while (thumbnailCache.size > 24) thumbnailCache.delete(thumbnailCache.keys().next().value);
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
  function stopPlayer() {
    ++playerRequest;
    const video = $("player-video");
    video.pause(); video.removeAttribute("src"); video.load();
    playerClip = null;
    drainThumbnails();
  }
  function closePlayer() {
    if (!$("replay-player").open) return;
    $("replay-player").close();
  }
  async function watchReplay(clip) {
    const alreadyOpen = $("replay-player").open;
    if (alreadyOpen) stopPlayer();
    cancelThumbnail?.();
    if (!alreadyOpen) playerReturnFocus = document.activeElement;
    playerClip = clip;
    const request = ++playerRequest;
    $("player-title").textContent = clip.name || "Replay";
    $("player-status").textContent = "Loading replay…";
    $("player-detail").textContent = [clip.app, formatTime(clip.saved_at)].filter(Boolean).join(" · ");
    if (!alreadyOpen) $("replay-player").showModal();
    if (motionEnabled()) {
      const animation = $("replay-player").animate([{ opacity: .4, transform: "translateY(4px)" }, { opacity: 1, transform: "translateY(0)" }], { duration: 160, easing: "cubic-bezier(.2,.8,.2,1)" });
      entranceAnimations.add(animation);
      animation.finished.catch(() => {}).finally(() => entranceAnimations.delete(animation));
    }
    try {
      const url = await nativeCall("media_url", clip.path);
      if (request !== playerRequest || !$("replay-player").open) return;
      const video = $("player-video");
      video.src = url; video.load();
      video.play().catch(error => {
        if (request === playerRequest && error.name === "NotAllowedError") $("player-status").textContent = "Press Play to watch this replay.";
      });
    } catch (error) {
      if (request === playerRequest) $("player-status").textContent = errorText(error);
    }
  }
  function clipRow(clip, compact = false) {
    const row = document.createElement("div");
    row.className = `clip-row${compact ? " latest-clip" : ""}`;
    const preview = document.createElement("button");
    preview.type = "button"; preview.className = "clip-preview";
    preview.setAttribute("aria-label", `Watch ${clip.name || "replay"}`);
    preview.addEventListener("click", () => watchReplay(clip));
    const thumbnail = document.createElement("span"); thumbnail.className = "clip-thumbnail";
    const image = document.createElement("img"); image.alt = ""; image.hidden = true;
    const play = document.createElement("span"); play.className = "clip-play";
    play.innerHTML = '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="m7 4 9 6-9 6z"/></svg>';
    thumbnail.append(image, play);
    thumbnail.replayThumbnail = { clip, image };
    thumbnailObserver.observe(thumbnail);
    const info = document.createElement("span");
    info.className = "clip-info";
    const name = document.createElement("span");
    name.className = "clip-name";
    name.textContent = clip.name || clip.path?.split(/[\\/]/).pop() || "Saved replay";
    name.title = clip.path || name.textContent;
    const meta = document.createElement("span");
    meta.className = "clip-meta";
    meta.textContent = [clip.app, formatTime(clip.saved_at), formatBytes(clip.bytes)].filter(Boolean).join(" · ");
    info.append(name, meta);
    const actions = document.createElement("div");
    actions.className = "clip-actions";
    actions.append(actionButton("Open", "open_clip", clip.path));
    if (!compact) actions.append(actionButton("Show in folder", "reveal_clip", clip.path));
    preview.append(thumbnail, info);
    row.append(preview, actions);
    return row;
  }
  function renderClips() {
    const clips = Array.isArray(state.snapshot?.clips) ? state.snapshot.clips : [];
    const key = JSON.stringify(clips);
    if (key === state.clipsKey) return;
    state.clipsKey = key;
    thumbnailObserver.disconnect();
    const known = new Set(clips.map(clipKey));
    for (const key of thumbnailCache.keys()) if (!known.has(key)) thumbnailCache.delete(key);
    for (const key of thumbnailJobs.keys()) if (!known.has(key)) thumbnailJobs.delete(key);
    if (playerClip && !clips.some(clip => clip.path === playerClip.path)) {
      closePlayer();
      showError("This replay was deleted or moved. Your recent clips have been updated.");
    }
    $("clips-list").replaceChildren();
    $("latest-content").replaceChildren();
    $("latest-content").className = clips.length ? "" : "empty-line";
    if (clips.length) {
      for (const clip of clips.slice(0, 50)) $("clips-list").append(clipRow(clip));
      for (const clip of clips.slice(0, 5)) $("latest-content").append(clipRow(clip, true));
    } else {
      const empty = document.createElement("div");
      empty.className = "clips-empty";
      const title = document.createElement("h2"); title.textContent = "No replays saved yet";
      const description = document.createElement("p"); description.textContent = "Start capture, then press your save shortcut. Open your clips folder to find older files.";
      empty.append(title, description);
      $("clips-list").append(empty);
      $("latest-content").textContent = "No replays saved yet. Start capture, then press your save shortcut.";
    }
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
      if (result === false || result?.error || result?.ok === false) throw new Error(errorText(result?.error) || "The native controller did not accept this action.");
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
      if (!snapshot || typeof snapshot !== "object" || !snapshot.settings) throw new Error("The controller returned an invalid state. Reopen OBSReplay if this continues.");
      if (state.native && state.loaded && !state.pending && snapshot.revision === state.snapshot?.revision && snapshot.hotkey_error === state.snapshot?.hotkey_error) {
        polling = false;
        schedulePoll(snapshot.busy ? 500 : 1000);
        return;
      }
      state.native = true;
      state.snapshot = snapshot;
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
          if (!snapshot.error && ["setup", "apply"].includes(action)) { fillForm(snapshot.settings); if (action === "setup" && snapshot.integration) showTab("capture"); }
        } else if (Date.now() - state.pending.started > 120000) {
          state.pending = null;
          showError("The controller has not confirmed completion. Check OBS and the connection state before trying again. Replay saves are never retried automatically.");
        }
      }
      if (!state.initialRoute) { state.initialRoute = true; if (!configured()) showTab("settings"); }
      renderCapture();
      renderClips();
      renderFeedback();
    } catch (error) {
      state.native = false;
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
    if (motionEnabled()) {
      const animation = $("confirm-dialog").animate([{ opacity: .4, transform: "translateY(5px) scale(.985)" }, { opacity: 1, transform: "translateY(0) scale(1)" }], { duration: 160, easing: "cubic-bezier(.2,.8,.2,1)" });
      entranceAnimations.add(animation);
      animation.finished.catch(() => {}).finally(() => entranceAnimations.delete(animation));
    }
    return new Promise(resolve => $("confirm-dialog").addEventListener("close", () => resolve($("confirm-dialog").returnValue === "confirm"), { once: true }));
  }
  function closeDialog(value) {
    if (dialogClosing) return;
    const dialog = $("confirm-dialog");
    if (!motionEnabled()) { dialog.close(value); return; }
    dialogClosing = true;
    const animation = dialog.animate([{ opacity: 1, transform: "translateY(0)" }, { opacity: 0, transform: "translateY(4px)" }], { duration: 110, easing: "ease-in" });
    entranceAnimations.add(animation);
    animation.finished.catch(() => {}).finally(() => { entranceAnimations.delete(animation); if (dialog.open) dialog.close(value); dialogClosing = false; });
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
  arrangeSettings();
  const readerObserver = new ResizeObserver(scheduleFade);
  readerObserver.observe($("main"));
  readerObserver.observe(document.querySelector(".app-footer"));
  for (const view of $("main").children) readerObserver.observe(view);
  $("main").addEventListener("scroll", scheduleFade, { passive: true });
  for (const button of document.querySelectorAll("[data-tab]")) button.addEventListener("click", () => showTab(button.dataset.tab));
  for (const button of document.querySelectorAll("[data-category]")) {
    button.addEventListener("click", () => showCategory(button.dataset.category));
    button.addEventListener("keydown", event => {
      const names = ["capture", "audio", "app"];
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
  $("help-button").addEventListener("click", () => showTab("help"));
  $("capture-settings").addEventListener("click", () => showTab("settings"));
  $("all-clips").addEventListener("click", () => showTab("clips"));
  $("dismiss-error").addEventListener("click", () => { state.dismissedError = state.localError || [errorText(state.snapshot?.error), errorText(state.snapshot?.hotkey_error)].filter(Boolean).join("\n"); state.localError = ""; renderFeedback(); });
  $("change-conflicting-shortcut").addEventListener("click", () => {
    showTab("settings");
    showCategory("app");
    const error = errorText(state.snapshot?.hotkey_error);
    const name = /show/i.test(error) && !/save/i.test(error) ? "show_hotkey" : "save_hotkey";
    document.querySelector(`[data-record="${name}"]`).focus();
  });
  $("toggle-capture").addEventListener("click", async () => {
    if (state.snapshot?.replay_active && !await confirmAction("Stop replay capture?", "The unsaved replay buffer will be discarded. Replays already saved stay in your clips folder.", "Stop capture")) return;
    dispatch(state.snapshot?.replay_active ? "stop" : "start");
  });
  $("settings-form").addEventListener("input", markDirty);
  $("settings-form").addEventListener("change", markDirty);
  $("encoder").addEventListener("change", () => updateCodecs(true));
  $("setup-start-at-login").addEventListener("input", () => { $("start_at_login").checked = $("setup-start-at-login").checked; markDirty(); });
  $("start_at_login").addEventListener("input", () => { $("setup-start-at-login").checked = $("start_at_login").checked; });
  $("settings-form").addEventListener("submit", event => {
    event.preventDefault();
    const value = formValue();
    if (!value.obs_path) { showError("Choose the OBS Studio executable before configuring."); $("obs_path").focus(); return; }
    if (!value.clips_dir) { showError("Choose a clips folder before configuring."); $("clips_dir").focus(); return; }
    if (value.save_hotkey.toLowerCase() === value.show_hotkey.toLowerCase()) { showError("Save replay and Show / hide window need different shortcuts."); $("save_hotkey").focus(); return; }
    dispatch(configured() && !needsRepair() ? "apply" : "setup", value);
  });
  $("discard-settings").addEventListener("click", () => { cancelRecording(); fillForm(settings()); renderFeedback(); });
  for (const [buttonId, functionName, fieldId] of [["pick-folder", "pick_folder", "clips_dir"], ["pick-obs", "pick_obs", "obs_path"]]) $(buttonId).addEventListener("click", async () => {
    try {
      const result = await nativeCall(functionName, $(fieldId).value);
      if (result) { $(fieldId).value = result; markDirty(); }
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
  $("quit-app").addEventListener("click", async () => { if (await confirmAction("Quit OBSReplay?", "OBS and any active capture keep running. OBSReplay shortcuts will stop working until you reopen this app.", "Quit OBSReplay")) nativeWindowAction("quit"); });
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
  $("player-external").addEventListener("click", () => {
    if (!playerClip || busy()) return;
    const clip = playerClip;
    closePlayer(); dispatch("open_clip", { path: clip.path });
  });
  $("player-reveal").addEventListener("click", () => {
    if (!playerClip || busy()) return;
    $("player-video").pause();
    dispatch("reveal_clip", { path: playerClip.path });
  });
  $("player-delete").addEventListener("click", async () => {
    if (!playerClip || busy()) return;
    const clip = playerClip;
    const video = $("player-video");
    const wasPlaying = !video.paused;
    video.pause();
    const confirmed = await confirmAction("Delete replay?", `Permanently delete “${clip.name || "this replay"}” from your computer? This cannot be undone.`, "Delete replay", true);
    if (playerClip?.path !== clip.path || !$("replay-player").open) return;
    if (confirmed) { closePlayer(); dispatch("delete_clip", { path: clip.path }); }
    else if (wasPlaying && !document.hidden) video.play().catch(() => {});
  });
  $("player-video").addEventListener("loadeddata", () => { if (playerClip) $("player-status").textContent = ""; });
  $("player-video").addEventListener("error", () => {
    if (playerClip && $("player-video").getAttribute("src")) $("player-status").textContent = "Windows could not play this video. You can try Open file.";
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) { cancelThumbnail?.(); $("player-video").pause(); }
    else drainThumbnails();
  });
  document.addEventListener("visibilitychange", () => { if (!document.hidden) { schedulePoll(0); scheduleFade(); } else { cancelEntrance(); cancelAnimationFrame(fadeFrame); fadeFrame = 0; if (!state.pending) clearTimeout(pollTimer); } });
  window.addEventListener("obsreplay:reopen", () => { schedulePoll(0); if (presented) playEntrance($(state.tab + "-view")); });
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
