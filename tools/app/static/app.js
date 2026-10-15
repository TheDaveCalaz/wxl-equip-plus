"use strict";
// Backport App - talks to tools/app/server.py on this PC only.

const TOKEN = (location.hash.match(/t=([\w-]+)/) || [])[1] || sessionStorage.getItem("bpt") || "";
if (TOKEN) { try { sessionStorage.setItem("bpt", TOKEN); } catch (e) {} }

const EXP = ["Classic", "Burning Crusade", "Wrath", "Cataclysm", "Mists", "Warlords", "Legion", "Battle for Azeroth",
  "Shadowlands", "Dragonflight", "The War Within", "Midnight", "The Last Titan"];
const QUALITY = ["#9d9d9d", "#ffffff", "#1eff00", "#0070dd", "#a335ee", "#ff8000", "#e6cc80", "#00ccff", "#00ccff"];
const INV = {1: "Head", 2: "Neck", 3: "Shoulder", 4: "Shirt", 5: "Chest", 6: "Waist", 7: "Legs", 8: "Feet", 9: "Wrist",
  10: "Hands", 11: "Finger", 12: "Trinket", 13: "One-hand", 14: "Shield", 15: "Ranged", 16: "Back", 17: "Two-hand",
  19: "Tabard", 20: "Robe", 21: "Main hand", 22: "Off hand", 23: "Held in off-hand", 25: "Thrown", 26: "Ranged"};

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const esc = (t) => String(t ?? "").replace(/[&<>"']/g, (c) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));

async function api(path, body) {
  const opt = {headers: {"X-Token": TOKEN}};
  if (body !== undefined) { opt.method = "POST"; opt.body = JSON.stringify(body); opt.headers["Content-Type"] = "application/json"; }
  const r = await fetch("/api/" + path, opt);
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || ("HTTP " + r.status));
  return data;
}

function toast(text, ms = 3200) {
  const t = $("#toast"); t.textContent = text; t.classList.remove("hidden");
  clearTimeout(toast._t); toast._t = setTimeout(() => t.classList.add("hidden"), ms);
}

function lightbox(src) {
  const d = document.createElement("div"); d.className = "lightbox";
  d.innerHTML = `<img src="${esc(src)}" alt="">`; d.onclick = () => d.remove(); document.body.append(d);
}

const sheetUrl = (f) => "/sheet/" + encodeURIComponent(f) + "?t=" + encodeURIComponent(TOKEN);
const wowhead = (kind, id) => kind === "set" ? `https://www.wowhead.com/transmog-set=${id}` : `https://www.wowhead.com/item=${id}`;
const refreshWowhead = () => { try { window.$WowheadPower && window.$WowheadPower.refreshLinks(); } catch (e) {} };

// ------------------------------------------------------------------ navigation and state

let STATE = null;

function show(view) {
  $$(".nav").forEach((b) => b.classList.toggle("on", b.dataset.view === view));
  $$(".view").forEach((v) => v.classList.toggle("hidden", v.id !== "view-" + view));
  try { localStorage.setItem("bp-view", view); } catch (e) {}
  if (view === "project") loadProject();
  if (view === "build") loadGallery();
}

async function loadState() {
  try { STATE = await api("state"); } catch (e) { $("#side-status").textContent = "Not connected: " + e.message; return; }
  const s = STATE;
  $("#nav-warn").classList.toggle("hidden", !s.problems.length);
  const scan = s.steps.find((x) => x.step === "scan");
  $("#scan-banner").classList.toggle("hidden", !!s.problems.length || scan.state === "done");
  $("#steps").innerHTML = s.steps.map((x) =>
    `<li class="${x.state}"><span class="st"></span>${esc(x.words)}<span class="at">${x.state === "failed" ? "failed " : ""}${esc(x.at)}</span></li>`).join("");
  const i = s.installed || {};
  $("#facts").innerHTML = s.problems.length ? `<dt>Setup</dt><dd>Finish the settings first.</dd>` : [
    ["Sets patch", i.kit_patch ? `${esc(i.kit_patch)} ${i.kit_patch_installed ? "✓ installed" : "not installed"}` : "not built yet"],
    ["3D pieces patch", i.equip_plus_patches?.length ? esc(i.equip_plus_patches.join(", ")) + " ✓" : "not installed"],
    ["Equip Plus", i.equip_plus_dll ? "✓ installed" : "not installed"],
    ["Extended DBC", i.extended_dbc ? "✓ installed" : "not installed (needed for brought-back looks)"],
    ["Left out / brought back", `${i.left_out || 0} / ${i.brought_back || 0}`],
  ].map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join("");
  $("#undo-btn").disabled = !i.can_undo;
  $("#bring-back").classList.toggle("hidden", !i.left_out);
  const proj = (s.settings.work || "").split(/[\\/]/).filter(Boolean).pop() || "";
  $("#side-status").textContent = s.problems.length ? "Setup needed" : "Project: " + proj;
  $("#side-status").title = s.settings.work || "";
  fillSettings(s);
  if (s.job) attachJob(s.job);
}

// ------------------------------------------------------------------ find

let RESULTS = [];
const PICKED = new Map();   // key -> {kind, id, name}

function renderResults() {
  const sets = RESULTS.filter((r) => r.kind === "set"), items = RESULTS.filter((r) => r.kind === "item");
  const row = (r) => {
    const key = r.kind + ":" + r.id, on = PICKED.has(key);
    const exp = EXP[+r.exp] || ("expansion " + r.exp);
    const meta = r.kind === "set"
      ? `${exp} · set ${r.id}${r.parent !== "0" ? " · colour of " + esc(r.parent) : ""}`
      : `${INV[+r.inv] || "slot " + r.inv} · ${exp} · item ${r.id}`;
    const name = r.kind === "item"
      ? `<a class="r-name" href="${wowhead("item", r.id)}" target="_blank" rel="noopener" style="color:${QUALITY[+r.quality] || "#fff"};text-decoration:none" onclick="event.preventDefault()">${esc(r.name)}</a>`
      : `<span class="r-name">${esc(r.name)}</span>`;
    return `<div class="row-r${on ? " picked" : ""}" data-key="${key}">
      <input type="checkbox"${on ? " checked" : ""} tabindex="-1">
      <div>${name}<div class="r-meta">${meta}</div></div>
      <span class="tag">${r.kind}</span>
      <a class="preview" href="${wowhead(r.kind, r.id)}" target="_blank" rel="noopener">Preview ↗</a></div>`;
  };
  let html = "";
  if (sets.length) html += `<div class="group-h">Sets (${sets.length}) - one entry per colour</div><div class="list">${sets.map(row).join("")}</div>`;
  if (items.length) html += `<div class="group-h">Items (${items.length})</div><div class="list">${items.map(row).join("")}</div>`;
  $("#results").innerHTML = html;
  refreshWowhead();
}

function renderBasket() {
  $("#basket").classList.toggle("hidden", !PICKED.size);
  $("#basket-count").textContent = PICKED.size;
  $("#family-wrap").classList.toggle("hidden", ![...PICKED.values()].some((p) => p.kind === "set"));
}

$("#results").addEventListener("click", (e) => {
  if (e.target.closest(".preview")) return;
  const row = e.target.closest(".row-r"); if (!row) return;
  const key = row.dataset.key, r = RESULTS.find((x) => x.kind + ":" + x.id === key);
  if (PICKED.has(key)) PICKED.delete(key); else PICKED.set(key, {kind: r.kind, id: r.id, name: r.name});
  row.classList.toggle("picked", PICKED.has(key)); row.querySelector("input").checked = PICKED.has(key);
  renderBasket();
});

$("#search-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const q = $("#search-text").value.trim(); if (q.length < 2) return;
  $("#search-note").textContent = "Searching… the first search downloads retail's tables, which can take a minute.";
  try {
    const r = await api("search?q=" + encodeURIComponent(q));
    if (r.need_scan) { $("#search-note").textContent = "Read your client first (button above)."; $("#scan-banner").classList.remove("hidden"); return; }
    if (r.error) { $("#search-note").textContent = "The search failed: " + r.error.split("\n").slice(-3).join(" "); return; }
    RESULTS = r.results;
    $("#search-note").textContent = RESULTS.length ? `${RESULTS.length} found. Tick what you want.` :
      "Nothing matched. Try fewer words, or the name as Wowhead spells it.";
    renderResults();
  } catch (err) { $("#search-note").textContent = err.message; }
});

