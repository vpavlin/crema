// Crema player (web proof of concept; the Android + Basecamp apps come next).
// Map of the event's cafés, scan a café's rotating QR, check you're there, keep the signed stamp
// in your passport, and export it for the leaderboard.
import L from "leaflet";
import jsQR from "jsqr";
import * as C from "../../protocol/src/index.js";

const ORGANIZER = window.CREMA_ORGANIZER || "";     // pinned in player.html: the only catalog signer we trust
const KEY_ID = "crema.player.v1", KEY_CLAIMS = "crema.claims.v1";
const $ = (id) => document.getElementById(id);

// ── identity ──
function identity() {
  let id = null;
  try { id = JSON.parse(localStorage.getItem(KEY_ID) || "null"); } catch {}
  if (!id) { id = C.newPlayerKey(); id.name = ""; localStorage.setItem(KEY_ID, JSON.stringify(id)); }
  return id;
}
const me = identity();
const claims = () => { try { return JSON.parse(localStorage.getItem(KEY_CLAIMS) || "[]"); } catch { return []; } };
const saveClaim = (c) => localStorage.setItem(KEY_CLAIMS, JSON.stringify([...claims(), c]));

let catalog = null, map = null, markers = {};

function visitedIds() {
  const lb = C.leaderboard(claims().filter((c) => c.player === me.publicKey), catalog);
  return new Set(lb.length ? lb[0].visited : []);
}

function toast(msg, good) {
  const t = $("toast");
  t.textContent = msg; t.className = "show " + (good ? "good" : "bad");
  clearTimeout(t._h); t._h = setTimeout(() => (t.className = ""), 4500);
}

function renderPassport() {
  const v = visitedIds();
  $("count").textContent = `${v.size} / ${catalog.cafes.length}`;
  $("list").innerHTML = "";
  for (const cafe of catalog.cafes) {
    const li = document.createElement("li");
    li.className = v.has(cafe.id) ? "done" : "";
    li.innerHTML = `<span class="stamp">${v.has(cafe.id) ? "☕" : "○"}</span><span><b></b><br><small></small></span>`;
    li.querySelector("b").textContent = cafe.name;
    li.querySelector("small").textContent = cafe.address || "";
    li.onclick = () => { show("map"); map.setView([cafe.lat, cafe.lon], 17); markers[cafe.id]?.openPopup(); };
    $("list").appendChild(li);
  }
  for (const cafe of catalog.cafes) {
    const m = markers[cafe.id];
    if (m) m.setStyle({ fillColor: v.has(cafe.id) ? "#d9a066" : "#6b4f3f" });
  }
}

function initMap() {
  map = L.map("mapview", { zoomControl: true });
  L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", { maxZoom: 19, attribution: "© OpenStreetMap contributors" }).addTo(map);
  const pts = [];
  for (const cafe of catalog.cafes) {
    const m = L.circleMarker([cafe.lat, cafe.lon], { radius: 9, color: "#2a1d16", weight: 2, fillColor: "#6b4f3f", fillOpacity: 0.95 }).addTo(map);
    m.bindPopup(`<b>${cafe.name.replace(/</g, "&lt;")}</b><br>${(cafe.address || "").replace(/</g, "&lt;")}`);
    markers[cafe.id] = m; pts.push([cafe.lat, cafe.lon]);
  }
  if (pts.length) map.fitBounds(pts, { padding: [30, 30] });
}

// ── scanning ──
let stream = null, scanning = false;
async function startScan() {
  show("scan");
  $("scanmsg").textContent = "Point the camera at the café's Crema code";
  try {
    stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: "environment" }, audio: false });
  } catch (e) { $("scanmsg").textContent = "No camera access: " + e.message; return; }
  const video = $("video");
  video.srcObject = stream; await video.play();
  const canvas = document.createElement("canvas"), ctx = canvas.getContext("2d", { willReadFrequently: true });
  scanning = true;
  const tick = () => {
    if (!scanning) return;
    if (video.readyState === video.HAVE_ENOUGH_DATA) {
      canvas.width = video.videoWidth; canvas.height = video.videoHeight;
      ctx.drawImage(video, 0, 0);
      const code = jsQR(ctx.getImageData(0, 0, canvas.width, canvas.height).data, canvas.width, canvas.height, { inversionAttempts: "dontInvert" });
      if (code && code.data.startsWith(C.QR_PREFIX + ".")) { stopScan(); onCode(code.data); return; }
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}
function stopScan() {
  scanning = false;
  if (stream) { stream.getTracks().forEach((t) => t.stop()); stream = null; }
}

function position() {
  return new Promise((ok, fail) => navigator.geolocation.getCurrentPosition((p) => ok(p.coords), fail, { enableHighAccuracy: true, timeout: 15000, maximumAge: 10000 }));
}

async function onCode(text) {
  const checked = C.checkStamp(text, catalog, Date.now());
  if (!checked.ok) { toast(checked.reason, false); show("passport"); return; }
  $("scanmsg").textContent = `Checking you're at ${checked.cafe.name}…`;
  let near;
  try {
    const c = await position();
    near = C.nearCafe(checked.cafe, c.latitude, c.longitude, c.accuracy);
  } catch (e) { toast("Location is needed to stamp (" + e.message + ")", false); show("passport"); return; }
  if (!near.ok) { toast(`You seem to be ${near.distanceM} m from ${checked.cafe.name}. Stamps count only at the café.`, false); show("passport"); return; }
  const already = visitedIds().has(checked.cafe.id);
  saveClaim(C.makeClaim(checked, catalog.eventId, me.secretKey, Date.now()));
  renderPassport();
  show("passport");
  toast(already ? `${checked.cafe.name}: stamped again today counts once - enjoy the coffee ☕` : `Stamped: ${checked.cafe.name} ☕`, true);
}

// ── export for the leaderboard ──
function exportPassport() {
  const blob = { v: 1, kind: "crema.passport", eventId: catalog.eventId, name: me.name || "", player: me.publicKey, claims: claims().filter((c) => c.eventId === catalog.eventId) };
  const text = JSON.stringify(blob);
  navigator.clipboard?.writeText(text).then(() => toast("Passport copied - send it to the organizer", true), () => {});
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  a.download = `crema-passport-${me.publicKey.slice(0, 8)}.json`;
  a.click();
}

function show(tab) {
  for (const t of ["map", "passport", "scan"]) {
    $(t).hidden = t !== tab;
    document.querySelector(`[data-tab=${t}]`)?.classList.toggle("on", t === tab);
  }
  if (tab === "map") setTimeout(() => map.invalidateSize(), 50);
  if (tab !== "scan") stopScan();
}

async function boot() {
  try {
    const r = await fetch("catalog.json", { cache: "no-cache" });
    catalog = await r.json();
  } catch (e) { $("event").textContent = "Couldn't load the event: " + e.message; return; }
  if (!C.verifyCatalog(catalog, ORGANIZER)) { $("event").textContent = "The event list isn't signed by the organizer - not loading it."; catalog = null; return; }
  $("event").textContent = catalog.name;
  $("me").textContent = "you: " + me.publicKey.slice(0, 10) + "…";
  $("nick").value = me.name || "";
  $("nick").onchange = () => { me.name = $("nick").value.trim().slice(0, 40); localStorage.setItem(KEY_ID, JSON.stringify(me)); };
  initMap();
  renderPassport();
  for (const b of document.querySelectorAll("[data-tab]")) b.onclick = () => (b.dataset.tab === "scan" ? startScan() : show(b.dataset.tab));
  $("export").onclick = exportPassport;
  show("passport");
}
boot();
