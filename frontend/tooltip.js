"use strict";

// Custom tooltips for every element with a tooltip, in both windows.
// Sources (first match wins): data-tip-title + data-tip (rich), data-tip, or a plain title attribute.
// A title written as "Label  Key" (two spaces before the shortcut) shows the shortcut as key caps;
// data-tip-keys does the same explicitly. Native title tooltips are suppressed while ours are used.
(() => {
  const tip = document.createElement("div");
  tip.id = "app-tooltip";
  tip.className = "app-tooltip";
  tip.setAttribute("role", "tooltip");
  tip.hidden = true;
  let target = null, showTimer = 0, hideTimer = 0, lastHidden = 0;

  function adopt(element) {
    const title = element.getAttribute("title");
    if (title) {
      const split = title.split(/\s{2,}/);
      // A title set (or updated) by the page always wins over the previous text.
      element.dataset.tip = split[0];
      if (split[1]) element.dataset.tipKeys = split.slice(1).join(" ");
      else if (!element.dataset.tipKeysFixed) delete element.dataset.tipKeys;
      element.removeAttribute("title");
    }
    return element.dataset.tip || element.dataset.tipTitle ? element : null;
  }
  function source(node) {
    for (let element = node instanceof Element ? node : null; element && element !== document.body; element = element.parentElement) {
      if (element.hasAttribute("title") || element.dataset?.tip || element.dataset?.tipTitle) return adopt(element);
    }
    return null;
  }
  function render(element) {
    tip.replaceChildren();
    if (element.dataset.tipTitle) {
      const heading = document.createElement("strong");
      heading.textContent = element.dataset.tipTitle;
      tip.append(heading);
    }
    if (element.dataset.tip) {
      const body = document.createElement("span");
      body.textContent = element.dataset.tip;
      tip.append(body);
    }
    if (element.dataset.tipKeys) {
      const keys = document.createElement("span");
      keys.className = "tip-keys";
      for (const key of element.dataset.tipKeys.split(/\s+/)) {
        const cap = document.createElement("kbd");
        cap.textContent = key;
        keys.append(cap);
      }
      tip.append(keys);
    }
    tip.classList.toggle("rich", Boolean(element.dataset.tipTitle));
  }
  function place(element) {
    const rect = element.getBoundingClientRect();
    tip.style.left = "0px"; tip.style.top = "0px";
    const box = { width: tip.offsetWidth, height: tip.offsetHeight };
    const gap = 8;
    let top = rect.top - box.height - gap;
    let below = false;
    if (top < 6) { top = rect.bottom + gap; below = true; }
    const left = Math.min(innerWidth - box.width - 6, Math.max(6, rect.left + rect.width / 2 - box.width / 2));
    tip.style.left = `${Math.round(left)}px`;
    tip.style.top = `${Math.round(Math.min(top, innerHeight - box.height - 6))}px`;
    tip.style.setProperty("--arrow", `${Math.round(Math.min(box.width - 12, Math.max(12, rect.left + rect.width / 2 - left)))}px`);
    tip.dataset.side = below ? "below" : "above";
  }
  function show(element) {
    clearTimeout(hideTimer);
    target = element;
    render(element);
    tip.hidden = false;
    tip.classList.remove("visible");
    place(element);
    element.setAttribute("aria-describedby", tip.id);
    requestAnimationFrame(() => tip.classList.add("visible"));
  }
  function hide(immediate = false) {
    clearTimeout(showTimer);
    if (!target) return;
    target.removeAttribute("aria-describedby");
    target = null;
    lastHidden = performance.now();
    tip.classList.remove("visible");
    if (immediate) tip.hidden = true;
    else hideTimer = setTimeout(() => { tip.hidden = true; }, 120);
  }
  function schedule(element, keyboard) {
    if (!element || element === target) return;
    clearTimeout(showTimer);
    // Moving between controls keeps tooltips flowing without the initial delay.
    const quick = keyboard || performance.now() - lastHidden < 400 || target;
    hide(true);
    showTimer = setTimeout(() => show(element), quick ? 60 : 450);
  }

  document.addEventListener("pointerover", event => {
    if (event.pointerType === "touch") return;
    const element = source(event.target);
    if (element) schedule(element, false);
    else if (target && !target.contains(event.target)) hide();
  });
  document.addEventListener("pointerout", event => {
    if (target && !target.contains(event.relatedTarget)) hide();
  });
  document.addEventListener("focusin", event => {
    if (!event.target.matches?.(":focus-visible")) return;
    const element = source(event.target);
    if (element) schedule(element, true);
  });
  document.addEventListener("focusout", () => hide());
  document.addEventListener("pointerdown", () => hide(true), true);
  document.addEventListener("keydown", event => { if (event.key === "Escape" && target) hide(true); }, true);
  window.addEventListener("blur", () => hide(true));
  window.addEventListener("scroll", () => hide(true), true);
  // Remove native titles up front so the browser's own tooltip never appears.
  const sweep = root => { for (const element of root.querySelectorAll?.("[title]") || []) adopt(element); };
  new MutationObserver(records => {
    for (const record of records) {
      if (record.type === "attributes" && record.target.hasAttribute("title")) {
        adopt(record.target);
        if (record.target === target) { render(target); place(target); }
      }
      for (const node of record.addedNodes) if (node.nodeType === 1) { if (node.hasAttribute("title")) adopt(node); sweep(node); }
    }
  }).observe(document.documentElement, { subtree: true, childList: true, attributes: true, attributeFilter: ["title"] });
  const start = () => { document.body.append(tip); sweep(document); };
  if (document.body) start(); else document.addEventListener("DOMContentLoaded", start, { once: true });
})();