$("#basket-clear").onclick = () => { PICKED.clear(); renderResults(); renderBasket(); };
$("#basket-add").onclick = async () => {
  const chosen = [...PICKED.values()].map(({kind, id}) => ({kind, id}));
  if (await run("add", {chosen, family: $("#family").checked})) { PICKED.clear(); renderResults(); renderBasket(); }
};

// ------------------------------------------------------------------ project

let PROJECT = [];
const DROP = new Set();

async function loadProject() {
  try { PROJECT = (await api("project")).entries; } catch (e) { PROJECT = []; }
  $("#nav-count").textContent = PROJECT.length || "";
  renderProject();
}

function renderProject() {
  const f = $("#project-filter").value.trim().toLowerCase();
  const list = PROJECT.filter((e) => !f || e.name.toLowerCase().includes(f) ||
    e.pieces.some((p) => p.name.toLowerCase().includes(f)));
  if (!PROJECT.length) { $("#project-list").innerHTML = `<div class="empty">Nothing added yet. Use Find to search retail.</div>`; }
  else $("#project-list").innerHTML = list.map((e) => {
    const on = DROP.has(e.index);
    const prev = e.kind === "set" ? wowhead("set", e.set) : (e.pieces[0]?.retail_item ? wowhead("item", e.pieces[0].retail_item) : "");
    return `<div class="entry${on ? " picked" : ""}" data-i="${e.index}">
      <div class="entry-h"><input type="checkbox"${on ? " checked" : ""} tabindex="-1">
        <span class="entry-name">${esc(e.name)}</span><span class="tag">${e.kind}${e.set ? " " + esc(e.set) : ""}</span>
        <span class="grow"></span>${prev ? `<a class="preview" href="${prev}" target="_blank" rel="noopener">Preview ↗</a>` : ""}</div>
      <div class="pieces">${e.pieces.map((p) => `<div class="piece">
        ${p.sheets[0] ? `<img src="${sheetUrl(p.sheets[0])}" data-full="${sheetUrl(p.sheets[0])}" alt="">` : ""}
        <div><div style="color:${QUALITY[+p.quality] || "#fff"}">${esc(p.name)}</div>
        <div class="slot">${esc(p.slot)}${p.item_id ? " · item " + esc(p.item_id) : ""}</div></div>
        ${p.left_out ? `<span class="tag warn">left out</span>` : ""}${p.brought_back ? `<span class="tag ok">Equip Plus</span>` : ""}
      </div>`).join("")}</div></div>`;
  }).join("");
  $("#remove-selected").disabled = !DROP.size;
  $("#remove-selected").textContent = DROP.size ? `Remove ${DROP.size} selected` : "Remove selected";
}

