// Crema protocol: the parts that are Crema's own - the café's rotating stamp, the organizer's signed
// catalog, the geofence and the join link. The event log, player signatures, sealing and catch-up
// come from loam-sync (packages/loam-sync); the engine (packages/engine) puts them together.
//
// - A café's display shows a QR that changes every STEP_S seconds: an Ed25519 signature by the
//   café's key over (café id, time step). Only the display can make it; anyone can check it with the
//   café's public key from the organizer-signed catalog. No server needed to verify.
// - The GPS check runs on the player's phone only; the position is never published.
// crema_core (C++) implements the same bytes; test/vectors.json pins them.
import { ed25519 } from "@noble/curves/ed25519.js";
import { bytesToHex, hexToBytes, utf8ToBytes, randomBytes } from "@noble/hashes/utils.js";

export const DOMAIN = "crema";
export const STEP_S = 30;            // how long one QR is valid
export const SKEW_STEPS = 1;         // accept the previous/next step too (clock drift, slow scans)
export const QR_PREFIX = "CREMA1";
export const DEFAULT_RADIUS_M = 75;

// ── encoding ──
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
export function b64url(bytes) {
  let s = "", i = 0;
  for (; i + 2 < bytes.length; i += 3) {
    const n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    s += B64[n >> 18] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
  }
  if (i < bytes.length) {
    const n = (bytes[i] << 16) | ((bytes[i + 1] ?? 0) << 8);
    s += B64[n >> 18] + B64[(n >> 12) & 63] + (i + 1 < bytes.length ? B64[(n >> 6) & 63] : "");
  }
  return s;
}
export function unb64url(s) {
  const out = [];
  let buf = 0, bits = 0;
  for (const c of s) {
    const v = B64.indexOf(c);
    if (v < 0) throw new Error("bad base64url");
    buf = (buf << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; out.push((buf >> bits) & 255); }
  }
  return new Uint8Array(out);
}
// Deterministic JSON: sorted keys, undefined dropped. Signed data uses only strings, integers,
// booleans, arrays and objects (no floats - coordinates are microdegrees), so C++ matches it.
export function canonical(v) {
  if (v === null || typeof v !== "object") return JSON.stringify(v);
  if (Array.isArray(v)) return "[" + v.map(canonical).join(",") + "]";
  return "{" + Object.keys(v).filter((k) => v[k] !== undefined).sort().map((k) => JSON.stringify(k) + ":" + canonical(v[k])).join(",") + "}";
}

// ── keys (cafés and organizers: Ed25519) ──
export function newKey() {
  const sk = ed25519.utils.randomSecretKey();
  return { secretKey: bytesToHex(sk), publicKey: bytesToHex(ed25519.getPublicKey(sk)) };
}
export const publicKeyOf = (secretHex) => bytesToHex(ed25519.getPublicKey(hexToBytes(secretHex)));

// ── the café's rotating stamp ──
export const stepOf = (nowMs) => Math.floor(nowMs / 1000 / STEP_S);
export const stepTimeMs = (step) => step * STEP_S * 1000;
const stampMessage = (cafeId, step) => utf8ToBytes(`crema/stamp/v1|${cafeId}|${step}`);
export const CAFE_ID = /^[a-z0-9-]{1,40}$/;

