// Crema engine: one coffee event as a loam-sync event log.
//
// - The event secret (from the organizer's join QR) is a loam-sync group secret: it gives the
//   Delivery topic and seals every frame (ChaCha20-Poly1305, deterministic nonce), so only
//   participants can read the event.
// - The log holds three event types: "catalog" (the organizer-signed café list, highest version
//   wins), "stamp" (a player's claim of a café's rotating QR, signed with the player's secp256k1
//   identity via loam-sync - Keycard custody works the same) and "profile" (the leaderboard name,
//   last write wins).
// - The leaderboard is a pure fold over the log: every device computes the same ranking; nobody is
//   trusted to rank.
// - A player who doesn't join the leaderboard keeps stamps OUT of the shared log (a private list),
//   so catch-up never serves them to anyone.
// - Catch-up is loam-sync's RBSR: a joining device learns exactly what it lacks from any peer.
//
// Pure logic: no I/O. The app feeds frames from the transport in and publishes what comes out.
import { mergeOne } from "../../loam-sync/dist/merge.js";
import { Clock, compareHlc } from "../../loam-sync/dist/event.js";
import { SoftwareSigner, signEvent, verifyEvent, address, fromHex, hex } from "../../loam-sync/dist/signing.js";
import { deriveIdentity, topicFor, seal, open } from "../../loam-sync/dist/crypto.js";
import { buildInitial, respond } from "../../loam-sync/dist/catchup.js";
import * as P from "../../protocol/src/index.js";

const TD = new TextEncoder(), DE = new TextDecoder();
export const MAX_FRAME = 16 * 1024;          // a frame larger than this is refused unread
export const CLAIM_LATE_MS = 2 * P.STEP_S * 1000;  // a stamp claimed later than this after its QR was passed around
const NAME_MAX = 40;

export const stampId = (author, cafeId, step) => `st-${author}-${cafeId}-${step}`;
export const catalogId = (version) => `cat-${String(version).padStart(8, "0")}`;

// ── signature checks, remembered ──
// On a phone (Hermes, pure-JS crypto) one verification costs tens of ms, and the fold runs them all;
// re-verifying on every render froze the UI for seconds. A signature's validity never changes, so
// remember it - keyed by the FULL content (never just the id: same id + altered payload must re-check).
const VERIFIED = new Map();
function remember(key, fn) {
  const hit = VERIFIED.get(key);
  if (hit !== undefined) return hit;
  const v = !!fn();
  if (VERIFIED.size > 50000) VERIFIED.clear();
  VERIFIED.set(key, v);
  return v;
}
function trustRestored(e, join) {
  if (!e || typeof e !== "object") return;
  if (e.type === "catalog" && e.payload?.catalog) VERIFIED.set("cat\u0000" + join.organizer + "\u0000" + JSON.stringify(e.payload.catalog), true);
  else if (e.type === "stamp" || e.type === "profile") VERIFIED.set("ev\u0000" + JSON.stringify(e), true);
}
const eventSigOk = (e) => remember("ev\u0000" + JSON.stringify(e), () => verifyEvent(P.DOMAIN, e));

// ── validity (the fold's admission rules; every device applies the same) ──
export function validCatalogEvent(e, join) {
  if (!e || e.type !== "catalog" || !e.payload || typeof e.payload !== "object") return null;
  const c = e.payload.catalog;
  if (!c || c.eventId !== join.eventId || !Number.isInteger(c.version) || e.id !== catalogId(c.version)) return null;
  return remember("cat\u0000" + join.organizer + "\u0000" + JSON.stringify(c), () => P.verifyCatalog(c, join.organizer)) ? c : null;
}
export function validStamp(e, catalog) {
  if (!e || e.type !== "stamp" || !catalog || !eventSigOk(e)) return false;
  const p = e.payload || {};
  if (typeof p.cafeId !== "string" || !Number.isInteger(p.step) || typeof p.cafeSig !== "string") return false;
  if (e.id !== stampId(e.dev, p.cafeId, p.step)) return false;
  const cafe = catalog.cafes.find((c) => c.id === p.cafeId);
  if (!cafe || !remember("cafe\u0000" + p.cafeId + "\u0000" + p.step + "\u0000" + p.cafeSig + "\u0000" + cafe.pubkey,
    () => P.verifyStampSig(p.cafeId, p.step, p.cafeSig, cafe.pubkey))) return false;
  const t = P.stepTimeMs(p.step);
  if (t < catalog.start || t > catalog.end) return false;
  return Math.abs(e.hlc.wall - t) <= CLAIM_LATE_MS;
}
function validProfile(e) {
  return e && e.type === "profile" && typeof e.payload?.name === "string" && e.payload.name.length <= NAME_MAX && eventSigOk(e);
}