$("#project-list").addEventListener("click", (e) => {
  const img = e.target.closest("img[data-full]"); if (img) return lightbox(img.dataset.full);
  if (e.target.closest(".preview")) return;
  const h = e.target.closest(".entry-h"); if (!h) return;
  const i = +h.parentElement.dataset.i;
  DROP.has(i) ? DROP.delete(i) : DROP.add(i);
  renderProject();
});
$("#project-filter").addEventListener("input", renderProject);
$("#remove-selected").onclick = async () => {
  if (await run("remove", {indexes: [...DROP]})) DROP.clear();
};

// ------------------------------------------------------------------ build gallery

async function loadGallery(target = $("#gallery")) {
  let sheets = {};
  try { sheets = (await api("sheets")).sheets; } catch (e) {}
  const files = Object.values(sheets).flat();
  target.innerHTML = files.length ? files.map((f) =>
    `<figure data-full="${sheetUrl(f)}"><img loading="lazy" src="${sheetUrl(f)}" alt=""><figcaption>${esc(f.replace(/\.png$/i, "").replace(/_/g, " "))}</figcaption></figure>`).join("")
    : `<div class="muted">No pictures yet - they are drawn at the end of a build.</div>`;
}
document.addEventListener("click", (e) => { const f = e.target.closest("figure[data-full]"); if (f) lightbox(f.dataset.full); });

// ------------------------------------------------------------------ settings

let settingsDirty = false;
function fillSettings(s) {
  if (settingsDirty) return;
  const form = $("#settings-form");
  for (const [k, v] of Object.entries(s.settings)) {
    const el = form.elements[k]; if (!el) continue;
    if (el.type === "checkbox") el.checked = !!v; else el.value = v || "";
  }
  $("#init-row").classList.toggle("hidden", !(s.problems.includes("project") && !s.problems.includes("work")));
}
$("#settings-form").addEventListener("input", () => { settingsDirty = true; });
$("#settings-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const form = e.target, body = {};
  for (const el of form.elements) if (el.name) body[el.name] = el.type === "checkbox" ? el.checked : el.value;
  const r = await api("settings", body);
  settingsDirty = false;
  $$("small[data-for]").forEach((s) => { s.textContent = r.problems[s.dataset.for] || ""; });
  $("#settings-saved").textContent = Object.keys(r.problems).length ? "Saved - fix the marked fields." : "Saved.";
  STATE = r.state; loadState();
});

// ------------------------------------------------------------------ jobs

let JOB = null, SINCE = 0, POLL = null;

async function run(action, body = {}) {
  if (settingsDirty && !["download_kit", "init"].includes(action)) { toast("Save your settings first."); return false; }
  if (["download_kit", "init"].includes(action) && settingsDirty) $("#settings-form").requestSubmit();
  try {
    const r = await api("run/" + action, body);
    attachJob(r.job, true);
    return true;
  } catch (e) { toast(e.message); return false; }
}

document.addEventListener("click", (e) => {
  const b = e.target.closest("[data-run]"); if (b) { e.preventDefault(); run(b.dataset.run); }
  const o = e.target.closest("[data-open]");
  if (o) api("open", {what: o.dataset.open}).then((r) => { if (!r.ok) toast("That folder does not exist yet."); });
});

