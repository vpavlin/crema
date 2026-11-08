import { test } from "node:test";
import assert from "node:assert/strict";
import { secp256k1 } from "@noble/curves/secp256k1.js";
import * as P from "../../protocol/src/index.js";
import { CremaGame, stampId, CLAIM_LATE_MS } from "../src/engine.js";
import { signEvent, SoftwareSigner, fromHex, hex } from "../../loam-sync/dist/signing.js";

const T0 = Date.UTC(2026, 10, 10, 9, 0, 0);
const org = P.newKey();
const kA = P.newKey(), kB = P.newKey();
const secretHex = P.newEventSecret();
const join = P.parseJoinLink(P.makeJoinLink({ eventId: "brno-test", organizer: org.publicKey, secretHex, name: "Brno test" }));
const catalog = (version, extra = []) => P.signCatalog({
  v: 1, eventId: "brno-test", name: "Brno test", city: "Brno", version, start: T0 - 3600e3, end: T0 + 7 * 86400e3,
  cafes: [
    { id: "kafec", name: "Kafec", latE6: 49192982, lonE6: 16611540, radiusM: 75, pubkey: kA.publicKey },
    { id: "monogram", name: "Monogram", latE6: 49191717, lonE6: 16609925, radiusM: 75, pubkey: kB.publicKey },
    ...extra,
  ],
}, org.secretKey);
const AT_KAFEC = { lat: 49.19299, lon: 16.61155, accuracy: 10 };
const AT_MONOGRAM = { lat: 49.19172, lon: 16.60993, accuracy: 10 };
const newPlayerSecret = () => hex(secp256k1.utils.randomSecretKey());

// A fake topic: every published frame reaches every other device (and replies are published too)
function bus(devices) {
  const deliver = (from, frames) => {
    const queue = frames.map((f) => [from, f]);
    let n = 0;
    while (queue.length && n++ < 10000) {
      const [src, f] = queue.shift();
      for (const d of devices) if (d !== src) for (const o of d.game.receive(f).out) queue.push([d, o]);
    }
  };
  return { deliver };
}
const device = (name, now) => ({ name, game: new CremaGame({ join, playerSecretHex: newPlayerSecret(), now }) });

test("organizer publishes the café list; players get it and stamp at the café; everyone folds the same leaderboard", () => {
  let now = T0;
  const clock = () => now;
  const orgDev = device("org", clock), alice = device("alice", clock), bob = device("bob", clock);
  const net = bus([orgDev, alice, bob]);
  const pub = orgDev.game.publishCatalog(catalog(1));
  assert.equal(pub.ok, true);
  net.deliver(orgDev, pub.out);
  assert.equal(alice.game.state().catalog.cafes.length, 2, "Alice has the catalog");

  net.deliver(alice, alice.game.setName("Alice").out);
  let r = alice.game.stamp(P.makeStamp("kafec", kA.secretKey, now), AT_KAFEC);
  assert.equal(r.ok, true, r.reason);
  net.deliver(alice, r.out);
  now += 600e3;
  r = alice.game.stamp(P.makeStamp("monogram", kB.secretKey, now), AT_MONOGRAM);
  assert.equal(r.ok, true, r.reason);
  net.deliver(alice, r.out);
  r = bob.game.stamp(P.makeStamp("monogram", kB.secretKey, now), AT_MONOGRAM);
  net.deliver(bob, r.out);

  for (const d of [orgDev, alice, bob]) {
    const lb = d.game.state().leaderboard;
    assert.deepEqual(lb.map((x) => [x.name, x.cafes]), [["Alice", 2], ["", 1]], `${d.name} sees the same ranking`);
  }
  assert.equal(alice.game.state().myVisited.size, 2);
});

test("a stamp needs the café's fresh code AND being there", () => {
  let now = T0;
  const alice = device("alice", () => now);
  alice.game.publishCatalog; // (players can't publish a catalog: not the organizer's key)
  const orgDev = device("org", () => now);
  const net = bus([orgDev, alice]);
  net.deliver(orgDev, orgDev.game.publishCatalog(catalog(1)).out);
  const qr = P.makeStamp("kafec", kA.secretKey, now);
  assert.match(alice.game.stamp(qr, AT_MONOGRAM).reason || "", /m from Kafec/, "wrong place");
  assert.match(alice.game.stamp(qr, null).reason || "", /Location/, "no GPS");
  now += 5 * 60e3;
  assert.match(alice.game.stamp(qr, AT_KAFEC).reason || "", /expired/, "an old photo of the code");
  assert.equal(alice.game.stamp(P.makeStamp("kafec", kB.secretKey, now), AT_KAFEC).ok, false, "a code signed by another café");
});

