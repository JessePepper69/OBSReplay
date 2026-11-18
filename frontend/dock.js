"use strict";

// Fullscreen video with docked controls, shared by the replay viewer and the clip editor.
// While `root` is fullscreen, `dock` (the transport and timeline) sits below the screen edge with a progress strip
// peeking out. Pointing at the strip slides the controls up; 250 ms after the pointer leaves they slide back down.
// They stay up while `held()` says so (a drag on the timeline, an open panel) and while Tab has put focus in them.
// Shortcut keys pressed with the timeline focused do not count: they would keep the controls up for good.
//   onChange(fullscreen)   called after the page enters or leaves fullscreen
//   progress()             0 to 1, the playhead position the strip shows
// Returns { toggle, active, render }: render() redraws the strip, call it when the playhead moves.
window.videoDock = (root, dock, { held = () => false, onChange = () => {}, progress = () => 0 } = {}) => {
  const RETRACT_DELAY = 250;
  const peek = document.createElement("div");
  peek.className = "dock-peek";
  peek.setAttribute("aria-hidden", "true");
  dock.prepend(peek);
  let timer = 0, tabbing = false, tabbed = false;
  const active = () => document.fullscreenElement === root;
  const busy = () => held() || tabbed || dock.matches(":hover");
  function open() {
    clearTimeout(timer); timer = 0;
    dock.classList.add("open");
  }
  function retract() {
    clearTimeout(timer);
    timer = setTimeout(function check() {
      if (!active()) return;
      if (busy()) { timer = setTimeout(check, RETRACT_DELAY); return; }
      timer = 0;
      dock.classList.remove("open");
    }, RETRACT_DELAY);
  }
  function render() {
    const value = Math.min(1, Math.max(0, Number(progress()) || 0));
    peek.style.setProperty("--played", value.toFixed(4));
  }
  function toggle() {
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    else root.requestFullscreen().catch(() => {});
  }
  dock.addEventListener("pointerenter", () => { if (active()) open(); });
  dock.addEventListener("pointerleave", () => { if (active()) retract(); });
  document.addEventListener("keydown", event => { tabbing = event.key === "Tab"; }, true);
  document.addEventListener("pointerdown", () => { tabbing = tabbed = false; }, true);
  dock.addEventListener("focusin", () => { if (tabbing) tabbed = true; if (tabbed && active()) open(); });
  dock.addEventListener("focusout", event => { if (dock.contains(event.relatedTarget)) return; tabbed = false; if (active()) retract(); });
  document.addEventListener("fullscreenchange", () => {
    clearTimeout(timer); timer = 0;
    const on = active();
    // Entering shows the controls for a moment and then lowers them, so the strip reads as the controls' handle.
    dock.classList.toggle("open", on);
    if (on) retract();
    render();
    onChange(on);
  });
  render();
  return { toggle, active, render };
};
