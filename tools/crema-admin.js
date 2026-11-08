#!/usr/bin/env node
// crema-admin - the organizer's tool for the proof of concept (the Basecamp companion comes next).
//
//   crema-admin init  --event brno-coffee-week-2026 --name "Brno Coffee Week" --city Brno --start 2026-11-16 --days 7
//   crema-admin cafe  --id industra --name "Industra Coffee" --lat 49.18 --lon 16.60 [--address "..."] [--radius 75]
//   crema-admin publish [--base https://host/crema/]   signs web/catalog.json, prints each café's display setup link
//   crema-admin board passport1.json passport2.json ...  the leaderboard from players' exported passports
//
// Secrets (organizer key, café keys) live in admin/ - never commit or publish that folder.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import * as C from "../protocol/src/index.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const ADMIN = path.join(ROOT, "admin"), WEB = path.join(ROOT, "web");
const f = (n) => path.join(ADMIN, n);
const load = (n, d) => (fs.existsSync(f(n)) ? JSON.parse(fs.readFileSync(f(n), "utf8")) : d);
const save = (n, v) => { fs.mkdirSync(ADMIN, { recursive: true, mode: 0o700 }); fs.writeFileSync(f(n), JSON.stringify(v, null, 2) + "\n", { mode: 0o600 }); };

function args(list) {
  const o = { _: [] };
  for (let i = 0; i < list.length; i++) {
    if (list[i].startsWith("--")) o[list[i].slice(2)] = list[i + 1] && !list[i + 1].startsWith("--") ? list[++i] : true;
    else o._.push(list[i]);
  }
  return o;
}

const [cmd, ...rest] = process.argv.slice(2);
const a = args(rest);

if (cmd === "init") {
  if (!a.event || !a.name || !a.start) { console.error("init needs --event --name --start [--days 7] [--city]"); process.exit(2); }
  const org = load("organizer.json", null) || C.newOrganizerKey();
  save("organizer.json", org);
  const start = Date.parse(a.start + "T00:00:00+01:00");
  const days = Number(a.days || 7);
  save("event.json", { v: 1, eventId: a.event, name: a.name, city: a.city || "", start, end: start + days * 86400e3 - 1, cafes: load("event.json", { cafes: [] }).cafes });
  console.log(`event ${a.event}: ${new Date(start).toISOString()} + ${days} days; organizer ${org.publicKey}`);
} else if (cmd === "cafe") {
  const ev = load("event.json", null);
  if (!ev) { console.error("run init first"); process.exit(2); }
  if (!a.id || !a.name || !a.lat || !a.lon) { console.error("cafe needs --id --name --lat --lon"); process.exit(2); }
  const keys = load("cafe-keys.json", {});
  if (!keys[a.id]) keys[a.id] = C.newCafeKey();
  save("cafe-keys.json", keys);
  ev.cafes = ev.cafes.filter((c) => c.id !== a.id);
  ev.cafes.push({ id: a.id, name: a.name, lat: Number(a.lat), lon: Number(a.lon), radiusM: Number(a.radius || C.DEFAULT_RADIUS_M), pubkey: keys[a.id].publicKey, ...(a.address ? { address: a.address } : {}) });
  save("event.json", ev);
  console.log(`café ${a.id} (${a.name}) - ${ev.cafes.length} in the event`);
} else if (cmd === "publish") {
  const ev = load("event.json", null), org = load("organizer.json", null), keys = load("cafe-keys.json", {});
  if (!ev || !org) { console.error("run init first"); process.exit(2); }
  const signed = C.signCatalog(ev, org.secretKey);
  if (!C.verifyCatalog(signed, org.publicKey)) throw new Error("catalog didn't verify");
  fs.writeFileSync(path.join(WEB, "catalog.json"), JSON.stringify(signed, null, 2) + "\n");
  // pin the organizer in the player page
  const idx = path.join(WEB, "index.html");
  fs.writeFileSync(idx, fs.readFileSync(idx, "utf8").replace(/window\.CREMA_ORGANIZER = "[^"]*";/, `window.CREMA_ORGANIZER = "${org.publicKey}";`));
  console.log(`web/catalog.json signed: ${signed.cafes.length} cafés, organizer ${org.publicKey}`);
  const base = (a.base || "http://localhost:8099/").replace(/\/?$/, "/");
  console.log("\nCafé display setup links (each contains that café's SECRET key - hand over privately, open once on the café's device):");
  for (const c of signed.cafes) {
    const q = new URLSearchParams({ c: c.id, k: keys[c.id].secretKey, n: c.name, e: ev.name });
    console.log(`  ${c.name}: ${base}display.html#${q}`);
  }
} else if (cmd === "board") {
  const catalog = JSON.parse(fs.readFileSync(path.join(WEB, "catalog.json"), "utf8"));
  const names = {}, all = [];
  for (const file of a._) {
    const p = JSON.parse(fs.readFileSync(file, "utf8"));
    if (p.kind !== "crema.passport" || p.eventId !== catalog.eventId) { console.error(`${file}: not a passport for ${catalog.eventId}`); continue; }
    names[p.player] = p.name || p.player.slice(0, 10);
    all.push(...p.claims);
  }
  const lb = C.leaderboard(all, catalog);
  console.log(`${catalog.name} - ${catalog.cafes.length} cafés\n`);
  lb.forEach((r, i) => console.log(`${String(i + 1).padStart(2)}. ${names[r.player] || r.player.slice(0, 10)}  ${r.cafes} cafés  (${r.stamps} stamps)`));
} else {
  console.log(fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n").slice(1, 9).join("\n").replace(/^\/\/ ?/gm, ""));
}
