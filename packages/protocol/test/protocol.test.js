import { test } from "node:test";
import assert from "node:assert/strict";
import * as C from "../src/index.js";

const T0 = Date.UTC(2026, 10, 10, 9, 0, 0);
const org = C.newKey();
const k1 = C.newKey(), k2 = C.newKey();
const catalog = C.signCatalog({
  v: 1, eventId: "brno-coffee-week-2026", name: "Brno Coffee Week", city: "Brno", version: 1,
  start: T0 - 3600e3, end: T0 + 7 * 86400e3,
  cafes: [
    { id: "cafe-a", name: "Café A", latE6: 49195100, lonE6: 16606800, radiusM: 75, pubkey: k1.publicKey },
    { id: "cafe-b", name: "Café B", latE6: 49200200, lonE6: 16611000, radiusM: 75, pubkey: k2.publicKey },
  ],
}, org.secretKey);

test("a fresh QR from the café's display checks out", () => {
  const qr = C.makeStamp("cafe-a", k1.secretKey, T0);
  assert.match(qr, /^CREMA1\.cafe-a\.[0-9a-z]+\.[A-Za-z0-9_-]{86}$/);
  const r = C.checkStamp(qr, catalog, T0 + 5000);
  assert.equal(r.ok, true, r.reason);
  assert.equal(r.cafe.id, "cafe-a");
});

test("an old QR (photo sent to a friend) has expired; one step of skew is allowed", () => {
  const qr = C.makeStamp("cafe-a", k1.secretKey, T0);
  assert.equal(C.checkStamp(qr, catalog, T0 + 3 * C.STEP_S * 1000).ok, false);
  assert.equal(C.checkStamp(qr, catalog, T0 + C.STEP_S * 1000).ok, true);
});

test("a QR signed with another café's key, or tampered, fails", () => {
  assert.equal(C.checkStamp(C.makeStamp("cafe-a", k2.secretKey, T0), catalog, T0).ok, false);
  const p = C.makeStamp("cafe-a", k1.secretKey, T0).split(".");
  const other = C.makeStamp("cafe-b", k2.secretKey, T0).split(".");
  assert.equal(C.checkStamp([p[0], "cafe-b", p[2], p[3]].join("."), catalog, T0).ok, false);
  assert.equal(C.checkStamp([p[0], p[1], (parseInt(p[2], 36) + 1).toString(36), p[3]].join("."), catalog, T0).ok, false);
  assert.equal(C.checkStamp([p[0], p[1], p[2], other[3]].join("."), catalog, T0).ok, false);
  assert.equal(C.checkStamp("hello", catalog, T0).ok, false);
});

test("outside the event window doesn't count", () => {
  assert.equal(C.checkStamp(C.makeStamp("cafe-a", k1.secretKey, T0 + 30 * 86400e3), catalog, T0 + 30 * 86400e3).ok, false);
});

test("the catalog is signed by the organizer; any edit breaks it", () => {
  assert.equal(C.verifyCatalog(catalog, org.publicKey), true);
  assert.equal(C.verifyCatalog({ ...catalog, cafes: [...catalog.cafes, { id: "evil", pubkey: k1.publicKey }] }), false);
  assert.equal(C.verifyCatalog(catalog, C.newKey().publicKey), false);
});

test("geofence: at the café yes, across town no, with GPS accuracy allowed", () => {
  const cafe = catalog.cafes[0];
  assert.equal(C.nearCafe(cafe, 49.19515, 16.60685).ok, true);
  assert.equal(C.nearCafe(cafe, 49.2100, 16.6200).ok, false);
  assert.equal(C.nearCafe(cafe, 49.1960, 16.6068, 50).ok, true);
});

test("join link round trip; junk is refused", () => {
  const secretHex = C.newEventSecret();
  const link = C.makeJoinLink({ eventId: catalog.eventId, organizer: org.publicKey, secretHex, name: "Brno Coffee Week" });
  assert.deepEqual(C.parseJoinLink(link), { eventId: catalog.eventId, organizer: org.publicKey, secretHex, name: "Brno Coffee Week" });
  assert.equal(C.parseJoinLink("crema:join?e=x&o=zz&k=abc"), null);
  assert.equal(C.parseJoinLink("https://example.com"), null);
});

test("base64url round trip", () => {
  for (const n of [0, 1, 2, 3, 31, 64]) {
    const b = C.randomBytes(n);
    assert.deepEqual(C.unb64url(C.b64url(b)), b);
  }
});

test("café setup link round trip: the device can sign stamps the catalog accepts", () => {
  const link = C.makeCafeLink({ cafeId: "cafe-a", secretHex: k1.secretKey, name: "Café A", eventName: "Brno Coffee Week" });
  const c = C.parseCafeLink(link);
  assert.equal(c.publicKey, k1.publicKey);
  assert.equal(C.checkStamp(C.makeStamp(c.cafeId, c.secretHex, T0), catalog, T0).ok, true);
  assert.equal(C.parseCafeLink("crema:cafe?c=BAD ID&k=x"), null);
});

test("the event map: mapOf accepts the signed shape, rejects junk, and the signature covers it", () => {
  const org = C.newKey();
  const map = { cid: "zDvZRwzm4Hh6V1J3XHxi2k9NkmRoyGcMcbBzgzaD3ZHAYXZ2AGAk", sha256: "ab".repeat(32), size: 1000, bboxE6: [16593000, 49176700, 16636100, 49215600], maxzoom: 16 };
  const c = C.signCatalog({ v: 1, eventId: "e", version: 1, cafes: [], map }, org.secretKey);
  assert.deepEqual(C.mapOf(c), { ...map, providers: [] });
  assert.equal(C.mapOf({ map: { ...map, sha256: "nope" } }), null);
  assert.equal(C.mapOf({ map: { ...map, bboxE6: [1.5, 2, 3, 4] } }), null);
  assert.equal(C.mapOf({}), null);
  assert.ok(C.verifyCatalog(c, org.publicKey));
  assert.ok(!C.verifyCatalog({ ...c, map: { ...map, cid: "zDvSwapped" + map.cid.slice(10) } }, org.publicKey), "a swapped map CID breaks the signature");
});
