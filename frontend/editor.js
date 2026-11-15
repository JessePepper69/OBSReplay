"use strict";

(() => {
  const $ = id => document.getElementById(id);
  const video = $("video");
  const canvas = $("timeline-canvas");
  const context = canvas.getContext("2d");
  const MIN_PIECE = 0.05;
  const QUALITY_NAMES = ["Smallest · 720p", "Very small · 720p", "Small", "Compact", "Standard", "Balanced", "Good", "High", "Near original", "Original"];
  const LAYOUT = { ruler: 24, gap: 6, video: 62, audio: 38, overview: 8 };

  const state = {
    native: null, revision: -1, clip: null, duration: 0, fps: 60, aspect: 16 / 9,
    pieces: [], selected: new Set(), anchor: null, nextId: 1,
    history: [], future: [], baseline: "", exportedSignature: "",
    view: { start: 0, end: 1 }, hover: null, drag: null,
    skipRemoved: true, mediaReady: false, closing: false, quality: 10,
    exportStatus: "idle", lastExport: null, waveform: null, waveRate: 100, waveRequested: false,
    thumbs: [], thumbsRequested: false, motion: true,
    timecode: "duration", scale: 1, master: 0.5, masterMuted: false, tracks: [], tracksFromProbe: false, savedTrackLevels: {},
    stems: [], previewSwapped: false, prefsLoaded: false
  };
  const trackFor = index => state.tracks.find(track => track.index === index);
  let nativeChain = Promise.resolve();
  let pollTimer = 0;
  let drawFrame = 0;
  let playFrame = 0;

  function api() { return window.saucer?.exposed; }
  function nativeCall(name, ...args) {
    const call = nativeChain.then(() => {
      if (typeof api()?.[name] !== "function") throw new Error("The editor needs the OBSReplay desktop app.");
      return api()[name](...args);
    });
    nativeChain = call.catch(() => {});
    return call;
  }
  const errorText = error => !error ? "" : typeof error === "string" ? error : error.message || JSON.stringify(error);
  function announce(text) { $("announcer").textContent = ""; requestAnimationFrame(() => { $("announcer").textContent = text; }); }

  function formatTime(seconds, precise = true) {
    if (!Number.isFinite(seconds)) return "—";
    const negative = seconds < 0; seconds = Math.abs(seconds);
    const hours = Math.floor(seconds / 3600), minutes = Math.floor(seconds / 60) % 60;
    const whole = Math.floor(seconds % 60), hundredths = Math.floor((seconds * 100) % 100);
    const body = `${hours ? `${hours}:${String(minutes).padStart(2, "0")}` : minutes}:${String(whole).padStart(2, "0")}${precise ? `.${String(hundredths).padStart(2, "0")}` : ""}`;
    return negative ? `-${body}` : body;
  }
  const TIME_FORMATS = [
    { id: "duration", label: "Duration" },
    { id: "clock", label: "Clock time" },
    { id: "smpte", label: "Timecode (frames)" }
  ];
  // Formats a length or position in the chosen style. `places` is the decimal precision for Duration.
  function formatAs(format, seconds, places = 1) {
    if (!Number.isFinite(seconds)) return "—";
    seconds = Math.max(0, seconds);
    if (format === "clock") {
      const millis = Math.round(seconds * 1000);
      const h = Math.floor(millis / 3600000), m = Math.floor(millis / 60000) % 60, s = Math.floor(millis / 1000) % 60, ms = millis % 1000;
      return `${h ? `${h}:${String(m).padStart(2, "0")}` : m}:${String(s).padStart(2, "0")}.${String(ms).padStart(3, "0")}`;
    }
    if (format === "smpte") {
      const rate = Math.max(1, Math.round(state.fps));
      const frames = Math.round(seconds * rate);
      const f = frames % rate, total = Math.floor(frames / rate);
      return [Math.floor(total / 3600), Math.floor(total / 60) % 60, total % 60, f].map(value => String(value).padStart(2, "0")).join(":");
    }
    const factor = 10 ** places;
    const rounded = Math.round(seconds * factor) / factor;
    const h = Math.floor(rounded / 3600), m = Math.floor(rounded / 60) % 60;
    const s = (rounded - h * 3600 - m * 60).toFixed(places);
    return h ? `${h}h ${m}m ${s}s` : m ? `${m}m ${s}s` : `${s}s`;
  }
  const formatLength = seconds => formatAs(state.timecode, seconds, 1);
  const formatPosition = seconds => formatAs(state.timecode, seconds, 2);
  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) return "";
    const format = new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 });
    return bytes < 1024 * 1024 ? `${format.format(bytes / 1024)} KB` : bytes < 1024 ** 3 ? `${format.format(bytes / 1024 ** 2)} MB` : `${format.format(bytes / 1024 ** 3)} GB`;
  }

  // ---------- Edit model ----------
  const signature = () => JSON.stringify(keptSegments().map(([start, end]) => [Math.round(start * 1000), Math.round(end * 1000)]));
  const keptSegments = () => {
    const segments = [];
    for (const piece of state.pieces) {
      if (piece.removed) continue;
      const last = segments[segments.length - 1];
      if (last && Math.abs(last[1] - piece.start) < 1e-6) last[1] = piece.end;
      else segments.push([piece.start, piece.end]);
    }
    return segments;
  };
  const keptLength = () => keptSegments().reduce((sum, [start, end]) => sum + end - start, 0);
  const editable = () => state.mediaReady && state.exportStatus !== "running";
  function pieceIndexAt(time) {
    const pieces = state.pieces;
    if (!pieces.length) return -1;
    if (time >= state.duration) return pieces.length - 1;
    for (let i = 0; i < pieces.length; i++) if (time < pieces[i].end) return i;
    return pieces.length - 1;
  }
  function snapshot() { return { pieces: state.pieces.map(piece => ({ ...piece })), selected: [...state.selected] }; }
  function restore(saved) { state.pieces = saved.pieces.map(piece => ({ ...piece })); state.selected = new Set(saved.selected); }
  function commit(message) {
    state.future = [];
    state.history.push(state.pending || snapshot());
    state.pending = null;
    if (state.history.length > 200) state.history.shift();
    if (message) announce(message);
    changed();
  }
  function remember() { state.pending = snapshot(); }
  function undo() {
    if (!editable() || !state.history.length) return;
    state.future.push(snapshot()); restore(state.history.pop()); announce("Undone"); changed();
  }
  function redo() {
    if (!editable() || !state.future.length) return;
    state.history.push(snapshot()); restore(state.future.pop()); announce("Redone"); changed();
  }
  function splitAt(time, quiet = false) {
    const index = pieceIndexAt(time);
    const piece = state.pieces[index];
    if (!piece || time - piece.start < MIN_PIECE || piece.end - time < MIN_PIECE) return false;
    const right = { id: state.nextId++, start: time, end: piece.end, removed: piece.removed };
    piece.end = time;
    state.pieces.splice(index + 1, 0, right);
    if (state.selected.has(piece.id)) state.selected.add(right.id);
    if (!quiet) announce(`Split at ${formatTime(time)}`);
    return true;
  }
  function split() {
    if (!editable()) return;
    remember();
    if (splitAt(currentTime())) commit();
    else { state.pending = null; flash("Move the playhead away from an existing cut to split."); }
  }
  function targetPieces() {
    if (state.selected.size) return state.pieces.filter(piece => state.selected.has(piece.id));
    const piece = state.pieces[pieceIndexAt(currentTime())];
    return piece ? [piece] : [];
  }
  function toggleRemove(mode = "toggle") {
    if (!editable()) return;
    const targets = targetPieces();
    if (!targets.length) return;
    const remove = targets.some(piece => !piece.removed);
    if (mode === "remove" && !remove) return flash("Already removed. Press R or Restore section to bring it back.");
    if (mode === "restore" && remove && !targets.some(piece => piece.removed)) return flash("Nothing to restore here. Select a removed section first.");
    if (mode === "restore") { remember(); for (const piece of targets) piece.removed = false; commit(`${targets.length === 1 ? "Section" : `${targets.length} sections`} restored`); return; }
    if (remove && state.pieces.every(piece => piece.removed || targets.includes(piece))) {
      flash("Keep at least one section. Split first to delete part of the clip.");
      return;
    }
    remember();
    for (const piece of targets) piece.removed = remove;
    const length = targets.reduce((sum, piece) => sum + piece.end - piece.start, 0);
    commit(`${targets.length === 1 ? "Section" : `${targets.length} sections`} ${remove ? "deleted" : "restored"} (${formatLength(length)})`);
    if (remove && !video.paused) skipRemoved();
  }
  function trim(before) {
    if (!editable()) return;
    const time = currentTime();
    if (before ? time < MIN_PIECE : state.duration - time < MIN_PIECE) return flash(before ? "Move the playhead into the clip to delete what comes before it." : "Move the playhead back to delete what comes after it.");
    remember();
    splitAt(time, true);
    let touched = false;
    for (const piece of state.pieces) {
      const outside = before ? piece.end <= time + 1e-6 : piece.start >= time - 1e-6;
      if (outside && !piece.removed) { piece.removed = true; touched = true; }
    }
    if (!state.pieces.some(piece => !piece.removed)) { restore(state.pending); state.pending = null; return flash("Keep at least one section."); }
    if (!touched && state.pending && JSON.stringify(state.pending.pieces) === JSON.stringify(state.pieces)) { state.pending = null; return; }
    commit(before ? `Deleted everything before ${formatTime(time)}` : `Deleted everything after ${formatTime(time)}`);
  }
  function joinCut(index) {
    if (!editable() || index <= 0 || index >= state.pieces.length) return;
    remember();
    const left = state.pieces[index - 1], right = state.pieces[index];
    left.end = right.end;
    left.removed = left.removed && right.removed;
    if (state.selected.has(right.id)) state.selected.add(left.id);
    state.selected.delete(right.id);
    state.pieces.splice(index, 1);
    commit(`Cut removed at ${formatTime(right.start)}`);
  }
  function selectAt(time, event) {
    const index = pieceIndexAt(time);
    const piece = state.pieces[index];
    if (!piece) return;
    if (event.shiftKey && state.anchor !== null) {
      const anchorIndex = Math.max(0, state.pieces.findIndex(item => item.id === state.anchor));
      const [from, to] = anchorIndex < index ? [anchorIndex, index] : [index, anchorIndex];
      if (!event.ctrlKey) state.selected.clear();
      for (let i = from; i <= to; i++) state.selected.add(state.pieces[i].id);
    } else if (event.ctrlKey || event.metaKey) {
      if (state.selected.has(piece.id)) state.selected.delete(piece.id); else state.selected.add(piece.id);
      state.anchor = piece.id;
    } else {
      state.selected = new Set([piece.id]);
      state.anchor = piece.id;
    }
    renderControls(); requestDraw();
  }
  function resetPieces() {
    state.pieces = [{ id: state.nextId++, start: 0, end: state.duration, removed: false }];
    state.selected.clear(); state.history = []; state.future = [];
    state.baseline = signature(); state.exportedSignature = "";
  }

  // ---------- Playback ----------
  const currentTime = () => Math.min(state.duration, Math.max(0, state.scrubTime ?? video.currentTime ?? 0));
  function seek(time, follow = true) {
    if (!state.mediaReady) return;
    time = Math.min(state.duration, Math.max(0, time));
    if (state.drag?.mode === "scrub" && video.seeking) { state.pendingSeek = time; state.scrubTime = time; }
    else { state.pendingSeek = null; state.scrubTime = null; video.currentTime = time; }
    if (follow) keepVisible(time);
    renderTime(); requestDraw();
  }
  function frameStep(direction) {
    if (!state.mediaReady) return;
    video.pause();
    const frame = Math.floor(currentTime() * state.fps + 0.01);
    seek((frame + direction + 0.5) / state.fps);
  }
  function cutTimes() { return state.pieces.slice(1).map(piece => piece.start); }
  function jumpCut(direction) {
    const time = currentTime();
    const times = [0, ...cutTimes(), state.duration];
    const target = direction < 0 ? [...times].reverse().find(value => value < time - 0.01) : times.find(value => value > time + 0.01);
    if (target !== undefined) seek(target);
  }
  function nextKeptAfter(time) { return state.pieces.find(piece => !piece.removed && piece.end > time + 0.01); }
  function skipRemoved() {
    if (!state.skipRemoved) return;
    const piece = state.pieces[pieceIndexAt(currentTime())];
    if (!piece?.removed) return;
    const next = nextKeptAfter(piece.end - 0.001);
    if (next) video.currentTime = Math.max(next.start, piece.end);
    else { video.pause(); video.currentTime = Math.max(0, piece.start - 0.001); }
  }
  async function playFromStart() {
    if (!state.mediaReady) return;
    const first = state.skipRemoved ? state.pieces.find(piece => !piece.removed) : state.pieces[0];
    video.currentTime = first ? first.start : 0;
    try { await video.play(); } catch (error) { flash(errorText(error)); }
  }
  async function togglePlay() {
    if (!state.mediaReady) return;
    if (!video.paused) { video.pause(); return; }
    const atEnd = currentTime() >= state.duration - 0.05 || (state.skipRemoved && !nextKeptAfter(currentTime()));
    if (atEnd) {
      const first = state.skipRemoved ? state.pieces.find(piece => !piece.removed) : state.pieces[0];
      video.currentTime = first ? first.start : 0;
    }
    skipRemoved();
    try { await video.play(); } catch (error) { flash(errorText(error)); }
  }
  function syncStems(force = false) {
    for (const { audio } of state.stems) {
      if (force || Math.abs(audio.currentTime - video.currentTime) > 0.08) { try { audio.currentTime = video.currentTime; } catch {} }
      if (video.paused || video.seeking) { if (!audio.paused) audio.pause(); }
      else if (audio.paused) audio.play().catch(() => {});
    }
  }
  // Effective level = master x track. Until the separate tracks are ready the clip's own mix plays at the master level.
  function applyVolumes() {
    const master = state.masterMuted ? 0 : state.master;
    $("volume-master").value = String(state.master);
    $("volume-master").style.setProperty("--fill", `${master * 100}%`);
    $("mute-master").classList.toggle("muted", master === 0);
    $("mute-master").setAttribute("aria-pressed", String(state.masterMuted));
    $("mute-master").setAttribute("aria-label", state.masterMuted ? "Unmute" : "Mute");
    for (const track of state.tracks) {
      const level = track.muted ? 0 : track.level;
      if (track.audio) track.audio.volume = Math.min(1, master * level);
      const row = document.querySelector(`.track-row[data-track="${track.index}"]`);
      if (!row) continue;
      row.querySelector("input").value = String(track.level);
      row.querySelector("input").style.setProperty("--fill", `${level * 100}%`);
      row.querySelector("output").textContent = `${Math.round(level * 100)}%`;
      const button = row.querySelector("button");
      button.classList.toggle("muted", level === 0);
      button.setAttribute("aria-pressed", String(track.muted));
    }
    video.muted = state.stems.length > 0 || master === 0;
    video.volume = master;
  }
  function playLoop() {
    playFrame = 0;
    if (video.paused) return;
    skipRemoved();
    syncStems();
    keepVisible(currentTime(), true);
    renderTime(); requestDraw();
    playFrame = requestAnimationFrame(playLoop);
  }

  // ---------- Timeline geometry ----------
  function metrics() {
    const width = canvas.clientWidth, height = canvas.clientHeight;
    const zoomed = state.view.end - state.view.start < state.duration - 1e-6;
    const videoTop = LAYOUT.ruler + 2;
    const audioTop = videoTop + LAYOUT.video + LAYOUT.gap;
    const overviewTop = audioTop + LAYOUT.audio + 9;
    return { width, height, zoomed, videoTop, audioTop, overviewTop, bottom: audioTop + LAYOUT.audio };
  }
  const span = () => Math.max(1e-6, state.view.end - state.view.start);
  const timeToX = time => (time - state.view.start) / span() * canvas.clientWidth;
  const xToTime = x => Math.min(state.duration, Math.max(0, state.view.start + x / canvas.clientWidth * span()));
  function setView(start, end) {
    const minimum = Math.min(state.duration, Math.max(0.5, 40 / state.fps));
    let length = Math.min(state.duration, Math.max(minimum, end - start));
    start = Math.min(Math.max(0, start), state.duration - length);
    state.view = { start, end: start + length };
    const zoom = state.duration > minimum ? Math.log(state.duration / length) / Math.log(state.duration / minimum) * 100 : 0;
    $("zoom-fit").hidden = zoom < 0.5;
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

  // ---------- Drawing ----------
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
      roundRect(0, m.videoTop, width, LAYOUT.video, 6); context.fillStyle = "#0c121b"; context.fill();
      roundRect(0, m.audioTop, width, LAYOUT.audio, 6); context.fill();
      return;
    }
    const pps = width / span();
    // Ruler
    const step = tickStep(pps);
    const minor = step / (step >= 60 ? 6 : step >= 10 ? 5 : step >= 1 ? 4 : 5);
    context.fillStyle = "#8a9bb3";
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
    // Pieces
    state.pieces.forEach((piece, index) => {
      const x1 = timeToX(piece.start), x2 = timeToX(piece.end);
      if (x2 < -4 || x1 > width + 4) return;
      const left = Math.max(-6, x1) + (index ? 1 : 0), right = Math.min(width + 6, x2) - (index < state.pieces.length - 1 ? 1 : 0);
      const w = right - left;
      if (w <= 0) return;
      const selected = state.selected.has(piece.id);
      // video lane with filmstrip
      context.save();
      roundRect(left, m.videoTop, w, LAYOUT.video, 5); context.clip();
      context.fillStyle = "#0b121c"; context.fillRect(left, m.videoTop, w, LAYOUT.video);
      drawFilmstrip(left, right, m.videoTop);
      if (piece.removed) {
        context.fillStyle = "rgba(8,11,17,.78)"; context.fillRect(left, m.videoTop, w, LAYOUT.video);
        context.strokeStyle = "rgba(238,130,147,.34)"; context.lineWidth = 1.5;
        context.beginPath();
        for (let x = left - LAYOUT.video; x < right; x += 9) { context.moveTo(x, m.videoTop + LAYOUT.video); context.lineTo(x + LAYOUT.video, m.videoTop); }
        context.stroke();
        if (w > 76) { context.fillStyle = "#f4b8c3"; context.font = "600 11px Inter, 'Segoe UI', sans-serif"; context.fillText("Removed", left + w / 2 - context.measureText("Removed").width / 2, m.videoTop + LAYOUT.video / 2); context.font = "500 11px Inter, 'Segoe UI', sans-serif"; }
      } else if (selected) { context.fillStyle = "rgba(107,182,255,.16)"; context.fillRect(left, m.videoTop, w, LAYOUT.video); }
      context.restore();
      // audio lane
      context.save();
      roundRect(left, m.audioTop, w, LAYOUT.audio, 5); context.clip();
      context.fillStyle = piece.removed ? "#140f16" : selected ? "#16304b" : "#122236"; context.fillRect(left, m.audioTop, w, LAYOUT.audio);
      drawWave(left, right, m.audioTop, piece.removed ? "#6d4553" : selected ? "#a9d3ff" : "#5f93c8");
      context.restore();
      // outline
      const outline = selected ? (piece.removed ? "#f4b8c3" : "#a9d3ff") : piece.removed ? "#6f3c4b" : "#3c5674";
      context.lineWidth = selected ? 2 : 1; context.strokeStyle = outline;
      roundRect(left + (selected ? 1 : .5), m.videoTop + (selected ? 1 : .5), w - (selected ? 2 : 1), LAYOUT.video - (selected ? 2 : 1), 5); context.stroke();
      roundRect(left + (selected ? 1 : .5), m.audioTop + (selected ? 1 : .5), w - (selected ? 2 : 1), LAYOUT.audio - (selected ? 2 : 1), 5); context.stroke();
    });
    // Cuts
    const hoverCut = state.drag?.mode === "cut" ? state.drag.index : state.hover?.cut;
    state.pieces.forEach((piece, index) => {
      if (!index) return;
      const x = Math.round(timeToX(piece.start));
      if (x < -6 || x > width + 6) return;
      const active = hoverCut === index;
      context.fillStyle = active ? "#ffffff" : "#d7e4f8";
      context.fillRect(x - (active ? 1.5 : 1), m.videoTop - 3, active ? 3 : 2, m.bottom - m.videoTop + 6);
      roundRect(x - 5, m.videoTop - 7, 10, 8, 2); context.fill();
    });
    // Hover guide
    if (state.hover && !state.drag && state.hover.cut == null) {
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
    // Overview when zoomed
    if (m.zoomed) {
      roundRect(0, m.overviewTop, width, LAYOUT.overview, 4); context.fillStyle = "#162131"; context.fill();
      for (const piece of state.pieces) if (piece.removed) { context.fillStyle = "#4a2734"; context.fillRect(piece.start / state.duration * width, m.overviewTop + 2, (piece.end - piece.start) / state.duration * width, LAYOUT.overview - 4); }
      const x = state.view.start / state.duration * width, w = Math.max(18, span() / state.duration * width);
      roundRect(Math.min(x, width - w), m.overviewTop, w, LAYOUT.overview, 4); context.fillStyle = state.drag?.mode === "pan" ? "#8fb7d6" : m.zoomed ? "#5d7896" : "#2c3d52"; context.fill();
      context.fillStyle = "#ffffff"; context.fillRect(currentTime() / state.duration * width, m.overviewTop - 1, 1.5, LAYOUT.overview + 2);
    }
    // Hover time tooltip on ruler
    if (state.hover && state.hover.time != null && !state.drag && Math.abs(timeToX(state.hover.time) - timeToX(currentTime())) > 36) {
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
    const first = Math.floor(xToTime(Math.max(0, left)) / tileSeconds);
    for (let k = first; ; k++) {
      const start = k * tileSeconds;
      const x = timeToX(start);
      if (x > right || start > state.duration) break;
      const center = Math.min(state.duration, start + tileSeconds / 2);
      const thumb = nearestThumb(center);
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
  function cutNear(x, y) {
    const m = metrics();
    if (y < m.videoTop - 8 || y > m.bottom + 2) return null;
    let best = null, distance = 6;
    state.pieces.forEach((piece, index) => {
      if (!index) return;
      const d = Math.abs(timeToX(piece.start) - x);
      if (d <= distance) { distance = d; best = index; }
    });
    return best;
  }
  function localPoint(event) { const rect = canvas.getBoundingClientRect(); return { x: event.clientX - rect.left, y: event.clientY - rect.top }; }
  canvas.addEventListener("pointerdown", event => {
    if (!state.mediaReady || event.button !== 0) return;
    $("timeline").focus({ preventScroll: true, focusVisible: false });
    const { x, y } = localPoint(event);
    const m = metrics();
    canvas.setPointerCapture(event.pointerId);
    if (y >= m.overviewTop - 3) {
      if (!m.zoomed) { canvas.releasePointerCapture(event.pointerId); return; }
      const offset = x / m.width * state.duration - state.view.start;
      const inside = offset >= 0 && offset <= span();
      state.drag = { mode: "pan", offset: inside ? offset : span() / 2 };
      panTo(x);
      return;
    }
    const cut = editable() ? cutNear(x, y) : null;
    if (cut !== null) {
      remember();
      state.drag = { mode: "cut", index: cut, moved: false };
      canvas.style.cursor = "ew-resize";
      return;
    }
    const time = xToTime(x);
    if (y > LAYOUT.ruler && y < m.bottom + 2) selectAt(time, event);
    state.drag = { mode: "scrub", wasPlaying: !video.paused };
    video.pause();
    seek(time, false);
  });
  function panTo(x) {
    const center = x / canvas.clientWidth * state.duration - state.drag.offset;
    setView(center, center + span());
  }
  canvas.addEventListener("pointermove", event => {
    const { x, y } = localPoint(event);
    const drag = state.drag;
    if (drag?.mode === "pan") { panTo(x); return; }
    if (drag?.mode === "cut") {
      const left = state.pieces[drag.index - 1], right = state.pieces[drag.index];
      let time = xToTime(x);
      const playhead = currentTime();
      if (Math.abs(timeToX(playhead) - x) < 7) time = playhead;
      time = Math.min(right.end - MIN_PIECE, Math.max(left.start + MIN_PIECE, time));
      left.end = right.start = time; drag.moved = true;
      edgeScroll(x);
      renderControls(); requestDraw();
      return;
    }
    if (drag?.mode === "scrub") { edgeScroll(x); seek(xToTime(x), false); return; }
    const cut = state.mediaReady && editable() ? cutNear(x, y) : null;
    state.hover = state.mediaReady ? { time: xToTime(x), cut } : null;
    canvas.style.cursor = cut !== null ? "ew-resize" : y >= metrics().overviewTop - 3 ? (metrics().zoomed ? "grab" : "default") : y > metrics().bottom + 2 ? "default" : "pointer";
    requestDraw();
  });
  function edgeScroll(x) {
    if (!metrics().zoomed) return;
    const width = canvas.clientWidth;
    const push = x < 16 ? -1 : x > width - 16 ? 1 : 0;
    if (push) setView(state.view.start + push * span() * 0.02, state.view.end + push * span() * 0.02);
  }
  function endDrag() {
    const drag = state.drag;
    state.drag = null;
    canvas.style.cursor = "";
    if (!drag) return;
    if (drag.mode === "cut") {
      if (drag.moved) commit(`Cut moved to ${formatTime(state.pieces[drag.index].start)}`);
      else state.pending = null;
    }
    if (drag.mode === "scrub" && drag.wasPlaying) togglePlay();
    requestDraw();
  }
  canvas.addEventListener("pointerup", endDrag);
  canvas.addEventListener("dblclick", event => {
    if (!editable()) return;
    const { x, y } = localPoint(event);
    const cut = cutNear(x, y);
    if (cut !== null) joinCut(cut);
  });
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

  // ---------- Controls ----------
  let flashTimer = 0;
  function flash(text) {
    $("toast").textContent = text;
    $("toast").hidden = false;
    clearTimeout(flashTimer);
    flashTimer = setTimeout(() => { $("toast").hidden = true; }, 2600);
    announce(text);
  }
  function renderTime() {
    const time = currentTime();
    $("time-current").textContent = formatPosition(time);
    $("time-total").textContent = `/ ${formatPosition(state.duration)}`;
    let output = 0, inside = false;
    for (const piece of state.pieces) {
      if (piece.end <= time) { if (!piece.removed) output += piece.end - piece.start; continue; }
      if (piece.start <= time) { inside = !piece.removed; if (inside) output += time - piece.start; }
      break;
    }
    const removedHere = state.mediaReady && state.pieces[pieceIndexAt(time)]?.removed;
    $("time-result").textContent = !state.mediaReady || keptLength() === state.duration ? "" : removedHere ? "Not in export" : `In export ${formatPosition(output)}`;
    $("stage").classList.toggle("on-removed", Boolean(removedHere));
    $("removed-flag").hidden = !removedHere;
    const timeline = $("timeline");
    timeline.setAttribute("aria-valuenow", time.toFixed(2));
    const index = pieceIndexAt(time);
    timeline.setAttribute("aria-valuetext", `${formatTime(time)}, section ${index + 1} of ${state.pieces.length}${removedHere ? ", removed" : ""}`);
  }
  function renderControls() {
    const ready = state.mediaReady, edit = editable();
    const targets = ready ? targetPieces() : [];
    const restoring = targets.length > 0 && targets.every(piece => piece.removed);
    $("toggle-remove").classList.toggle("restore", restoring);
    $("toggle-label").textContent = restoring ? (targets.length > 1 ? "Restore sections" : "Restore section") : (targets.length > 1 ? "Delete sections" : "Delete section");
    $("toggle-remove").disabled = !edit || !targets.length;
    for (const id of ["split", "trim-start", "trim-end"]) $(id).disabled = !edit;
    $("undo").disabled = !edit || !state.history.length;
    $("redo").disabled = !edit || !state.future.length;
    for (const id of ["play", "frame-back", "frame-forward", "prev-cut", "next-cut", "zoom-fit", "skip-removed", "volume-master", "mute-master", "tracks-button"]) $(id).disabled = !ready;
    const segments = keptSegments(), length = keptLength();
    $("result-length").textContent = ready ? formatLength(length) : "—";
    const removedLength = state.duration - length;
    $("result-detail").textContent = !ready ? "Loading…" : removedLength > 0.005 ? `${formatLength(removedLength)} removed · ${segments.length} ${segments.length === 1 ? "part" : "parts"}` : "Full clip · no cuts yet";
    const dirty = ready && signature() !== state.baseline && signature() !== state.exportedSignature;
    if (dirty !== state.reportedDirty) { state.reportedDirty = dirty; nativeCall("editor_dirty", dirty).catch(() => {}); }
    $("edit-state").textContent = state.exportStatus === "running" ? "Exporting…" : !ready ? "Loading" : dirty ? "Unexported edits" : state.exportedSignature && signature() === state.exportedSignature ? "Exported" : "No changes";
    $("edit-dot").className = `state-dot${state.exportStatus === "running" ? " busy" : dirty ? " dirty" : state.exportedSignature ? " ready" : ""}`;
    renderExport();
  }
  function changed() {
    if (state.exportStatus === "done" || state.exportStatus === "failed" || state.exportStatus === "canceled") {
      if (signature() !== state.exportedSignature) state.exportStatus = "idle";
    }
    renderControls(); renderTime(); requestDraw();
  }
  function setQuality(value) {
    value = Math.min(10, Math.max(1, Math.round(Number(value) || 8)));
    state.quality = value;
    $("quality").value = String(value);
    $("quality-value").textContent = String(value);
    $("quality-label").textContent = QUALITY_NAMES[value - 1];
    $("quality").setAttribute("aria-valuetext", `${value} of 10, ${QUALITY_NAMES[value - 1]}`);
    [...$("quality-steps").children].forEach((step, index) => {
      step.className = index + 1 === value ? "current" : index < value ? "on" : "";
    });
  }
  for (let i = 0; i < 10; i++) { const step = document.createElement("span"); step.style.height = `${6 + i * 1.6}px`; $("quality-steps").append(step); }

  function renderExport() {
    const native = state.native;
    const tool = native?.ffmpeg || { status: "checking" };
    const job = native?.export || { status: "idle" };
    const status = state.exportStatus;
    const sourceMissing = Boolean(native?.source_missing);
    const probeFailed = native?.probe?.status === "failed";
    const ffmpegBlocked = tool.status === "missing" || tool.status === "invalid" || sourceMissing || probeFailed;
    const bar = $("export-bar");
    const mode = status === "running" ? "running" : (status === "done" || status === "failed") ? "result" : ffmpegBlocked ? "ffmpeg" : "idle";
    bar.dataset.state = mode;
    $("export-idle").hidden = mode !== "idle";
    $("ffmpeg-notice").hidden = mode !== "ffmpeg";
    $("export-progress").hidden = mode !== "running";
    $("export-result").hidden = mode !== "result";
    if (mode === "ffmpeg") {
      const toolProblem = tool.status === "missing" || tool.status === "invalid";
      $("ffmpeg-title").textContent = sourceMissing ? "The original replay is gone" : probeFailed && !toolProblem ? "FFmpeg couldn't read this clip" : tool.status === "invalid" ? "That file isn't a usable FFmpeg" : "FFmpeg is needed to export";
      $("ffmpeg-text").textContent = sourceMissing ? "It was moved or deleted while the editor was open, so it can't be exported. Close the editor." : probeFailed && !toolProblem ? (native.probe.message || "Its video details couldn't be read.") + " Try again, or open the file in another app." : tool.status === "invalid" ? (tool.message || "Choose ffmpeg.exe from a full FFmpeg build.") : "Editing works now. Install FFmpeg with the command below, or locate ffmpeg.exe.";
      $("ffmpeg-help").hidden = $("ffmpeg-locate").hidden = $("ffmpeg-copy").hidden = sourceMissing || (probeFailed && !toolProblem);
      $("ffmpeg-copy").hidden = $("ffmpeg-copy").hidden || tool.status === "invalid";
      $("probe-retry").hidden = !(probeFailed && !toolProblem && !sourceMissing);
    }
    if (sourceMissing) $("stage-message").textContent = "The original replay was moved or deleted.";
    const probeReady = native?.probe?.status === "ready";
    const length = keptLength();
    const exportDisabledReason = !state.mediaReady ? "Loading clip" : tool.status === "checking" ? "Looking for FFmpeg…" : !probeReady ? "Analyzing clip…" : length < 0.1 ? "Nothing to export" : "";
    $("quality").disabled = !state.mediaReady;
    $("export").disabled = Boolean(exportDisabledReason);
    $("export").title = exportDisabledReason || "Export the kept sections  Ctrl+E";
    $("export-format").textContent = tool.status !== "ready" ? exportDisabledReason || "MP4" : exportEstimate(length);
    if (mode === "running") {
      const progress = Math.max(0, Math.min(1, Number(job.progress) || 0));
      const percent = Math.round(progress * 100);
      $("progress-title").textContent = progress > 0 ? `Exporting… ${percent}%` : "Starting export…";
      const speed = Number(job.speed) > 0 ? `${Number(job.speed).toFixed(1)}× speed` : "";
      const eta = Number(job.eta) > 0 ? `about ${job.eta < 60 ? `${Math.max(1, Math.ceil(job.eta))} s` : formatTime(Math.ceil(job.eta), false)} left` : "";
      $("progress-detail").textContent = [`${formatLength(Number(job.total) || length)} video`, job.encoder, speed, eta].filter(Boolean).join(" · ");
      $("progress-fill").style.width = `${percent}%`;
      $("progress-bar").setAttribute("aria-valuenow", String(percent));
      $("export-progress").classList.toggle("indeterminate", progress <= 0);
    }
    if (mode === "result") {
      const failed = status === "failed";
      $("export-result").classList.toggle("failed", failed);
      $("result-mark").innerHTML = failed ? '<svg viewBox="0 0 20 20"><path d="M10 5.5v5.5M10 14.2v.3"/></svg>' : '<svg viewBox="0 0 20 20"><path d="m5 10.5 3.2 3.2L15 6.5"/></svg>';
      const last = state.lastExport || {};
      $("result-title").textContent = failed ? "Export failed" : `Exported ${last.name || "video"}`;
      $("result-text").textContent = failed ? (last.error || "FFmpeg stopped unexpectedly.") : [formatBytes(last.bytes), formatLength(last.duration), last.revealed ? "Shown in File Explorer" : "Saved in Edited Exports", last.note].filter(Boolean).join(" · ");
      $("result-reveal").hidden = failed; $("result-open").hidden = failed;
      $("result-dismiss").textContent = failed ? "Dismiss" : "Done";
    }
  }

  // Estimated size and format of the export at the selected quality, from the native quality plans.
  function exportEstimate(length) {
    const plan = state.native?.quality_plans?.[state.quality - 1];
    if (!plan || !(length > 0)) return "MP4";
    const tracks = Number(state.native?.probe?.audio_tracks) || 0;
    const bytes = length * (plan.video_kbps + plan.audio_kbps * tracks) * 1000 / 8 * 1.01;
    return `≈ ${formatBytes(bytes)} · ${plan.original ? `original ${plan.codec}` : `${plan.codec}${plan.height ? ` ${plan.height}p` : ""}`}`;
  }

  async function startExport() {
    if (!state.mediaReady || $("export").disabled || state.exportStatus === "running") return;
    video.pause();
    const segments = keptSegments();
    try {
      await nativeCall("start_export", JSON.stringify({ segments, quality: state.quality }));
      state.exportStatus = "running";
      state.exportSignature = signature();
      announce("Export started");
      renderControls(); schedulePoll(0);
    } catch (error) {
      state.exportStatus = "failed"; state.lastExport = { error: errorText(error) };
      renderControls();
    }
  }

  // ---------- Preferences ----------
  function savePref(key, value) { nativeCall("save_pref", key, JSON.stringify(value)).catch(() => {}); }
  function renderScale() {
    $("scale-value").textContent = `${Math.round(state.scale * 100)}%`;
    $("scale-button").setAttribute("aria-label", `Interface scale, ${Math.round(state.scale * 100)}%`);
  }
  function loadPrefs(prefs = {}) {
    if (state.prefsLoaded) return;
    state.prefsLoaded = true;
    if (["duration", "clock", "smpte"].includes(prefs.timecode)) state.timecode = prefs.timecode;
    if ([0.75, 1, 1.25, 1.5].includes(prefs.scale)) state.scale = prefs.scale;
    const master = Number(prefs.volume_master);
    if (Number.isFinite(master) && master >= 0 && master <= 1) state.master = master;
    if (prefs.track_volumes && typeof prefs.track_volumes === "object") state.savedTrackLevels = { ...prefs.track_volumes };
    if (typeof prefs.preview_cuts === "boolean") { state.skipRemoved = prefs.preview_cuts; $("skip-removed").checked = prefs.preview_cuts; }
    renderScale(); applyVolumes();
  }
  // Per-track sliders use the names stored in the recording. When the first track is a mix of the
  // others (OBS: Mix, Desktop, Microphone), only the individual tracks are listed. Mirrors native.
  function setTracks(count, names = []) {
    const first = String(names[0] || "").toLowerCase();
    const from = count >= 3 || (count === 2 && first === "mix") ? 1 : 0;
    state.tracks = [];
    if (count >= 2) for (let index = from; index < count && state.tracks.length < 6; index++) {
      const name = String(names[index] || "").trim() || `Track ${index + 1}`;
      const saved = Number(state.savedTrackLevels[name]);
      state.tracks.push({ index, name, level: Number.isFinite(saved) && saved >= 0 && saved <= 1 ? saved : 1, muted: false, audio: null });
    }
    renderTracks();
  }
  function renderTracks() {
    const list = $("tracks-list");
    list.replaceChildren();
    if (!state.tracks.length) {
      const empty = document.createElement("p");
      empty.className = "tracks-empty";
      empty.textContent = state.tracksFromProbe ? "This recording has a single audio track. Use the main volume." : "Reading the recording's audio tracks…";
      list.append(empty);
      return;
    }
    for (const track of state.tracks) {
      const row = document.createElement("div");
      row.className = "track-row"; row.dataset.track = String(track.index);
      const mute = document.createElement("button");
      mute.type = "button"; mute.className = "icon-button mute";
      mute.setAttribute("aria-label", `Mute ${track.name}`);
      mute.dataset.tip = `Mute ${track.name}`;
      mute.innerHTML = '<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M3.5 8h3l4-3.5v11l-4-3.5h-3z"/><path class="volume-wave" d="M13.5 7.5a3.5 3.5 0 0 1 0 5M15.5 5.5a6.5 6.5 0 0 1 0 9"/><path class="volume-off" d="m13.5 8 4 4m0-4-4 4"/></svg>';
      mute.addEventListener("click", () => { track.muted = !track.muted; applyVolumes(); });
      const id = `track-volume-${track.index}`;
      const label = document.createElement("label");
      label.htmlFor = id; label.textContent = track.name; label.title = track.name;
      const slider = document.createElement("input");
      slider.type = "range"; slider.id = id; slider.min = "0"; slider.max = "1"; slider.step = "0.05";
      slider.addEventListener("input", () => { track.level = Number(slider.value); track.muted = false; applyVolumes(); });
      slider.addEventListener("change", () => {
        state.savedTrackLevels[track.name] = track.level;
        savePref("track_volumes", state.savedTrackLevels);
      });
      const value = document.createElement("output");
      value.htmlFor = id;
      row.append(mute, label, slider, value);
      list.append(row);
    }
    applyVolumes();
  }
  function placeTracksPanel() {
    const panel = $("tracks-panel"), anchor = $("tracks-button").getBoundingClientRect(), box = panel.getBoundingClientRect();
    panel.style.left = `${Math.max(8, Math.min(innerWidth - box.width - 8, anchor.right - box.width))}px`;
    panel.style.top = `${Math.max(8, anchor.top - box.height - 8)}px`;
  }
  function toggleTracksPanel(open = $("tracks-panel").hidden) {
    const panel = $("tracks-panel");
    if (!open) { if (!panel.hidden) { panel.hidden = true; $("tracks-button").setAttribute("aria-expanded", "false"); } return; }
    closeMenu();
    renderTracks();
    panel.hidden = false;
    $("tracks-button").setAttribute("aria-expanded", "true");
    placeTracksPanel();
    (panel.querySelector("input") || panel).focus?.();
  }
  async function useGeneratedPreview() {
    if (state.previewRequested) return;
    state.previewRequested = true;
    let urls;
    try { urls = JSON.parse(await nativeCall("preview_urls")); } catch { return; }
    for (const stem of urls.stems || []) {
      const track = trackFor(stem.track);
      if (!track || track.audio) continue;
      const audio = new Audio();
      audio.preload = "auto"; audio.crossOrigin = "anonymous"; audio.src = stem.url;
      track.audio = audio;
      state.stems.push({ track: stem.track, audio });
    }
    applyVolumes(); syncStems(true);
    if (urls.video && !state.previewSwapped) {
      state.previewSwapped = true;
      const position = currentTime(), playing = !video.paused, original = video.src;
      const restore = () => { video.currentTime = position; if (playing) video.play().catch(() => {}); };
      video.addEventListener("loadedmetadata", restore, { once: true });
      video.addEventListener("error", () => { if (video.src !== original) { video.src = original; video.addEventListener("loadedmetadata", restore, { once: true }); } }, { once: true });
      video.addEventListener("loadedmetadata", () => { if (!state.thumbsRequested) buildThumbnails(); }, { once: true });
      video.src = urls.video;
      video.load();
    }
  }
  function renderPreview(preview = {}) {
    const building = preview.status === "building" || (preview.status === "pending" && state.mediaReady && state.native?.ffmpeg?.status === "ready");
    $("preview-status").hidden = !building;
    if (building) {
      const percent = Math.round((Number(preview.progress) || 0) * 100);
      $("preview-text").textContent = preview.stage === "video" ? `Preparing smooth scrubbing… ${percent}%` : "Preparing audio tracks…";
    }
  }

  // ---------- Menus (scale, time format) ----------
  let menuAnchor = null, menuSelect = null;
  function closeMenu(focusAnchor = false) {
    if ($("menu").hidden) return;
    $("menu").hidden = true;
    menuAnchor?.setAttribute("aria-expanded", "false");
    if (focusAnchor) menuAnchor?.focus();
    menuAnchor = menuSelect = null;
  }
  function openMenu(anchor, options, current, onSelect) {
    if (menuAnchor === anchor) { closeMenu(true); return; }
    closeMenu();
    const menu = $("menu");
    menu.replaceChildren(...options.map(option => {
      const item = document.createElement("button");
      item.type = "button"; item.setAttribute("role", "option");
      item.setAttribute("aria-selected", String(option.value === current));
      item.dataset.value = String(option.value);
      const label = document.createElement("span"); label.textContent = option.label; item.append(label);
      if (option.hint) { const hint = document.createElement("small"); hint.textContent = option.hint; item.append(hint); }
      item.addEventListener("click", () => { closeMenu(true); onSelect(option.value); });
      return item;
    }));
    menu.hidden = false;
    menuAnchor = anchor; menuSelect = onSelect;
    anchor.setAttribute("aria-expanded", "true");
    const rect = anchor.getBoundingClientRect(), box = menu.getBoundingClientRect();
    const below = rect.bottom + 6 + box.height <= innerHeight;
    menu.style.left = `${Math.min(innerWidth - box.width - 8, Math.max(8, rect.left))}px`;
    menu.style.top = `${below ? rect.bottom + 6 : Math.max(8, rect.top - box.height - 6)}px`;
    (menu.querySelector('[aria-selected="true"]') || menu.firstElementChild)?.focus();
  }
  $("menu").addEventListener("keydown", event => {
    const items = [...$("menu").children];
    const index = items.indexOf(document.activeElement);
    if (event.key === "ArrowDown" || event.key === "ArrowUp") { event.preventDefault(); items[(index + (event.key === "ArrowDown" ? 1 : items.length - 1)) % items.length].focus(); }
    else if (event.key === "Home" || event.key === "End") { event.preventDefault(); items[event.key === "Home" ? 0 : items.length - 1].focus(); }
    else if (event.key === "Escape" || event.key === "Tab") { event.preventDefault(); closeMenu(true); }
  });
  document.addEventListener("pointerdown", event => { if (!$("menu").hidden && !$("menu").contains(event.target) && !menuAnchor?.contains(event.target)) closeMenu(); }, true);
  window.addEventListener("blur", () => closeMenu());

  // ---------- Native state ----------
  function applyNative(native) {
    state.native = native;
    if (!state.clip) {
      state.clip = native.clip;
      $("clip-title").textContent = native.clip?.name || "Replay";
      document.title = `Edit ${native.clip?.name || "replay"} – OBSReplay`;
      setQuality(native.quality);
      loadPrefs(native.prefs);
      document.documentElement.dataset.motion = native.animations === false ? "off" : "on";
      loadMedia();
    }
    const probe = native.probe || {};
    if (probe.status === "ready") {
      if (!state.tracksFromProbe && !state.stems.length) { state.tracksFromProbe = true; setTracks(probe.audio_tracks || 0, probe.audio_names || []); }
      if (probe.fps > 1) state.fps = probe.fps;
      if (probe.width && probe.height) state.aspect = probe.width / probe.height;
    }
    if (native.waveform?.status === "ready" && !state.waveRequested) {
      state.waveRequested = true;
      state.waveRate = native.waveform.rate || 100;
      nativeCall("editor_waveform").then(data => {
        if (!data) return;
        const binary = atob(data);
        const values = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; i++) values[i] = binary.charCodeAt(i);
        state.waveform = values; requestDraw();
      }).catch(() => {});
    }
    renderPreview(native.preview);
    if (state.mediaReady && !state.thumbsRequested && ["failed", "unavailable"].includes(native.preview?.status)) buildThumbnails();
    if (native.preview?.status === "ready" && state.mediaReady) useGeneratedPreview();
    const job = native.export || {};
    if (state.exportStatus === "running" && job.status && job.status !== "running") {
      if (job.status === "done") {
        state.exportStatus = "done"; state.lastExport = job; state.exportedSignature = state.exportSignature;
        announce(`Exported ${job.name}. File Explorer opened with the video selected.`);
      } else if (job.status === "failed") { state.exportStatus = "failed"; state.lastExport = job; announce("Export failed"); }
      else { state.exportStatus = "idle"; flash("Export canceled. No file was saved."); }
    }
    renderControls();
  }
  async function poll() {
    clearTimeout(pollTimer);
    try {
      const native = JSON.parse(await nativeCall("editor_state"));
      if (native.revision !== state.revision || !state.clip) { state.revision = native.revision; applyNative(native); }
      else if (state.native) { state.native.ffmpeg = native.ffmpeg; renderExport(); }
    } catch (error) {
      $("stage-message").textContent = errorText(error);
    }
    const native = state.native;
    const settling = state.exportStatus === "running" || native?.ffmpeg?.status === "checking" || native?.probe?.status === "pending" || native?.waveform?.status === "pending" || ["pending", "building"].includes(native?.preview?.status);
    schedulePoll(state.exportStatus === "running" ? 250 : settling ? 500 : 1500);
  }
  function schedulePoll(delay) { clearTimeout(pollTimer); pollTimer = setTimeout(poll, delay); }

  async function loadMedia() {
    try {
      const url = await nativeCall("media_url", state.clip.path);
      video.src = url;
      video.load();
    } catch (error) { $("stage-message").textContent = errorText(error); }
  }
  video.addEventListener("loadedmetadata", () => {
    if (state.mediaReady) return;
    state.duration = Number.isFinite(video.duration) ? video.duration : 0;
    if (!state.duration) { $("stage-message").textContent = "This video has no readable length. Try Open file from the replay list."; return; }
    if (video.videoWidth && video.videoHeight) state.aspect = video.videoWidth / video.videoHeight;
    state.mediaReady = true;
    $("stage-message").textContent = "";
    $("timeline").setAttribute("aria-valuemax", state.duration.toFixed(2));
    resetPieces();
    setView(0, state.duration);
    applyVolumes();
    changed();
    maybeBuildThumbnails();
    if (state.native?.preview?.status === "ready") useGeneratedPreview();
  });
  video.addEventListener("error", () => {
    if (!video.getAttribute("src")) return;
    $("stage-message").textContent = "Windows could not play this video, so it can't be edited here. HEVC and AV1 clips need the matching Windows video extension.";
    state.mediaReady = false; renderControls();
  });
  video.addEventListener("playing", () => syncStems(true));
  video.addEventListener("play", () => { $("play").classList.add("playing"); $("play").setAttribute("aria-label", "Pause (Space)"); if (!playFrame) playFrame = requestAnimationFrame(playLoop); });
  video.addEventListener("pause", () => { syncStems(true); });
  video.addEventListener("pause", () => { $("play").classList.remove("playing"); $("play").setAttribute("aria-label", "Play (Space)"); renderTime(); requestDraw(); });
  // The latest seek always wins: any seek that isn't the queued scrub target drops the queue.
  video.addEventListener("seeking", () => {
    if (state.pendingSeek != null && !state.drag && Math.abs(video.currentTime - state.pendingSeek) > 0.0005) { state.pendingSeek = null; state.scrubTime = null; }
  });
  video.addEventListener("seeked", () => syncStems(true));
  video.addEventListener("seeked", () => {
    if (state.pendingSeek != null) { const next = state.pendingSeek; state.pendingSeek = null; video.currentTime = next; if (!state.drag) state.scrubTime = null; }
    else state.scrubTime = null;
    renderTime(); requestDraw();
  });
  video.addEventListener("ended", () => { renderTime(); requestDraw(); });


  // The filmstrip decodes from the seek-friendly preview when one is coming; otherwise from the clip.
  function maybeBuildThumbnails() {
    const preview = state.native?.preview?.status;
    if (state.previewSwapped || !["pending", "building"].includes(preview) || state.native?.ffmpeg?.status !== "ready") buildThumbnails();
  }
  async function buildThumbnails() {
    if (state.thumbsRequested) return;
    state.thumbsRequested = true;
    const probe = document.createElement("video");
    probe.muted = true; probe.preload = "auto"; probe.crossOrigin = "anonymous";
    probe.src = video.currentSrc || video.src;
    await new Promise(resolve => { probe.onloadeddata = resolve; probe.onerror = resolve; setTimeout(resolve, 8000); });
    if (!probe.videoWidth) return;
    const count = Math.round(Math.min(120, Math.max(16, state.duration / 1.5)));
    const height = 90, width = Math.round(height * probe.videoWidth / probe.videoHeight);
    const surface = document.createElement("canvas"); surface.width = width; surface.height = height;
    const draw2d = surface.getContext("2d");
    // Coarse pass first so the whole strip fills quickly, then refine.
    const order = [];
    for (let stride = 8; stride >= 1; stride = Math.floor(stride / 2)) for (let i = 0; i < count; i += stride) if (!order.includes(i)) order.push(i);
    for (const i of order) {
      if (state.closing) break;
      const time = Math.min(state.duration - 0.05, (i + 0.5) / count * state.duration);
      const ok = await new Promise(resolve => {
        const timer = setTimeout(() => resolve(false), 4000);
        probe.onseeked = () => { clearTimeout(timer); resolve(true); };
        probe.currentTime = Math.max(0, time);
      });
      if (!ok) continue;
      try {
        draw2d.drawImage(probe, 0, 0, width, height);
        const bitmap = await createImageBitmap(surface);
        state.thumbs.push({ time, bitmap });
        requestDraw();
      } catch { break; }
    }
    probe.removeAttribute("src"); probe.load();
  }

  // ---------- Commands ----------
  $("play").addEventListener("click", togglePlay);
  $("play").addEventListener("dblclick", playFromStart);
  for (const [id, direction] of [["frame-back", -1], ["frame-forward", 1]]) {
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
    button.addEventListener("click", event => { if (event.detail === 0) frameStep(direction); });
  }
  $("prev-cut").addEventListener("click", () => jumpCut(-1));
  $("next-cut").addEventListener("click", () => jumpCut(1));
  $("split").addEventListener("click", split);
  $("toggle-remove").addEventListener("click", () => toggleRemove());
  $("trim-start").addEventListener("click", () => trim(true));
  $("trim-end").addEventListener("click", () => trim(false));
  $("undo").addEventListener("click", undo);
  $("redo").addEventListener("click", redo);
  $("zoom-fit").addEventListener("click", () => setView(0, state.duration));
  $("volume-master").addEventListener("input", () => { state.master = Number($("volume-master").value); state.masterMuted = false; applyVolumes(); });
  $("volume-master").addEventListener("change", () => savePref("volume_master", state.master));
  $("mute-master").addEventListener("click", () => { state.masterMuted = !state.masterMuted; if (!state.masterMuted && state.master === 0) state.master = 0.5; applyVolumes(); });
  $("tracks-button").addEventListener("click", () => toggleTracksPanel());
  $("tracks-panel").addEventListener("keydown", event => { if (event.key === "Escape") { event.preventDefault(); event.stopPropagation(); toggleTracksPanel(false); $("tracks-button").focus(); } });
  document.addEventListener("pointerdown", event => { if (!$("tracks-panel").hidden && !$("tracks-panel").contains(event.target) && !$("tracks-button").contains(event.target)) toggleTracksPanel(false); }, true);
  window.addEventListener("resize", () => { if (!$("tracks-panel").hidden) placeTracksPanel(); });
  $("skip-removed").addEventListener("change", () => {
    state.skipRemoved = $("skip-removed").checked;
    savePref("preview_cuts", state.skipRemoved);
    announce(state.skipRemoved ? "Preview skips removed sections" : "Preview plays the full clip");
  });
  $("quality").addEventListener("input", () => { setQuality($("quality").value); renderExport(); });
  $("quality").addEventListener("change", () => savePref("quality", state.quality));
  $("scale-button").addEventListener("click", () => openMenu($("scale-button"), [0.75, 1, 1.25, 1.5].map(value => ({ value, label: `${Math.round(value * 100)}%` })), state.scale, value => {
    state.scale = value; renderScale(); savePref("scale", value);
  }));
  $("result-summary").addEventListener("click", () => {
    const sample = state.mediaReady ? state.duration : 94.8;
    openMenu($("result-summary"), TIME_FORMATS.map(format => ({ value: format.id, label: format.label, hint: formatAs(format.id, sample, 1) })), state.timecode, value => {
      state.timecode = value; savePref("timecode", value); renderControls(); renderTime();
    });
  });
  $("export").addEventListener("click", startExport);
  $("cancel-export").addEventListener("click", () => nativeCall("cancel_export").catch(() => {}));
  $("result-reveal").addEventListener("click", () => nativeCall("reveal_output", true).catch(error => flash(errorText(error))));
  $("result-open").addEventListener("click", () => nativeCall("reveal_output", false).catch(error => flash(errorText(error))));
  $("result-dismiss").addEventListener("click", () => { state.exportStatus = "idle"; renderControls(); });
  $("ffmpeg-locate").addEventListener("click", async () => {
    try { if (await nativeCall("pick_ffmpeg")) schedulePoll(100); } catch (error) { flash(errorText(error)); }
  });
  $("ffmpeg-help").addEventListener("click", () => nativeCall("ffmpeg_help").catch(() => {}));
  $("ffmpeg-copy").addEventListener("click", async () => {
    try { await navigator.clipboard.writeText("winget install Gyan.FFmpeg"); flash("Copied: winget install Gyan.FFmpeg. Run it in Terminal, then reopen the editor."); }
    catch { flash("Run in Terminal: winget install Gyan.FFmpeg"); }
  });
  $("probe-retry").addEventListener("click", () => { nativeCall("retry_analysis").then(() => schedulePoll(200)).catch(() => {}); });
  // Sliders and buttons give the keyboard back to the timeline after a pointer interaction.
  for (const element of document.querySelectorAll(".toolbar button, .transport button:not(#tracks-button), #volume-master, #quality, #skip-removed"))
    element.addEventListener("pointerup", () => setTimeout(() => { if (document.activeElement === element) element.blur(); }, 0));
  $("shortcuts-button").addEventListener("click", () => $("shortcuts-dialog").showModal());
  $("shortcuts-close").addEventListener("click", () => $("shortcuts-dialog").close());
  $("shortcuts-dialog").addEventListener("click", event => { if (event.target === $("shortcuts-dialog")) $("shortcuts-dialog").close(); });
  for (const button of document.querySelectorAll("[data-window]")) button.addEventListener("click", async event => {
    if (event.detail !== 0 || !window.saucer) return;
    if (button.dataset.window === "minimize") await window.saucer.minimize(true);
    else if (button.dataset.window === "maximize") await window.saucer.maximize(!(await window.saucer.maximized()));
    else requestClose();
  });

  function confirm(title, text, action) {
    const dialog = $("confirm-dialog");
    $("confirm-title").textContent = title; $("confirm-text").textContent = text; $("confirm-action").textContent = action;
    dialog.returnValue = "cancel";
    dialog.showModal();
    dialog.querySelector('[value="cancel"]').focus();
    return new Promise(resolve => dialog.addEventListener("close", () => resolve(dialog.returnValue === "confirm"), { once: true }));
  }
  async function requestClose() {
    if (state.closing || $("confirm-dialog").open) return;
    const dirty = state.mediaReady && signature() !== state.baseline && signature() !== state.exportedSignature;
    if (state.exportStatus === "running") {
      if (!await confirm("Cancel export and close?", "The export in progress will stop and its partial file will be deleted.", "Cancel export and close")) return;
    } else if (dirty) {
      if (!await confirm("Close without exporting?", "Your cuts haven't been exported. Closing the editor discards them. The original replay is never changed.", "Discard edits")) return;
    }
    state.closing = true;
    video.pause(); video.removeAttribute("src"); video.load();
    nativeCall("close_editor").catch(() => { state.closing = false; });
  }
  window.addEventListener("editor:close-request", requestClose);

  document.addEventListener("keydown", event => {
    if (event.defaultPrevented || $("confirm-dialog").open || !$("menu").hidden) return;
    if (!$("tracks-panel").hidden && $("tracks-panel").contains(event.target)) return;
    if ($("shortcuts-dialog").open) { if (event.key === "?") $("shortcuts-dialog").close(); return; }
    const target = event.target;
    const typing = target instanceof HTMLInputElement && target.type !== "checkbox";
    if (typing && ["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End", "PageUp", "PageDown"].includes(event.key)) return;
    const key = event.key, ctrl = event.ctrlKey || event.metaKey;
    let handled = true;
    if (ctrl && key.toLowerCase() === "z") event.shiftKey ? redo() : undo();
    else if (ctrl && key.toLowerCase() === "y") redo();
    else if (ctrl && key.toLowerCase() === "a") { if (state.mediaReady) { state.selected = new Set(state.pieces.map(piece => piece.id)); renderControls(); requestDraw(); } }
    else if (ctrl && key.toLowerCase() === "e") startExport();
    else if (ctrl) handled = false;
    else if (key === " " || key.toLowerCase() === "k") togglePlay();
    else if (key === "ArrowLeft" || key === "ArrowRight") { video.pause(); seek(currentTime() + (key === "ArrowLeft" ? -1 : 1) * (event.shiftKey ? 5 : 1)); }
    else if (key === ",") frameStep(-1);
    else if (key === ".") frameStep(1);
    else if (key === "ArrowUp") jumpCut(-1);
    else if (key === "ArrowDown") jumpCut(1);
    else if (key === "Home") seek(0);
    else if (key === "End") seek(state.duration);
    else if (key.toLowerCase() === "s") split();
    else if (key === "Delete" || key === "Backspace" || key.toLowerCase() === "d") toggleRemove("remove");
    else if (key.toLowerCase() === "r") toggleRemove("restore");
    else if (key.toLowerCase() === "i") trim(true);
    else if (key.toLowerCase() === "o") trim(false);
    else if (key.toLowerCase() === "m") { state.masterMuted = !state.masterMuted; applyVolumes(); announce(state.masterMuted ? "Muted" : "Unmuted"); }
    else if (key === "+" || key === "=") zoomBy(1.6);
    else if (key === "-" || key === "_") zoomBy(1 / 1.6);
    else if (key === "0") setView(0, state.duration);
    else if (key === "?") $("shortcuts-dialog").showModal();
    else if (key === "Escape") { if (state.selected.size) { state.selected.clear(); renderControls(); requestDraw(); } else handled = false; }
    else handled = false;
    if (handled) event.preventDefault();
  });
  document.addEventListener("visibilitychange", () => { if (document.hidden) video.pause(); });
  // Ctrl+wheel and pinch never rescale the interface (use the Scale menu); on the timeline they zoom it instead.
  window.addEventListener("wheel", event => { if (event.ctrlKey) event.preventDefault(); }, { passive: false, capture: true });

  setQuality(8);
  renderControls();
  renderTime();
  requestDraw();
  poll();
})();