// ── the fold ──
export function fold(log, join, me) {
  let catalog = null;
  for (const e of log) {
    const c = validCatalogEvent(e, join);
    if (c && (!catalog || c.version > catalog.version)) catalog = c;
  }
  const names = new Map();
  const players = new Map();
  const sorted = [...log].sort((a, b) => compareHlc(a.hlc, b.hlc));
  for (const e of sorted) {
    if (validProfile(e)) { names.set(e.dev, e.payload.name.trim()); continue; }
    if (!validStamp(e, catalog)) continue;
    let p = players.get(e.dev);
    if (!p) players.set(e.dev, (p = { author: e.dev, cafes: new Set(), days: new Set(), stamps: 0, reachedAt: 0 }));
    const step = e.payload.step;
    const day = e.payload.cafeId + "@" + Math.floor((step * P.STEP_S) / 86400);
    if (p.days.has(day)) continue;               // one stamp per café per day
    p.days.add(day);
    p.stamps++;
    if (!p.cafes.has(e.payload.cafeId)) { p.cafes.add(e.payload.cafeId); p.reachedAt = P.stepTimeMs(step); }
  }
  const leaderboard = [...players.values()]
    .map((p) => ({ author: p.author, name: names.get(p.author) || "", cafes: p.cafes.size, stamps: p.stamps, visited: [...p.cafes], reachedAt: p.reachedAt }))
    .sort((a, b) => b.cafes - a.cafes || a.reachedAt - b.reachedAt || (a.author < b.author ? -1 : 1));
  const mine = players.get(me);
  return { catalog, leaderboard, names, myVisited: mine ? new Set(mine.cafes) : new Set() };
}

// ── one event, as seen from one device ──
export class CremaGame {
  // join: parseJoinLink() result; playerSecretHex: the player's secp256k1 key (Loam identity);
  // state: what toJSON() returned last time (optional); now: clock (tests)
  constructor({ join, playerSecretHex, state = null, now = () => Date.now() }) {
    this.join = join;
    this.now = now;
    this.signer = new SoftwareSigner(fromHex(playerSecretHex));
    this.author = address(this.signer.publicKey());
    this.group = deriveIdentity(fromHex(join.secretHex), P.DOMAIN);
    this.topic = topicFor(this.group, P.DOMAIN);
    this.log = [];
    this.rev = 0;                                // bumps on every change the fold could see
    this.memo = null;
    this.privateStamps = [];                     // stamps of a player not on the leaderboard
    this.share = true;                           // on the leaderboard?
    this.clock = new Clock(this.author, now);
    if (state) {
      // our own saved log: every event in it passed admit() before it was saved, so don't re-run the
      // crypto at start (seconds on a phone) - only mark it known-good. Network events are always checked.
      for (const e of state.log || []) { trustRestored(e, join); this.admit(e); }
      this.privateStamps = state.privateStamps || [];
      this.share = state.share !== false;
    }
    for (const e of this.log) this.clock.receive(e.hlc);
  }
  toJSON() { return { v: 1, log: this.log, privateStamps: this.privateStamps, share: this.share }; }

  // Admission: only events the fold could ever use (or a valid catalog) enter the log
  admit(e) {
    if (!e || typeof e.id !== "string" || !e.hlc) return false;
    if (e.type === "catalog") { if (!validCatalogEvent(e, this.join)) return false; }
    else if (e.type === "stamp" || e.type === "profile") { if (!eventSigOk(e)) return false; }
    else return false;
    const added = mergeOne(this.log, e);
    if (added) { this.clock.receive(e.hlc); this.rev++; }
    return added;
  }

  // The folded state, cached until the log / private stamps / sharing change (rev). Treat as read-only.
  state() {
    if (this.memo && this.memo.rev === this.rev) return this.memo.s;
    const s = this.foldNow();
    this.memo = { rev: this.rev, s };
    return s;
  }
  foldNow() {
    const s = fold(this.log, this.join, this.author);
    if (!this.share && s.catalog) {
      // my private stamps count for me, never for the shared leaderboard
      const mine = fold([...this.log.filter((e) => e.type !== "stamp" || e.dev !== this.author), ...this.privateStamps], this.join, this.author);
      s.myVisited = mine.myVisited;
    }
    return { ...s, author: this.author, share: this.share, eventName: s.catalog?.name || this.join.name };
  }

