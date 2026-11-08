// Writes crema_core/test/vectors.json from the JS side: fixed keys, a signed catalog, stamps, a join
// link, and sealed frames from three players. crema_core/test/parity.cpp checks the C++ side
// against them (same Ed25519 bytes, opens the frames, same leaderboard).
import fs from "node:fs";
import * as P from "../../protocol/src/index.js";
import { CremaGame } from "../src/engine.js";

const T0 = Date.UTC(2026, 10, 10, 9, 0, 0);
const org = { secretKey: "11".repeat(32) }; org.publicKey = P.publicKeyOf(org.secretKey);
const kA = { secretKey: "22".repeat(32) }; kA.publicKey = P.publicKeyOf(kA.secretKey);
const kB = { secretKey: "33".repeat(32) }; kB.publicKey = P.publicKeyOf(kB.secretKey);
const secretHex = "44".repeat(32);
const catalog = P.signCatalog({ v: 1, eventId: "brno-test", name: "Brno Coffee Week ☕", city: "Brno", version: 1, start: T0 - 3600e3, end: T0 + 7 * 86400e3,
  cafes: [
    { id: "kafec", name: "Kafec", latE6: 49192982, lonE6: 16611540, radiusM: 75, pubkey: kA.publicKey, address: "Orlí 16" },
    { id: "skog", name: "SKØG Urban Hub", latE6: 49193991, lonE6: 16607186, radiusM: 75, pubkey: kB.publicKey },
  ] }, org.secretKey);
const join = P.parseJoinLink(P.makeJoinLink({ eventId: "brno-test", organizer: org.publicKey, secretHex, name: "Brno Coffee Week ☕" }));
let now = T0;
const mk = (sk) => new CremaGame({ join, playerSecretHex: sk, now: () => now });
const orgDev = mk("55".repeat(32)), alice = mk("66".repeat(32)), bob = mk("77".repeat(32));
const frames = [];
frames.push(...orgDev.publishCatalog(catalog).out);
for (const f of frames.slice()) { alice.receive(f); bob.receive(f); }
frames.push(...alice.setName("Alice").out);
const AT = { kafec: { lat: 49.19299, lon: 16.61155, accuracy: 5 }, skog: { lat: 49.19399, lon: 16.60719, accuracy: 5 } };
let r = alice.stamp(P.makeStamp("kafec", kA.secretKey, now), AT.kafec); frames.push(...r.out);
now += 900e3;
r = alice.stamp(P.makeStamp("skog", kB.secretKey, now), AT.skog); frames.push(...r.out);
r = bob.stamp(P.makeStamp("skog", kB.secretKey, now), AT.skog); frames.push(...r.out);
const observer = mk("88".repeat(32));
for (const f of frames) observer.receive(f);
const lb = observer.state().leaderboard;
const out = {
  T0, organizer: org, cafes: { kafec: kA, skog: kB }, secretHex, topic: alice.topic,
  catalog, catalogMessage: "crema/catalog/v1|" + P.canonical((({ sig, ...b }) => b)(catalog)),
  stampAtT0: { kafec: P.makeStamp("kafec", kA.secretKey, T0) },
  joinLink: P.makeJoinLink({ eventId: "brno-test", organizer: org.publicKey, secretHex, name: "Brno Coffee Week ☕" }),
  cafeLink: P.makeCafeLink({ cafeId: "kafec", secretHex: kA.secretKey, name: "Kafec", eventName: "Brno Coffee Week ☕" }),
  framesB64: frames.map((f) => Buffer.from(f).toString("base64")),
  leaderboard: lb.map((x) => ({ author: x.author, name: x.name, cafes: x.cafes, stamps: x.stamps })),
  authors: { org: orgDev.author, alice: alice.author, bob: bob.author },
};
fs.writeFileSync(new URL("../../../crema_core/test/vectors.json", import.meta.url), JSON.stringify(out, null, 1) + "\n");
console.log("vectors:", frames.length, "frames, leaderboard", JSON.stringify(out.leaderboard.map((x) => [x.name || x.author.slice(0, 8), x.cafes])));
