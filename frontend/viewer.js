"use strict";

// Replay viewer: the modal that plays one saved clip in the main window.
// It shares the clip editor's transport and canvas timeline (ruler, filmstrip, waveform, playhead,
// zoom and scroll) and has no editing tools. app.js opens it, closes it and owns the clip actions.
(() => {
  const $ = id => document.getElementById(id);
  const dialog = $("replay-player");
  const video = $("player-video"), stage = $("viewer-stage"), timeline = $("viewer-timeline"), canvas = $("viewer-canvas");
  if (!dialog || !video || !canvas) return;
  const context = canvas.getContext("2d");
  const LAYOUT = { ruler: 24, gap: 6, video: 62, audio: 38, overview: 8 };  // the editor's lane sizes
  const VOLUME_KEY = "obsreplay.viewerVolume";
  const UNPLAYABLE = "Windows couldn’t play this video. Select Open file to use another player.";

  const state = {
    clip: null, session: null, duration: 0, fps: 60, aspect: 16 / 9, ready: false,
    view: { start: 0, end: 1 }, hover: null, drag: null, scrubTime: null, pendingSeek: null,
    thumbs: [], waveform: null, waveRate: 100, volume: 0.5, muted: false
  };
  let drawFrame = 0, playFrame = 0;

  const errorText = error => !error ? "" : typeof error === "string" ? error : error.message || JSON.stringify(error);
  function setMessage(text) { $("player-status").textContent = text; }

  // ---------- Time formats (the editor's ruler labels and its default Duration format) ----------
  function formatTime(seconds, precise = true) {
    if (!Number.isFinite(seconds)) return "—";
    seconds = Math.max(0, seconds);
    const hours = Math.floor(seconds / 3600), minutes = Math.floor(seconds / 60) % 60;
    const whole = Math.floor(seconds % 60), hundredths = Math.floor((seconds * 100) % 100);
    return `${hours ? `${hours}:${String(minutes).padStart(2, "0")}` : minutes}:${String(whole).padStart(2, "0")}${precise ? `.${String(hundredths).padStart(2, "0")}` : ""}`;
  }
  function formatDuration(seconds, places) {
    if (!Number.isFinite(seconds)) return "—";
    const factor = 10 ** places;
    const rounded = Math.round(Math.max(0, seconds) * factor) / factor;
    const h = Math.floor(rounded / 3600), m = Math.floor(rounded / 60) % 60;
    const s = (rounded - h * 3600 - m * 60).toFixed(places);
    return h ? `${h}h ${m}m ${s}s` : m ? `${m}m ${s}s` : `${s}s`;
  }

  // ---------- Playback ----------
  const currentTime = () => Math.min(state.duration, Math.max(0, state.scrubTime ?? video.currentTime ?? 0));
  function seek(time, follow = true) {
    if (!state.ready) return;
    time = Math.min(state.duration, Math.max(0, time));
    // While dragging, only the newest position is decoded: a seek in flight finishes first.
    if (state.drag?.mode === "scrub" && video.seeking) { state.pendingSeek = time; state.scrubTime = time; }
    else { state.pendingSeek = null; state.scrubTime = null; video.currentTime = time; }
    if (follow) keepVisible(time);
    renderTime(); requestDraw();
  }
  function frameStep(direction) {
    if (!state.ready) return;
    video.pause();
    const frame = Math.floor(currentTime() * state.fps + 0.01);
    seek((frame + direction + 0.5) / state.fps);
  }
  async function play() {
    if (!state.ready || !video.paused) return;
    if (currentTime() >= state.duration - 0.05) video.currentTime = 0;
    try { await video.play(); } catch {}
  }
  function togglePlay() {
    if (!state.ready) return;
    if (video.paused) play(); else video.pause();
  }
  function playLoop() {
    playFrame = 0;
    if (video.paused) return;
    keepVisible(currentTime(), true);
    renderTime(); requestDraw();
    playFrame = requestAnimationFrame(playLoop);
  }
  // The clip's frame rate is measured from the frames actually shown; until then the capture setting is assumed.
  function measureFrameRate(session) {
    if (typeof video.requestVideoFrameCallback !== "function") return;
    let last = null;
    const steps = [];
    const onFrame = (_now, frame) => {
      if (state.session !== session) return;
      const step = frame.mediaTime - last;
      if (last !== null && !video.paused && !video.seeking && step > 0.002 && step < 0.2) steps.push(step);
      last = frame.mediaTime;
      if (steps.length >= 16) {
        steps.sort((a, b) => a - b);
        const measured = 1 / steps[2];  // near the shortest step, so skipped frames don't halve the rate
        state.fps = [24, 25, 30, 48, 50, 60, 90, 120, 144, 240].find(rate => Math.abs(rate / measured - 1) < 0.04) || measured;
        return;
      }
      video.requestVideoFrameCallback(onFrame);
    };
    video.requestVideoFrameCallback(onFrame);
  }

  // ---------- Volume ----------
  function loadVolume() {
    let saved = NaN;
    try { const raw = localStorage.getItem(VOLUME_KEY); if (raw !== null && raw !== "") saved = Number(raw); } catch {}
    state.volume = Number.isFinite(saved) && saved >= 0 && saved <= 1 ? saved : 0.5;
    state.muted = false;
  }
  function saveVolume() { try { localStorage.setItem(VOLUME_KEY, String(state.volume)); } catch {} }
  function applyVolume() {
    const level = state.muted ? 0 : state.volume;
    const slider = $("viewer-volume"), button = $("viewer-mute");
    slider.value = String(state.volume);
    slider.style.setProperty("--fill", `${level * 100}%`);
    slider.setAttribute("aria-valuetext", `${Math.round(level * 100)}%`);
    button.classList.toggle("muted", level === 0);
    button.setAttribute("aria-pressed", String(level === 0));
    button.setAttribute("aria-label", level === 0 ? "Unmute" : "Mute");
    button.title = level === 0 ? "Unmute  M" : "Mute  M";
    video.muted = level === 0;
    video.volume = state.volume;
  }
  function toggleMute() {
    if (state.muted || state.volume === 0) { state.muted = false; if (state.volume === 0) { state.volume = 0.5; saveVolume(); } }
    else state.muted = true;
    applyVolume();
  }

  // ---------- Timeline geometry ----------
  // The waveform lane is dropped when the timeline is too short for it (small windows).
  function metrics() {
    const width = canvas.clientWidth, height = canvas.clientHeight;
    const zoomed = state.view.end - state.view.start < state.duration - 1e-6;
    const videoTop = LAYOUT.ruler + 2;
    const audioTop = videoTop + LAYOUT.video + LAYOUT.gap;
    const audio = height >= audioTop + LAYOUT.audio + 5 + LAYOUT.overview;
    const bottom = audio ? audioTop + LAYOUT.audio : videoTop + LAYOUT.video;
    return { width, height, zoomed, videoTop, audioTop, audio, bottom, overviewTop: bottom + 5 };
  }
  const span = () => Math.max(1e-6, state.view.end - state.view.start);
  const timeToX = time => (time - state.view.start) / span() * canvas.clientWidth;
  const xToTime = x => Math.min(state.duration, Math.max(0, state.view.start + x / canvas.clientWidth * span()));
  function setView(start, end) {
    if (!state.duration) return;
    const minimum = Math.min(state.duration, Math.max(0.5, 40 / state.fps));
    const length = Math.min(state.duration, Math.max(minimum, end - start));
    start = Math.min(Math.max(0, start), state.duration - length);
    state.view = { start, end: start + length };
    requestDraw();
  }
  function zoomBy(factor, anchorTime = currentTime()) {
    if (!state.duration) return;
    const ratio = (anchorTime - state.view.start) / span();
    const length = span() / factor;
    setView(anchorTime - ratio * length, anchorTime - ratio * length + length);
  }
  function keepVisible(time, paging = false) {
    if (time >= state.view.start && time <= state.view.end) return;
    const length = span();
    const start = paging ? time - length * 0.1 : time - length / 2;
    setView(start, start + length);
  }

  // ---------- Drawing (colours and shapes match the editor's timeline) ----------
  function requestDraw() { if (!drawFrame) drawFrame = requestAnimationFrame(draw); }
  function roundRect(x, y, w, h, r) {
    context.beginPath();
    context.roundRect(x, y, Math.max(0, w), h, Math.min(r, w / 2, h / 2));
  }
  function tickStep(pixelsPerSecond) {
    const steps = [1 / state.fps, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600];
    return steps.find(step => step * pixelsPerSecond >= 84) || 3600;
  }
  function draw() {
    drawFrame = 0;
    const ratio = window.devicePixelRatio || 1;
    const width = canvas.clientWidth, height = canvas.clientHeight;
    if (!width || !height) return;
    if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) { canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio); }
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, width, height);
    const m = metrics();
    context.font = "500 11px Inter, 'Segoe UI', sans-serif";
    context.textBaseline = "middle";
    if (!state.duration) {
      context.fillStyle = "#0c121b";
      roundRect(0, m.videoTop, width, LAYOUT.video, 6); context.fill();
      if (m.audio) { roundRect(0, m.audioTop, width, LAYOUT.audio, 6); context.fill(); }
      return;
    }
    // Ruler
    const step = tickStep(width / span());
    const minor = step / (step >= 60 ? 6 : step >= 10 ? 5 : step >= 1 ? 4 : 5);
    for (let t = Math.floor(state.view.start / minor) * minor; t <= state.view.end + minor; t += minor) {
      const x = Math.round(timeToX(t)) + 0.5;
      const major = Math.abs(t / step - Math.round(t / step)) < 1e-4;
      context.fillStyle = major ? "#8a9bb3" : "#4b5c73";
      context.fillRect(x, major ? 14 : 18, 1, major ? 9 : 5);
      if (major && x > -40 && x < width + 40) {
        context.fillStyle = "#b6c4d8";
        const label = step < 1 ? formatTime(t) : formatTime(t, false);
        context.fillText(label, Math.min(width - context.measureText(label).width, x + 4), 8);
      }
    }
    // The clip is one block: filmstrip lane, then waveform lane.
    const left = Math.max(-6, timeToX(0)), right = Math.min(width + 6, timeToX(state.duration));
    const blockWidth = right - left;
    if (blockWidth > 0) {
      context.save();
      roundRect(left, m.videoTop, blockWidth, LAYOUT.video, 5); context.clip();
      context.fillStyle = "#0b121c"; context.fillRect(left, m.videoTop, blockWidth, LAYOUT.video);
      drawFilmstrip(left, right, m.videoTop);
      context.restore();
      if (m.audio) {
        context.save();
        roundRect(left, m.audioTop, blockWidth, LAYOUT.audio, 5); context.clip();
        context.fillStyle = "#122236"; context.fillRect(left, m.audioTop, blockWidth, LAYOUT.audio);
        drawWave(left, right, m.audioTop, "#5f93c8");
        context.restore();
      }
      context.lineWidth = 1; context.strokeStyle = "#3c5674";
      roundRect(left + 0.5, m.videoTop + 0.5, blockWidth - 1, LAYOUT.video - 1, 5); context.stroke();
      if (m.audio) { roundRect(left + 0.5, m.audioTop + 0.5, blockWidth - 1, LAYOUT.audio - 1, 5); context.stroke(); }
    }
    // Hover guide
    if (state.hover && !state.drag) {
      const x = Math.round(timeToX(state.hover.time)) + 0.5;
      context.fillStyle = "rgba(215,228,248,.35)"; context.fillRect(x, LAYOUT.ruler, 1, m.bottom - LAYOUT.ruler);
    }
    // Playhead
    const px = Math.round(timeToX(currentTime()));
    if (px >= -8 && px <= width + 8) {
      context.fillStyle = "#ffffff";
      context.fillRect(px - 1, 12, 2, m.bottom - 10);
      context.beginPath(); context.moveTo(px - 6, 2); context.lineTo(px + 6, 2); context.lineTo(px + 6, 10); context.lineTo(px, 16); context.lineTo(px - 6, 10); context.closePath(); context.fill();
    }
    // Overview scrollbar, only while zoomed
    if (m.zoomed) {
      roundRect(0, m.overviewTop, width, LAYOUT.overview, 4); context.fillStyle = "#162131"; context.fill();
      const x = state.view.start / state.duration * width, w = Math.max(18, span() / state.duration * width);
      roundRect(Math.min(x, width - w), m.overviewTop, w, LAYOUT.overview, 4); context.fillStyle = state.drag?.mode === "pan" ? "#8fb7d6" : "#5d7896"; context.fill();
      context.fillStyle = "#ffffff"; context.fillRect(currentTime() / state.duration * width, m.overviewTop - 1, 1.5, LAYOUT.overview + 2);
    }
    // Hover time flag on the ruler
    if (state.hover && !state.drag && Math.abs(timeToX(state.hover.time) - timeToX(currentTime())) > 36) {
      const label = formatTime(state.hover.time);
      const w = context.measureText(label).width + 12;
      const x = Math.min(width - w, Math.max(0, timeToX(state.hover.time) - w / 2));
      roundRect(x, 0, w, 17, 4); context.fillStyle = "#26364b"; context.fill();
      context.fillStyle = "#f5f7fc"; context.fillText(label, x + 6, 9);
    }
  }
  function drawFilmstrip(left, right, top) {
    const height = LAYOUT.video, tileWidth = Math.max(40, Math.round(height * state.aspect));
    const tileSeconds = tileWidth / canvas.clientWidth * span();
    if (!state.thumbs.length || !tileSeconds) {
      if (right - left > 120) { context.fillStyle = "#6f819a"; context.fillText("Loading previews…", Math.max(left, 0) + 12, top + height / 2); }
      return;
    }
    for (let k = Math.floor(xToTime(Math.max(0, left)) / tileSeconds); ; k++) {
      const start = k * tileSeconds;
      const x = timeToX(start);
      if (x > right || start > state.duration) break;
      const thumb = nearestThumb(Math.min(state.duration, start + tileSeconds / 2));
      if (thumb) context.drawImage(thumb.bitmap, x, top, tileWidth, height);
    }
  }
  function nearestThumb(time) {
    let best = null, distance = Infinity;
    for (const thumb of state.thumbs) { const d = Math.abs(thumb.time - time); if (d < distance) { distance = d; best = thumb; } }
    return best;
  }
  function drawWave(left, right, top, color) {
    const middle = top + LAYOUT.audio / 2, amplitude = LAYOUT.audio / 2 - 4;
    context.fillStyle = color;
    if (!state.waveform) { context.fillRect(Math.max(left, 0), middle - 0.5, Math.min(right, canvas.clientWidth) - Math.max(left, 0), 1); return; }
    const rate = state.waveRate, data = state.waveform;
    const from = Math.max(0, Math.floor(left)), to = Math.min(canvas.clientWidth, Math.ceil(right));
    for (let x = from; x < to; x += 2) {
      const a = Math.floor(xToTime(x) * rate), b = Math.max(a + 1, Math.floor(xToTime(x + 2) * rate));
      let peak = 0;
      for (let i = a; i < b && i < data.length; i++) if (data[i] > peak) peak = data[i];
      const h = Math.max(1, Math.sqrt(peak / 255) * amplitude);
      context.fillRect(x, middle - h, 1.4, h * 2);
    }
  }

  // ---------- Timeline input ----------
  function localPoint(event) { const rect = canvas.getBoundingClientRect(); return { x: event.clientX - rect.left, y: event.clientY - rect.top }; }
  function panTo(x) {
    const start = x / canvas.clientWidth * state.duration - state.drag.offset;
    setView(start, start + span());
  }
  function edgeScroll(x) {
    if (!metrics().zoomed) return;
    const push = x < 16 ? -1 : x > canvas.clientWidth - 16 ? 1 : 0;
    if (push) setView(state.view.start + push * span() * 0.02, state.view.end + push * span() * 0.02);
  }
  canvas.addEventListener("pointerdown", event => {
    if (!state.ready || event.button !== 0) return;
    timeline.focus({ preventScroll: true, focusVisible: false });
    const { x, y } = localPoint(event);
    const m = metrics();
    if (y >= m.overviewTop - 3) {
      if (!m.zoomed) return;
      canvas.setPointerCapture(event.pointerId);
      const offset = x / m.width * state.duration - state.view.start;
      state.drag = { mode: "pan", offset: offset >= 0 && offset <= span() ? offset : span() / 2 };
      panTo(x);
      return;
    }
    canvas.setPointerCapture(event.pointerId);
    state.drag = { mode: "scrub", wasPlaying: !video.paused };
    video.pause();
    seek(xToTime(x), false);
  });
  canvas.addEventListener("pointermove", event => {
    const { x, y } = localPoint(event);
    if (state.drag?.mode === "pan") { panTo(x); return; }
    if (state.drag?.mode === "scrub") { edgeScroll(x); seek(xToTime(x), false); return; }
    const m = metrics();
    state.hover = state.ready && y < m.overviewTop - 3 ? { time: xToTime(x) } : null;
    canvas.style.cursor = !state.ready ? "default" : y >= m.overviewTop - 3 ? (m.zoomed ? "grab" : "default") : "pointer";
    requestDraw();
  });
  function endDrag() {
    const drag = state.drag;
    state.drag = null;
    if (!drag) return;
    if (drag.mode === "scrub" && drag.wasPlaying) play();
    requestDraw();
  }
  canvas.addEventListener("pointerup", endDrag);
  canvas.addEventListener("pointercancel", endDrag);
  canvas.addEventListener("pointerleave", () => { state.hover = null; requestDraw(); });
  canvas.addEventListener("wheel", event => {
    if (!state.duration) return;
    event.preventDefault();
    const time = xToTime(localPoint(event).x);
    if (!event.shiftKey && Math.abs(event.deltaY) >= Math.abs(event.deltaX)) zoomBy(Math.exp(-event.deltaY * 0.0022), time);
    else {
      const delta = (Math.abs(event.deltaX) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY) / canvas.clientWidth * span();
      setView(state.view.start + delta, state.view.end + delta);
    }
  }, { passive: false });
  new ResizeObserver(requestDraw).observe(canvas);
  // Moving the window to a display with another scale changes devicePixelRatio without a resize.
  (function watchScale() { matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`).addEventListener("change", () => { requestDraw(); watchScale(); }, { once: true }); })();

  // ---------- Controls ----------
  function renderTime() {
    const time = currentTime();
    $("viewer-current").textContent = formatDuration(time, 2);
    $("viewer-total").textContent = `/ ${formatDuration(state.duration, 2)}`;
    timeline.setAttribute("aria-valuenow", time.toFixed(2));
    timeline.setAttribute("aria-valuetext", `${formatTime(time)} of ${formatTime(state.duration)}`);
  }
  function renderControls() {
    for (const id of ["viewer-play", "viewer-frame-back", "viewer-frame-forward", "viewer-fullscreen"]) $(id).disabled = !state.ready;
    const playing = state.ready && !video.paused;
    $("viewer-play").classList.toggle("playing", playing);
    $("viewer-play").setAttribute("aria-label", playing ? "Pause" : "Play");
    $("viewer-play").title = playing ? "Pause  Space" : "Play  Space";
  }
  function toggleFullscreen() {
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    else if (state.ready) stage.requestFullscreen().catch(() => {});
  }

  // ---------- Filmstrip thumbnails, decoded from a second muted video like the editor's ----------
  async function buildThumbnails(session, url) {
    const probe = document.createElement("video");
    probe.muted = true; probe.preload = "auto"; probe.crossOrigin = "anonymous";
    probe.src = url;
    session.probe = probe;  // so closing the viewer can stop its downloads at once
    try {
      await new Promise(resolve => { probe.onloadeddata = resolve; probe.onerror = resolve; setTimeout(resolve, 8000); });
      if (!probe.videoWidth || state.session !== session) return;
      const duration = state.duration;
      const count = Math.round(Math.min(120, Math.max(16, duration / 1.5)));
      const height = 90, width = Math.round(height * probe.videoWidth / probe.videoHeight);
      const surface = document.createElement("canvas"); surface.width = width; surface.height = height;
      const draw2d = surface.getContext("2d");
      // Coarse pass first so the whole strip fills quickly, then refine.
      const order = [];
      for (let stride = 8; stride >= 1; stride = Math.floor(stride / 2)) for (let i = 0; i < count; i += stride) if (!order.includes(i)) order.push(i);
      for (const i of order) {
        if (state.session !== session) break;
        const time = Math.max(0, Math.min(duration - 0.05, (i + 0.5) / count * duration));
        const ok = await new Promise(resolve => {
          const timer = setTimeout(() => resolve(false), 4000);
          probe.onseeked = () => { clearTimeout(timer); resolve(true); };
          probe.currentTime = time;
        });
        if (!ok) continue;
        if (state.session !== session) break;
        draw2d.drawImage(probe, 0, 0, width, height);
        const bitmap = await createImageBitmap(surface);
        if (state.session !== session) { bitmap.close(); break; }
        state.thumbs.push({ time, bitmap });
        requestDraw();
      }
    } catch {} finally {
      probe.onloadeddata = probe.onseeked = probe.onerror = null;
      probe.removeAttribute("src"); probe.load();
    }
  }
  async function loadWaveform(session, request, path) {
    try {
      const result = JSON.parse(await request(path));
      if (state.session !== session || result?.status !== "ready" || !result.peaks) return;
      const binary = atob(result.peaks);
      const values = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) values[i] = binary.charCodeAt(i);
      state.waveform = values; state.waveRate = Number(result.rate) || 100;
      requestDraw();
    } catch {}  // no FFmpeg, no audio or an older app: the lane keeps its flat line
  }

  // ---------- Video events ----------
  video.addEventListener("loadedmetadata", () => {
    if (!state.session) return;
    state.duration = Number.isFinite(video.duration) ? video.duration : 0;
    if (!state.duration) { setMessage("This video has no readable length. Select Open file to use another player."); return; }
    if (video.videoWidth && video.videoHeight) state.aspect = video.videoWidth / video.videoHeight;
    state.ready = true;
    timeline.setAttribute("aria-valuemax", state.duration.toFixed(2));
    const length = document.createElement("span");
    length.className = "viewer-duration";
    length.textContent = formatDuration(state.duration, 1);
    $("player-detail").querySelector(".viewer-duration")?.remove();
    $("player-detail").append(length);
    setView(0, state.duration);
    applyVolume(); renderControls(); renderTime();
    measureFrameRate(state.session);
    buildThumbnails(state.session, video.currentSrc || video.src);
  });
  video.addEventListener("loadeddata", () => { if (state.session) setMessage(""); });
  video.addEventListener("error", () => {
    if (!state.session || !video.getAttribute("src")) return;
    state.ready = false;
    setMessage(UNPLAYABLE);
    renderControls();
  });
  video.addEventListener("play", () => { renderControls(); if (!playFrame) playFrame = requestAnimationFrame(playLoop); });
  video.addEventListener("pause", () => { renderControls(); renderTime(); requestDraw(); });
  // The latest seek always wins: any seek that isn't the queued scrub target drops the queue.
  video.addEventListener("seeking", () => {
    if (state.pendingSeek != null && !state.drag && Math.abs(video.currentTime - state.pendingSeek) > 0.0005) { state.pendingSeek = null; state.scrubTime = null; }
  });
  video.addEventListener("seeked", () => {
    if (state.pendingSeek != null) { const next = state.pendingSeek; state.pendingSeek = null; video.currentTime = next; if (!state.drag) state.scrubTime = null; }
    else state.scrubTime = null;
    renderTime(); requestDraw();
  });
  video.addEventListener("ended", () => { renderTime(); requestDraw(); });

  // ---------- Commands ----------
  stage.addEventListener("click", togglePlay);
  stage.addEventListener("dblclick", toggleFullscreen);
  $("viewer-play").addEventListener("click", togglePlay);
  for (const [id, direction] of [["viewer-frame-back", -1], ["viewer-frame-forward", 1]]) {
    const button = $(id);
    let delayTimer = 0, repeatTimer = 0;
    const stop = () => { clearTimeout(delayTimer); clearInterval(repeatTimer); delayTimer = repeatTimer = 0; };
    button.addEventListener("pointerdown", event => {
      if (event.button !== 0 || button.disabled) return;
      button.setPointerCapture(event.pointerId);
      frameStep(direction);
      stop();
      delayTimer = setTimeout(() => { repeatTimer = setInterval(() => frameStep(direction), 100); }, 500);
    });
    for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) button.addEventListener(type, stop);
    button.addEventListener("click", event => { if (event.detail === 0) frameStep(direction); });  // keyboard activation
  }
  $("viewer-volume").addEventListener("input", () => { state.volume = Number($("viewer-volume").value); state.muted = false; applyVolume(); });
  $("viewer-volume").addEventListener("change", saveVolume);
  $("viewer-mute").addEventListener("click", toggleMute);
  $("viewer-fullscreen").addEventListener("click", toggleFullscreen);
  // After a pointer interaction the keyboard goes back to the timeline, so Space plays instead of repeating the button.
  for (const element of dialog.querySelectorAll(".viewer-transport button, #viewer-volume"))
    element.addEventListener("pointerup", () => setTimeout(() => { if (document.activeElement === element) timeline.focus({ preventScroll: true, focusVisible: false }); }, 0));

  dialog.addEventListener("keydown", event => {
    if (event.defaultPrevented || event.ctrlKey || event.metaKey || event.altKey || !state.session) return;
    const target = event.target, key = event.key;
    if (target instanceof HTMLInputElement && ["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End", "PageUp", "PageDown"].includes(key)) return;
    if (target instanceof HTMLButtonElement && (key === " " || key === "Enter")) return;  // a focused button keeps its own activation
    let handled = true;
    if (key === " " || key.toLowerCase() === "k") togglePlay();
    else if (key === "ArrowLeft" || key === "ArrowRight") { video.pause(); seek(currentTime() + (key === "ArrowLeft" ? -1 : 1) * (event.shiftKey ? 5 : 1)); }
    else if (key === ",") frameStep(-1);
    else if (key === ".") frameStep(1);
    else if (key === "Home") seek(0);
    else if (key === "End") seek(state.duration);
    else if (key.toLowerCase() === "m") toggleMute();
    else if (key === "+" || key === "=") zoomBy(1.6);
    else if (key === "-" || key === "_") zoomBy(1 / 1.6);
    else if (key === "0") setView(0, state.duration);
    else if (key.toLowerCase() === "f") toggleFullscreen();
    else handled = false;
    if (handled) event.preventDefault();
  });

  // tooltip.js keeps its tooltip in <body>, which a modal dialog covers. While the viewer is open
  // the tooltip lives inside the dialog so the hints on its controls can be seen.
  function hostTooltip(inside) {
    const tip = $("app-tooltip");
    const home = inside ? dialog : document.body;
    if (tip && tip.parentElement !== home) home.append(tip);
  }
  dialog.addEventListener("close", () => hostTooltip(false));

  // ---------- Lifecycle (called by app.js) ----------
  function release() {
    const probe = state.session?.probe;
    if (probe) { probe.removeAttribute("src"); probe.load(); }
    state.session = null; state.clip = null;
    cancelAnimationFrame(playFrame); playFrame = 0;
    video.pause(); video.removeAttribute("src"); video.load();
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    for (const thumb of state.thumbs) thumb.bitmap.close();
    Object.assign(state, { duration: 0, ready: false, aspect: 16 / 9, view: { start: 0, end: 1 }, hover: null, drag: null, scrubTime: null, pendingSeek: null, thumbs: [], waveform: null });
    timeline.setAttribute("aria-valuemax", "0");
    $("player-detail").querySelector(".viewer-duration")?.remove();
    renderControls(); renderTime(); requestDraw();
  }
  // Opens the dialog on a clip, or switches an open viewer to another clip.
  //   mediaUrl(path) -> Promise<string>   playable URL for the clip
  //   waveform(path) -> Promise<string>   optional; JSON from the native clip_waveform
  //   fps                                 optional; frame rate assumed until it is measured
  function open(clip, { mediaUrl, waveform, fps } = {}) {
    release();
    const session = state.session = {};
    state.clip = clip;
    state.fps = Number(fps) > 1 ? Number(fps) : 60;
    $("player-title").textContent = clip.name || "Replay";
    setMessage("Loading replay…");
    loadVolume(); applyVolume(); renderControls(); renderTime();
    if (!dialog.open) dialog.showModal();
    hostTooltip(true);
    timeline.focus({ preventScroll: true, focusVisible: false });
    requestDraw();
    Promise.resolve().then(() => mediaUrl(clip.path)).then(url => {
      if (state.session !== session || !dialog.open) return;
      video.src = url; video.load();
      if (!document.hidden) video.play().catch(() => {});  // if autoplay is refused the Play button is there
    }).catch(error => { if (state.session === session) setMessage(errorText(error)); });
    if (waveform) loadWaveform(session, waveform, clip.path);
  }

  window.ReplayViewer = {
    open, release, play,
    pause() { video.pause(); },
    isOpen: () => dialog.open,
    isPlaying: () => Boolean(state.session) && !video.paused,
    clip: () => state.clip,
    // Read-only view of the timeline, for checks.
    inspect: () => ({ ready: state.ready, duration: state.duration, fps: state.fps, view: { ...state.view }, thumbnails: state.thumbs.length, peaks: state.waveform ? state.waveform.length : 0, volume: state.volume, muted: state.muted || state.volume === 0, lanes: metrics() })
  };

  applyVolume(); renderControls(); renderTime();
})();
