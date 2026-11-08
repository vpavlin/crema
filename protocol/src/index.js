// Crema protocol: a coffee passport you can't fake from your sofa.
//
// - A café's display shows a QR that changes every STEP_S seconds: an Ed25519 signature by the
//   café's key over (café id, time step). Only the display can make it; anyone can check it with the
//   café's public key from the event's signed catalog. No server needed to verify.
// - The organizer signs the event catalog (cafés, their public keys and places, the event window).
// - A player turns a scanned QR into a claim signed with their own secp256k1 identity (the Loam /
//   Keycard curve). Claims are what the leaderboard counts: distinct cafés per player per event.
// - The GPS check runs on the player's phone only; the position is never published.
import { ed25519 } from "@noble/curves/ed25519";
import { secp256k1 } from "@noble/curves/secp256k1";
import { sha256 } from "@noble/hashes/sha256";
import { bytesToHex, hexToBytes, utf8ToBytes, randomBytes } from "@noble/hashes/utils";

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
// Deterministic JSON: sorted keys, no undefined (what gets signed must be byte-identical everywhere)
export function canonical(v) {
  if (v === null || typeof v !== "object") return JSON.stringify(v);
  if (Array.isArray(v)) return "[" + v.map(canonical).join(",") + "]";
  return "{" + Object.keys(v).filter((k) => v[k] !== undefined).sort().map((k) => JSON.stringify(k) + ":" + canonical(v[k])).join(",") + "}";
}

// ── keys ──
export function newCafeKey() {
  const sk = ed25519.utils.randomPrivateKey();
  return { secretKey: bytesToHex(sk), publicKey: bytesToHex(ed25519.getPublicKey(sk)) };
}
export const newOrganizerKey = newCafeKey;
export function newPlayerKey() {
  const sk = secp256k1.utils.randomPrivateKey();
  return { secretKey: bytesToHex(sk), publicKey: bytesToHex(secp256k1.getPublicKey(sk, true)) };
}

