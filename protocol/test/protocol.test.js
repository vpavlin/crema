import { test } from "node:test";
import assert from "node:assert/strict";
import * as C from "../src/index.js";

const T0 = Date.UTC(2026, 10, 10, 9, 0, 0);          // an event morning
const org = C.newOrganizerKey();
const k1 = C.newCafeKey(), k2 = C.newCafeKey();
const catalog = C.signCatalog({
  v: 1, eventId: "brno-coffee-week-2026", name: "Brno Coffee Week", city: "Brno",
  start: T0 - 3600e3, end: T0 + 7 * 86400e3,
  cafes: [
    { id: "cafe-a", name: "Café A", lat: 49.1951, lon: 16.6068, radiusM: 75, pubkey: k1.publicKey },
    { id: "cafe-b", name: "Café B", lat: 49.2002, lon: 16.6110, radiusM: 75, pubkey: k2.publicKey },
  ],
}, org.secretKey);

test("a fresh QR from the café's display checks out", () => {
  const qr = C.makeStamp("cafe-a", k1.secretKey, T0);
  assert.match(qr, /^CREMA1\.cafe-a\.[0-9a-z]+\.[A-Za-z0-9_-]{86}$/);
  const r = C.checkStamp(qr, catalog, T0 + 5000);
  assert.equal(r.ok, true, r.reason);
  assert.equal(r.cafe.id, "cafe-a");
});

test("an old QR (photo sent to a friend) has expired", () => {
  const qr = C.makeStamp("cafe-a", k1.secretKey, T0);
  assert.equal(C.checkStamp(qr, catalog, T0 + 3 * C.STEP_S * 1000).ok, false);
  assert.equal(C.checkStamp(qr, catalog, T0 + C.STEP_S * 1000).ok, true, "one step of skew is allowed");
});

test("a QR signed with another café's key, or tampered, fails", () => {
  const forged = C.makeStamp("cafe-a", k2.secretKey, T0);
  assert.equal(C.checkStamp(forged, catalog, T0).ok, false);
  const qr = C.makeStamp("cafe-a", k1.secretKey, T0);
  const p = qr.split(".");
  const other = C.makeStamp("cafe-b", k2.secretKey, T0).split(".");
  assert.equal(C.checkStamp([p[0], "cafe-b", p[2], p[3]].join("."), catalog, T0).ok, false, "café id swapped");
  assert.equal(C.checkStamp([p[0], p[1], (parseInt(p[2], 36) + 1).toString(36), p[3]].join("."), catalog, T0).ok, false, "step changed");
  assert.equal(C.checkStamp([p[0], p[1], p[2], other[3]].join("."), catalog, T0).ok, false, "another sig");
  assert.equal(C.checkStamp("hello", catalog, T0).ok, false);
});

test("outside the event window doesn't count", () => {
  const qr = C.makeStamp("cafe-a", k1.secretKey, T0 + 30 * 86400e3);
  assert.equal(C.checkStamp(qr, catalog, T0 + 30 * 86400e3).ok, false);
});

test("the catalog is signed by the organizer; any edit breaks it", () => {
  assert.equal(C.verifyCatalog(catalog, org.publicKey), true);
  assert.equal(C.verifyCatalog({ ...catalog, cafes: [...catalog.cafes, { id: "evil", pubkey: k1.publicKey }] }), false);
  assert.equal(C.verifyCatalog(catalog, C.newOrganizerKey().publicKey), false, "another organizer");
});

test("geofence: at the café yes, across town no, with GPS accuracy allowed", () => {
  const cafe = catalog.cafes[0];
  assert.equal(C.nearCafe(cafe, 49.19515, 16.60685).ok, true);
  assert.equal(C.nearCafe(cafe, 49.2100, 16.6200).ok, false);
  assert.equal(C.nearCafe(cafe, 49.1960, 16.6068, 50).ok, true, "~100 m off with 50 m accuracy");
});

test("claims verify; forged or late claims don't", () => {
  const p = C.newPlayerKey();
  const qr = C.makeStamp("cafe-a", k1.secretKey, T0);
  const claim = C.makeClaim(C.checkStamp(qr, catalog, T0 + 1000), catalog.eventId, p.secretKey, T0 + 1000);
  assert.equal(C.verifyClaim(claim, catalog), true);
  assert.equal(C.verifyClaim({ ...claim, cafeId: "cafe-b" }, catalog), false, "moved to another café");
  assert.equal(C.verifyClaim({ ...claim, player: C.newPlayerKey().publicKey }, catalog), false, "stolen by another player");
  const late = C.makeClaim(C.checkStamp(qr, catalog, T0 + 1000), catalog.eventId, p.secretKey, T0 + 10 * 60e3);
  assert.equal(C.verifyClaim(late, catalog), false, "a QR claimed 10 min later was passed around");
});

test("leaderboard: distinct cafés, one stamp per café per day, earliest wins ties", () => {
  const alice = C.newPlayerKey(), bob = C.newPlayerKey();
  const claimAt = (pl, cafe, key, t) => C.makeClaim(C.checkStamp(C.makeStamp(cafe, key.secretKey, t), catalog, t), catalog.eventId, pl.secretKey, t);
  const claims = [
    claimAt(alice, "cafe-a", k1, T0),
    claimAt(alice, "cafe-a", k1, T0 + 3600e3),       // same café same day: ignored
    claimAt(alice, "cafe-b", k2, T0 + 7200e3),
    claimAt(bob, "cafe-b", k2, T0 + 60e3),
    claimAt(bob, "cafe-a", k1, T0 + 86400e3),        // next day
    claimAt(bob, "cafe-a", k1, T0 + 2 * 86400e3),    // another day: a stamp, but no new café
  ];
  const lb = C.leaderboard([...claims, { ...claims[0], sig: claims[3].sig }], catalog);
  assert.equal(lb.length, 2);
  assert.equal(lb[0].player, alice.publicKey, "both have 2 cafés; Alice got there first");
  assert.deepEqual([lb[0].cafes, lb[0].stamps], [2, 2]);
  assert.deepEqual([lb[1].cafes, lb[1].stamps], [2, 3]);
});

test("base64url round trip", () => {
  for (const n of [0, 1, 2, 3, 31, 64]) {
    const b = C.randomBytes(n);
    assert.deepEqual(C.unb64url(C.b64url(b)), b);
  }
});