// What the display shows: CREMA1.<cafeId>.<step base36>.<sig b64url>
export function makeStamp(cafeId, cafeSecretHex, nowMs = Date.now()) {
  if (!CAFE_ID.test(cafeId)) throw new Error("café id: a-z, 0-9 and '-', up to 40");
  const step = stepOf(nowMs);
  const sig = ed25519.sign(stampMessage(cafeId, step), hexToBytes(cafeSecretHex));
  return `${QR_PREFIX}.${cafeId}.${step.toString(36)}.${b64url(sig)}`;
}
export function parseStamp(text) {
  const p = String(text).trim().split(".");
  if (p.length !== 4 || p[0] !== QR_PREFIX) return null;
  if (!CAFE_ID.test(p[1]) || !/^[0-9a-z]{1,12}$/.test(p[2])) return null;
  let sig;
  try { sig = unb64url(p[3]); } catch { return null; }
  if (sig.length !== 64) return null;
  return { cafeId: p[1], step: parseInt(p[2], 36), sig: bytesToHex(sig) };
}
export function verifyStampSig(cafeId, step, sigHex, cafePubHex) {
  try { return ed25519.verify(hexToBytes(sigHex), stampMessage(cafeId, step), hexToBytes(cafePubHex)); }
  catch { return false; }
}
// Check a scanned QR against the catalog, now. { ok, reason?, cafe, step, sig }
export function checkStamp(text, catalog, nowMs = Date.now()) {
  const s = parseStamp(text);
  if (!s) return { ok: false, reason: "not a Crema code" };
  const cafe = catalog.cafes.find((c) => c.id === s.cafeId);
  if (!cafe) return { ok: false, reason: "this café isn't part of the event" };
  if (!verifyStampSig(s.cafeId, s.step, s.sig, cafe.pubkey)) return { ok: false, reason: "the code's signature is wrong", cafe };
  const now = stepOf(nowMs);
  if (Math.abs(now - s.step) > SKEW_STEPS) return { ok: false, reason: s.step < now ? "the code has expired - scan it fresh" : "the code is from the future (check your clock)", cafe };
  const t = stepTimeMs(s.step);
  if (t < catalog.start || t > catalog.end) return { ok: false, reason: "outside the event's dates", cafe };
  return { ok: true, cafe, step: s.step, sig: s.sig };
}

// ── where you are (checked on the phone, never published) ──
export function distanceM(lat1, lon1, lat2, lon2) {
  const R = 6371000, r = Math.PI / 180;
  const dLat = (lat2 - lat1) * r, dLon = (lon2 - lon1) * r;
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(a));
}
export const cafeLat = (cafe) => cafe.latE6 / 1e6;
export const cafeLon = (cafe) => cafe.lonE6 / 1e6;
// accuracyM: the phone's reported accuracy; a fix that might be inside the radius counts
export function nearCafe(cafe, lat, lon, accuracyM = 0) {
  const d = distanceM(cafeLat(cafe), cafeLon(cafe), lat, lon);
  return { ok: d - Math.min(accuracyM, 100) <= (cafe.radiusM || DEFAULT_RADIUS_M), distanceM: Math.round(d) };
}

// ── the organizer's signed catalog ──
// { v:1, eventId, name, city, start(ms), end(ms), version, organizer(ed25519 pub hex),
//   cafes:[{id,name,latE6,lonE6,radiusM,pubkey,address?}] }
const catalogMessage = (body) => utf8ToBytes("crema/catalog/v1|" + canonical(body));
export function signCatalog(catalog, organizerSecretHex) {
  const body = { ...catalog, organizer: publicKeyOf(organizerSecretHex) };
  delete body.sig;
  return { ...body, sig: bytesToHex(ed25519.sign(catalogMessage(body), hexToBytes(organizerSecretHex))) };
}
export function verifyCatalog(signed, expectedOrganizerHex) {
  if (!signed || !signed.sig || !signed.organizer || !Array.isArray(signed.cafes)) return false;
  if (expectedOrganizerHex && signed.organizer !== expectedOrganizerHex) return false;
  const body = { ...signed }; delete body.sig;
  try { return ed25519.verify(hexToBytes(signed.sig), catalogMessage(body), hexToBytes(signed.organizer)); }
  catch { return false; }
}