// ── the café's rotating stamp ──
export const stepOf = (nowMs) => Math.floor(nowMs / 1000 / STEP_S);
const stampMessage = (cafeId, step) => utf8ToBytes(`crema/stamp/v1|${cafeId}|${step}`);
const CAFE_ID = /^[a-z0-9-]{1,40}$/;

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
// Check a scanned QR against the catalog, now. { ok, reason?, cafe, step }
export function checkStamp(text, catalog, nowMs = Date.now()) {
  const s = parseStamp(text);
  if (!s) return { ok: false, reason: "not a Crema code" };
  const cafe = catalog.cafes.find((c) => c.id === s.cafeId);
  if (!cafe) return { ok: false, reason: "this café isn't part of the event" };
  if (!verifyStampSig(s.cafeId, s.step, s.sig, cafe.pubkey)) return { ok: false, reason: "the code's signature is wrong", cafe };
  const now = stepOf(nowMs);
  if (Math.abs(now - s.step) > SKEW_STEPS) return { ok: false, reason: s.step < now ? "the code has expired - scan it fresh" : "the code is from the future (check your clock)", cafe };
  const t = s.step * STEP_S * 1000;
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
// accuracyM: the phone's reported accuracy; a fix that might be inside the radius counts
export function nearCafe(cafe, lat, lon, accuracyM = 0) {
  const d = distanceM(cafe.lat, cafe.lon, lat, lon);
  return { ok: d - Math.min(accuracyM, 100) <= (cafe.radiusM || DEFAULT_RADIUS_M), distanceM: Math.round(d) };
}

// ── the organizer's signed catalog ──
// { v:1, eventId, name, city, start(ms), end(ms), organizer(ed25519 pub hex), cafes:[{id,name,lat,lon,radiusM,pubkey,address?,url?}] }
export function signCatalog(catalog, organizerSecretHex) {
  const body = { ...catalog, organizer: bytesToHex(ed25519.getPublicKey(hexToBytes(organizerSecretHex))) };
  delete body.sig;
  const sig = ed25519.sign(utf8ToBytes("crema/catalog/v1|" + canonical(body)), hexToBytes(organizerSecretHex));
  return { ...body, sig: bytesToHex(sig) };
}
export function verifyCatalog(signed, expectedOrganizerHex) {
  if (!signed || !signed.sig || !signed.organizer) return false;
  if (expectedOrganizerHex && signed.organizer !== expectedOrganizerHex) return false;
  const body = { ...signed }; delete body.sig;
  try { return ed25519.verify(hexToBytes(signed.sig), utf8ToBytes("crema/catalog/v1|" + canonical(body)), hexToBytes(signed.organizer)); }
  catch { return false; }
}

// ── the player's claim ──
// { v:1, type:"stamp.claim", eventId, cafeId, step, cafeSig, player(secp256k1 compressed hex), at(ms), id, sig }
const claimDigest = (c) => sha256(utf8ToBytes("crema/claim/v1|" + canonical({ v: c.v, type: c.type, eventId: c.eventId, cafeId: c.cafeId, step: c.step, cafeSig: c.cafeSig, player: c.player, at: c.at })));
export function makeClaim(checked, eventId, playerSecretHex, nowMs = Date.now()) {
  const player = bytesToHex(secp256k1.getPublicKey(hexToBytes(playerSecretHex), true));
  const c = { v: 1, type: "stamp.claim", eventId, cafeId: checked.cafe.id, step: checked.step, cafeSig: checked.sig, player, at: nowMs };
  const d = claimDigest(c);
  return { ...c, id: bytesToHex(d).slice(0, 32), sig: secp256k1.sign(d, hexToBytes(playerSecretHex)).toCompactHex() };
}
// Anyone can check a claim: the café really signed that step, the player really signed the claim
export function verifyClaim(c, catalog) {
  if (!c || c.v !== 1 || c.type !== "stamp.claim" || c.eventId !== catalog.eventId) return false;
  const cafe = catalog.cafes.find((x) => x.id === c.cafeId);
  if (!cafe || !verifyStampSig(c.cafeId, c.step, c.cafeSig, cafe.pubkey)) return false;
  const t = c.step * STEP_S * 1000;
  if (t < catalog.start || t > catalog.end) return false;
  // a claim made long after the QR it quotes means the code was passed around, not scanned
  if (Math.abs(c.at - t) > (SKEW_STEPS + 1) * STEP_S * 1000) return false;
  const d = claimDigest(c);
  if (c.id !== bytesToHex(d).slice(0, 32)) return false;
  try { return secp256k1.verify(c.sig, d, c.player); } catch { return false; }
}

// ── the leaderboard: distinct cafés per player; ties go to whoever got there first ──
export function leaderboard(claims, catalog) {
  const byPlayer = new Map();
  const sorted = [...claims].filter((c) => verifyClaim(c, catalog)).sort((a, b) => a.step - b.step || (a.id < b.id ? -1 : 1));
  for (const c of sorted) {
    let p = byPlayer.get(c.player);
    if (!p) byPlayer.set(c.player, (p = { player: c.player, cafes: new Set(), stamps: 0, days: new Set(), reachedAt: 0 }));
    const dayKey = c.cafeId + "@" + Math.floor((c.step * STEP_S) / 86400);
    if (p.days.has(dayKey)) continue;     // one stamp per café per day
    p.days.add(dayKey);
    p.stamps++;
    if (!p.cafes.has(c.cafeId)) { p.cafes.add(c.cafeId); p.reachedAt = c.step * STEP_S * 1000; }
  }
  return [...byPlayer.values()]
    .map((p) => ({ player: p.player, cafes: p.cafes.size, stamps: p.stamps, visited: [...p.cafes], reachedAt: p.reachedAt }))
    .sort((a, b) => b.cafes - a.cafes || a.reachedAt - b.reachedAt || (a.player < b.player ? -1 : 1));
}

export { randomBytes, bytesToHex, hexToBytes };