function attachJob(job, fresh) {
  if (!job) return;
  if (!JOB || JOB.id !== job.id) { JOB = job; SINCE = 0; $("#log").innerHTML = ""; }
  if (fresh || job.state === "running" || job.state === "waiting") $("#job").classList.remove("hidden");
  $("#job-name").textContent = job.name;
  if (!POLL) POLL = setInterval(pollJob, 400);
}

$("#job-close").onclick = () => $("#job").classList.add("hidden");

const STATE_WORDS = {running: "Working…", waiting: "Waiting for you", done: "Finished", failed: "Stopped"};

async function pollJob() {
  let r;
  try { r = await api("job?since=" + SINCE); } catch (e) { return; }
  if (!r.job) return;
  if (r.job.id !== JOB?.id) attachJob(r.job, true);
  const log = $("#log"), stick = log.scrollTop + log.clientHeight >= log.scrollHeight - 30;
  for (const ev of r.events) {
    const d = document.createElement("div");
    if (ev.t === "title") { d.className = "t"; d.textContent = ev.text; }
    else if (ev.t === "line") { d.textContent = ev.text; if (/^\s*!!|FAIL|PROBLEM/.test(ev.text)) d.className = "e"; else if (/^ {4}/.test(ev.text)) d.className = "k"; }
    else if (ev.t === "prompt") { d.className = "q"; d.textContent = "? " + ev.question; }
    else if (ev.t === "answer") { d.className = "a"; d.textContent = "→ " + ev.text; }
    else if (ev.t === "folder") { d.className = "a"; d.textContent = ev.sheets ? "(preview pictures shown below)" : "Folder: " + ev.path; if (ev.sheets) JOB.sheets = true; }
    else continue;
    log.append(d);
  }
  SINCE = r.next;
  if (r.events.length && Date.now() - (pollJob._st || 0) > 1500) { pollJob._st = Date.now(); loadState(); }
  if (stick) log.scrollTop = log.scrollHeight;
  const st = $("#job-state"); st.className = "job-state " + r.job.state; st.textContent = STATE_WORDS[r.job.state] || r.job.state;
  renderPrompt(r.job.prompt);
  if (r.job.state === "done" || r.job.state === "failed") {
    clearInterval(POLL); POLL = null; loadState(); loadProject();
    if (!$("#view-build").classList.contains("hidden")) loadGallery();
  }
}

let PROMPT_KEY = "";
function renderPrompt(p) {
  const box = $("#prompt");
  const key = p ? JSON.stringify(p) + SINCE : "";
  if (!p) { box.classList.add("hidden"); PROMPT_KEY = ""; return; }
  if (key === PROMPT_KEY) return;
  PROMPT_KEY = key;
  box.classList.remove("hidden");
  const pics = JOB?.sheets && p.kind === "yes" && /picture/i.test(p.question);
  let html = `<div class="q">${esc(p.question)}</div>`;
  if (pics) html += `<div class="gallery" id="prompt-gallery"></div>`;
  if (p.kind === "yes") {
    html += `<div class="row"><button class="btn ${p.default ? "primary" : ""}" data-a="yes">Yes</button>
      <button class="btn ${p.default ? "" : "primary"}" data-a="no">No</button></div>`;
  } else {
    html += `<form class="row" id="prompt-form"><input ${p.kind === "password" ? 'type="password"' : ""} id="prompt-in"
      value="${esc(p.kind === "password" ? "" : p.default)}" autocomplete="off"><button class="btn primary">OK</button></form>`;
  }
  box.innerHTML = html;
  if (pics) loadGallery($("#prompt-gallery"));
  const send = async (v) => { box.classList.add("hidden"); await api("answer", {value: v}); };
  $$("[data-a]", box).forEach((b) => b.onclick = () => send(b.dataset.a === "yes"));
  const f = $("#prompt-form", box);
  if (f) { f.onsubmit = (e) => { e.preventDefault(); send($("#prompt-in").value); }; $("#prompt-in").focus(); }
  $("#job").classList.remove("hidden");
}

// ------------------------------------------------------------------ start

$$(".nav").forEach((b) => b.onclick = () => show(b.dataset.view));
(async () => {
  if (!TOKEN) { document.body.innerHTML = "<p style='padding:30px'>Open the app from the Backport App window (the link it prints).</p>"; return; }
  await loadState();
  let v = "find"; try { v = localStorage.getItem("bp-view") || "find"; } catch (e) {}
  if (STATE && STATE.problems.length) v = "settings";
  show(v);
  loadProject();
  const r = await api("job").catch(() => ({}));
  if (r.job) attachJob(r.job, r.job.state === "running" || r.job.state === "waiting");
  setInterval(() => { if (!POLL) loadState(); }, 15000);
})();