// The event's own map (ADR 0002): vector tiles in ONE .pmtiles file on Logos Storage, named in the signed
// catalog. Integers only (microdegrees), so JS and C++ sign the same canonical bytes. providers: Storage
// nodes known to hold it ({peerId, addrs, spr?}), dialled first. Returns the map or null when absent/invalid.
export function mapOf(catalog) {
  const m = catalog && catalog.map;
  if (!m || typeof m !== "object") return null;
  if (typeof m.cid !== "string" || !/^[A-Za-z0-9]{20,120}$/.test(m.cid)) return null;
  if (typeof m.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(m.sha256)) return null;
  if (!Number.isInteger(m.size) || m.size <= 0 || m.size > 64 * 1024 * 1024) return null;
  if (!Array.isArray(m.bboxE6) || m.bboxE6.length !== 4 || !m.bboxE6.every(Number.isInteger)) return null;
  const providers = Array.isArray(m.providers) ? m.providers.filter((p) => p && typeof p.peerId === "string" && Array.isArray(p.addrs)).slice(0, 8) : [];
  const out = { cid: m.cid, sha256: m.sha256, size: m.size, bboxE6: m.bboxE6, minzoom: Number.isInteger(m.minzoom) ? m.minzoom : 0,
    maxzoom: Number.isInteger(m.maxzoom) ? m.maxzoom : 16, providers, pieces: null };
  // 0.3: the same file as one-block pieces (they cross circuit relays) + where each zoom is complete
  const okCid = (c) => typeof c === "string" && /^[A-Za-z0-9]{20,120}$/.test(c);
  if (Array.isArray(m.pieces) && m.pieces.length > 0 && m.pieces.length <= 200 && m.pieces.every(okCid)
      && Number.isInteger(m.pieceSize) && m.pieceSize > 0 && m.pieceSize * m.pieces.length >= m.size && m.pieceSize * (m.pieces.length - 1) < m.size
      && Number.isInteger(m.dirEnd) && m.dirEnd > 0 && m.dirEnd <= m.size
      && Array.isArray(m.zoomEnds) && m.zoomEnds.every((z) => Array.isArray(z) && z.length === 2 && z.every(Number.isInteger) && z[1] <= m.size)) {
    out.pieces = { cids: m.pieces, size: m.pieceSize, dirEnd: m.dirEnd, zoomEnds: m.zoomEnds };
  }
  return out;
}

// Query strings without URLSearchParams (React Native's lacks get())
function encQuery(obj) { return Object.entries(obj).map(([k, v]) => encodeURIComponent(k) + "=" + encodeURIComponent(v)).join("&"); }
function decQuery(q) {
  const o = {};
  for (const part of q.split("&")) {
    if (!part) continue;
    const i = part.indexOf("=");
    const k = decodeURIComponent((i < 0 ? part : part.slice(0, i)).replace(/\+/g, " "));
    o[k] = i < 0 ? "" : decodeURIComponent(part.slice(i + 1).replace(/\+/g, " "));
  }
  return { get: (k) => (k in o ? o[k] : null) };
}

// ── joining an event: what the organizer's join QR carries ──
// crema:join?e=<eventId>&o=<organizer pub hex>&k=<event secret, 32 bytes b64url>&n=<name>
export function makeJoinLink({ eventId, organizer, secretHex, name }) {
  return "crema:join?" + encQuery({ e: eventId, o: organizer, k: b64url(hexToBytes(secretHex)), n: name || "" });
}
export function parseJoinLink(text) {
  const t = String(text).trim();
  if (!t.startsWith("crema:join?")) return null;
  const q = decQuery(t.slice("crema:join?".length));
  const eventId = q.get("e"), organizer = q.get("o"), k = q.get("k");
  if (!eventId || !/^[0-9a-f]{64}$/.test(organizer || "") || !k) return null;
  let secret;
  try { secret = unb64url(k); } catch { return null; }
  if (secret.length !== 32) return null;
  return { eventId, organizer, secretHex: bytesToHex(secret), name: q.get("n") || eventId };
}
export const newEventSecret = () => bytesToHex(randomBytes(32));

// ── setting up a café's display: what the organizer shows the café's device ONCE, in person ──
// crema:cafe?c=<cafeId>&k=<café secret, 32 bytes b64url>&n=<café name>&e=<event name>
// It carries the café's SECRET key: never publish it, never send it over the event topic.
export function makeCafeLink({ cafeId, secretHex, name, eventName }) {
  return "crema:cafe?" + encQuery({ c: cafeId, k: b64url(hexToBytes(secretHex)), n: name || cafeId, e: eventName || "" });
}
export function parseCafeLink(text) {
  const t = String(text).trim();
  if (!t.startsWith("crema:cafe?")) return null;
  const q = decQuery(t.slice("crema:cafe?".length));
  const cafeId = q.get("c"), k = q.get("k");
  if (!cafeId || !CAFE_ID.test(cafeId) || !k) return null;
  let secret;
  try { secret = unb64url(k); } catch { return null; }
  if (secret.length !== 32) return null;
  const secretHex = bytesToHex(secret);
  return { cafeId, secretHex, publicKey: publicKeyOf(secretHex), name: q.get("n") || cafeId, eventName: q.get("e") || "" };
}

export { bytesToHex, hexToBytes, randomBytes };