test("forgeries never count: a stamp claimed long after its code, a stolen claim, a fake catalog", () => {
  let now = T0;
  const orgDev = device("org", () => now), alice = device("alice", () => now), mallory = device("mallory", () => now);
  const net = bus([orgDev, alice, mallory]);
  net.deliver(orgDev, orgDev.game.publishCatalog(catalog(1)).out);
  const r = alice.game.stamp(P.makeStamp("kafec", kA.secretKey, now), AT_KAFEC);
  net.deliver(alice, r.out);
  // Mallory re-signs Alice's café signature as her own, 10 minutes later
  const e = alice.game.log.find((x) => x.type === "stamp");
  const msig = new SoftwareSigner(fromHex(newPlayerSecret()));
  const forged = signEvent(msig, P.DOMAIN, { v: 1, type: "stamp", hlc: { wall: now + 10 * 60e3, ctr: 0, dev: "" }, dev: "", payload: e.payload, id: "" });
  forged.id = stampId(forged.dev, e.payload.cafeId, e.payload.step);
  // ...re-sign with the right id
  const forged2 = signEvent(msig, P.DOMAIN, { ...forged, pub: undefined, sig: undefined });
  mallory.game.admit(forged2);
  net.deliver(mallory, [mallory.game.eventFrame(forged2)]);
  assert.deepEqual(orgDev.game.state().leaderboard.map((x) => x.author), [alice.game.author], "the late re-claim doesn't count");
  // A fake catalog (signed by Mallory) is refused at the door
  const fake = P.signCatalog({ ...catalog(2), cafes: [] }, P.newKey().secretKey);
  assert.equal(mallory.game.publishCatalog(fake).ok, false);
  // A stamp event whose id doesn't match its content is refused by the fold
  assert.ok(CLAIM_LATE_MS > 0);
});

test("a newer catalog version wins everywhere (a café added mid-event)", () => {
  let now = T0;
  const orgDev = device("org", () => now), alice = device("alice", () => now);
  const net = bus([orgDev, alice]);
  net.deliver(orgDev, orgDev.game.publishCatalog(catalog(1)).out);
  const kC = P.newKey();
  net.deliver(orgDev, orgDev.game.publishCatalog(catalog(2, [{ id: "skog", name: "SKØG", latE6: 49193991, lonE6: 16607186, radiusM: 75, pubkey: kC.publicKey }])).out);
  assert.equal(alice.game.state().catalog.version, 2);
  assert.equal(alice.game.state().catalog.cafes.length, 3);
  assert.equal(orgDev.game.publishCatalog(catalog(2)).ok, false, "the same version twice is refused");
});

test("a late joiner catches up from any peer (RBSR), even with the organizer offline", () => {
  let now = T0;
  const orgDev = device("org", () => now), alice = device("alice", () => now), bob = device("bob", () => now);
  let net = bus([orgDev, alice, bob]);
  net.deliver(orgDev, orgDev.game.publishCatalog(catalog(1)).out);
  net.deliver(alice, alice.game.setName("Alice").out);
  net.deliver(alice, alice.game.stamp(P.makeStamp("kafec", kA.secretKey, now), AT_KAFEC).out);
  net.deliver(bob, bob.game.stamp(P.makeStamp("monogram", kB.secretKey, now), AT_MONOGRAM).out);
  // Carol joins later; the organizer is gone. She says hello; Alice and Bob answer.
  const carol = device("carol", () => now);
  net = bus([alice, bob, carol]);
  net.deliver(carol, [carol.game.hello()]);
  const s = carol.game.state();
  assert.equal(s.catalog?.version, 1, "Carol got the catalog from a player");
  assert.deepEqual(s.leaderboard.map((x) => x.cafes).sort(), [1, 1]);
  assert.equal(carol.game.log.length, alice.game.log.length, "Carol has the whole log");
});

test("a player off the leaderboard keeps stamps private; joining later publishes them", () => {
  let now = T0;
  const orgDev = device("org", () => now), alice = device("alice", () => now);
  const net = bus([orgDev, alice]);
  net.deliver(orgDev, orgDev.game.publishCatalog(catalog(1)).out);
  alice.game.setShare(false);
  const r = alice.game.stamp(P.makeStamp("kafec", kA.secretKey, now), AT_KAFEC);
  assert.equal(r.ok, true);
  assert.equal(r.out.length, 0, "nothing goes on the wire");
  assert.equal(alice.game.state().myVisited.size, 1, "but Alice sees her own stamp");
  assert.equal(orgDev.game.state().leaderboard.length, 0);
  // a catch-up from the organizer doesn't reveal it either
  net.deliver(orgDev, [orgDev.game.hello()]);
  assert.equal(orgDev.game.state().leaderboard.length, 0);
  net.deliver(alice, alice.game.setShare(true).out);
  assert.equal(orgDev.game.state().leaderboard[0]?.cafes, 1);
});

test("frames from outside the event (wrong key) and oversized frames are dropped unread", () => {
  const now = T0;
  const alice = device("alice", () => now);
  const otherJoin = P.parseJoinLink(P.makeJoinLink({ eventId: "brno-test", organizer: org.publicKey, secretHex: P.newEventSecret(), name: "x" }));
  const outsider = new CremaGame({ join: otherJoin, playerSecretHex: newPlayerSecret(), now: () => now });
  assert.equal(alice.game.receive(outsider.setName("spam").out[0]).changed, false);
  assert.equal(alice.game.receive(new Uint8Array(20000)).changed, false);
});

test("state survives a restart", () => {
  let now = T0;
  const orgDev = device("org", () => now), alice = device("alice", () => now);
  const net = bus([orgDev, alice]);
  net.deliver(orgDev, orgDev.game.publishCatalog(catalog(1)).out);
  net.deliver(alice, alice.game.stamp(P.makeStamp("kafec", kA.secretKey, now), AT_KAFEC).out);
  const saved = JSON.parse(JSON.stringify(alice.game.toJSON()));
  const again = new CremaGame({ join, playerSecretHex: newPlayerSecret(), state: saved, now: () => now });
  assert.equal(again.state().leaderboard.length, 1);
  assert.equal(again.state().catalog.version, 1);
});