  // ── frames ──
  frame(obj, id) {
    const plain = TD.encode(JSON.stringify(obj));
    return seal(this.group, P.DOMAIN, id, plain, this.topic);
  }
  eventFrame(e) { return this.frame({ t: "ev", e }, e.id); }
  catchupFrame(m) { return this.frame({ t: "cu", m }, "cu-" + hex(P.randomBytes(12))); }
  // What a device publishes when it joins or comes back online
  hello() { return this.catchupFrame(buildInitial(this.log, this.author)); }

  // A frame from the topic. Returns { changed, out: [frames to publish] }
  receive(bytes) {
    const res = { changed: false, out: [] };
    if (!bytes || bytes.length > MAX_FRAME) return res;
    let msg;
    try { msg = JSON.parse(DE.decode(open(this.group, bytes, this.topic))); } catch { return res; }
    if (msg?.t === "ev") res.changed = this.admit(msg.e);
    else if (msg?.t === "cu" && msg.m && msg.m.from !== this.author) {
      const step = respond(this.log, msg.m, this.author);
      for (const r of step.replies) res.out.push(this.catchupFrame(r));
      for (const e of step.serve) res.out.push(this.eventFrame(e));
    }
    return res;
  }

  // ── what a player does ──
  // Scan a café's code at the café. gps: { lat, lon, accuracy } from the phone (never published).
  // Returns { ok, reason?, cafe?, already?, out: [frames] } - the stamp is in the log before any send.
  stamp(qrText, gps) {
    const catalog = this.state().catalog;
    if (!catalog) return { ok: false, reason: "The café list hasn't arrived yet - give it a moment online", out: [] };
    const now = this.now();
    const checked = P.checkStamp(qrText, catalog, now);
    if (!checked.ok) return { ok: false, reason: checked.reason, out: [] };
    if (!gps) return { ok: false, reason: "Location is needed to stamp", cafe: checked.cafe, out: [] };
    const near = P.nearCafe(checked.cafe, gps.lat, gps.lon, gps.accuracy || 0);
    if (!near.ok) return { ok: false, reason: `You seem to be ${near.distanceM} m from ${checked.cafe.name} - stamps count only at the café`, cafe: checked.cafe, out: [] };
    const already = this.state().myVisited.has(checked.cafe.id);
    const e = signEvent(this.signer, P.DOMAIN, {
      v: 1, id: stampId(this.author, checked.cafe.id, checked.step), type: "stamp",
      hlc: this.clock.send(now), dev: this.author,
      payload: { cafeId: checked.cafe.id, step: checked.step, cafeSig: checked.sig },
    });
    if (!this.share) {
      if (!this.privateStamps.some((x) => x.id === e.id)) { this.privateStamps.push(e); this.rev++; }
      return { ok: true, cafe: checked.cafe, already, out: [] };
    }
    this.admit(e);
    return { ok: true, cafe: checked.cafe, already, out: [this.eventFrame(e)] };
  }

  setName(name) {
    const n = String(name || "").trim().slice(0, NAME_MAX);
    const e = signEvent(this.signer, P.DOMAIN, { v: 1, id: `pr-${this.author}-${this.now()}`, type: "profile", hlc: this.clock.send(), dev: this.author, payload: { name: n } });
    this.admit(e);
    return { ok: true, out: [this.eventFrame(e)] };
  }

  // Join or leave the leaderboard. Joining publishes the private stamps; leaving can't take back
  // what others already have, so it only stops new stamps from being shared.
  setShare(on) {
    const out = [];
    if (on && !this.share) {
      for (const e of this.privateStamps) { if (this.admit(e)) out.push(this.eventFrame(e)); }
      this.privateStamps = [];
    }
    this.share = !!on;
    this.rev++;
    return { ok: true, out };
  }

  // ── what the organizer does ──
  publishCatalog(signedCatalog) {
    const e = { v: 1, id: catalogId(signedCatalog.version), type: "catalog", hlc: this.clock.send(), dev: this.author, payload: { catalog: signedCatalog } };
    if (!this.admit(e)) return { ok: false, reason: validCatalogEvent(e, this.join) ? "this catalog version is already published" : "the catalog isn't signed by this event's organizer", out: [] };
    return { ok: true, out: [this.eventFrame(e)] };
  }
}
