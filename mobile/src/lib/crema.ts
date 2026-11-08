// Crema on the phone: the player's identity, the events they joined (each a CremaGame from the
// shared engine), the transport (the Loam shared node, else our own node) and café mode.
//
// Local-first: a stamp goes into the log and onto disk before anything is sent; the send is only
// the fast path, catch-up (RBSR) delivers whatever it missed. Nothing here awaits the network on
// the way to the UI.
import { secp256k1 } from "@noble/curves/secp256k1.js";
import * as transport from "./loam-transport";
import { fileStore, secretStore } from "./store";
import * as FS from "expo-file-system/legacy";
import { sha256 } from "@noble/hashes/sha2.js";
import { Buffer } from "buffer";
import * as storage from "./loam-transport-pkg/src/loam-storage";
import { crumb } from "./crashlog";
import * as P from "../../../packages/protocol/src/index.js";
import { CremaGame } from "../../../packages/engine/src/engine.js";
import { address, fromHex } from "../../../packages/loam-sync/dist/signing.js";

export const APP_ID = "xyz.vpavlin.crema";
const K_PLAYER = "crema-player-key", K_SHARED = "crema-shared-node", K_CAFE = "crema-cafe";
const HELLO_EVERY_MS = 5 * 60_000;

export type Join = { eventId: string; organizer: string; secretHex: string; name: string };
// The event's own map (docs/adr/0002): a .pmtiles file on Logos Storage, named (CID + sha256) in the
// signed catalog, fetched through Loam's Storage and kept here. Never a tile server.
export type MapState = { state: "none" | "no-storage" | "fetching" | "ready" | "failed"; path?: string; bytes?: number; total?: number; error?: string; sha256?: string; maxzoom?: number };

export type CafeMode = { cafeId: string; secretHex: string; publicKey: string; name: string; eventName: string };

const hexOf = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");

type Listener = () => void;
class Crema {
  playerSecret = "";
  games = new Map<string, CremaGame>();        // eventId -> game
  current = "";                                // the event on screen
  net = { status: "Starting…", started: false, shared: true, error: "" };
  cafe: CafeMode | null = null;
  private listeners = new Set<Listener>();
  private booted: Promise<void> | null = null;

  on(fn: Listener) { this.listeners.add(fn); return () => { this.listeners.delete(fn); }; }
  emit() { for (const fn of this.listeners) { try { fn(); } catch { /* */ } } }
  game(): CremaGame | null { return this.games.get(this.current) || null; }
  author(): string { return this.playerSecret ? address(secp256k1.getPublicKey(fromHex(this.playerSecret), true)) : ""; }

  // ── persistence ──
  private async saveGame(g: CremaGame) { await fileStore.set("game-" + g.join.eventId, JSON.stringify(g.toJSON())); }
  private async saveIndex() {
    await fileStore.set("events", JSON.stringify({ current: this.current, joins: [...this.games.values()].map((g) => g.join) }));
  }

  // ── boot: identity → events → (backend choice) → transport ──
  boot(): Promise<void> {
    if (this.booted) return this.booted;
    this.booted = (async () => {
      let sk = await secretStore.get(K_PLAYER);
      if (!sk) { sk = hexOf(secp256k1.utils.randomSecretKey()); await secretStore.set(K_PLAYER, sk); }
      this.playerSecret = sk;
      try { const c = await secretStore.get(K_CAFE); this.cafe = c ? JSON.parse(c) : null; } catch { this.cafe = null; }
      const idx = JSON.parse((await fileStore.get("events")) || "{}");
      for (const j of (idx.joins || []) as Join[]) {
        let state = null;
        try { state = JSON.parse((await fileStore.get("game-" + j.eventId)) || "null"); } catch { /* fresh */ }
        this.games.set(j.eventId, new CremaGame({ join: j, playerSecretHex: sk, state }));
      }
      this.current = idx.current && this.games.has(idx.current) ? idx.current : ([...this.games.keys()][0] || "");
      crumb("boot events=" + this.games.size);
      this.emit();
      await this.startTransport();
    })();
    return this.booted;
  }

  // loam-integrate-app §2: the backend choice comes before anything touches the transport
  private async startTransport() {
    try { this.net.shared = (await secretStore.get(K_SHARED)) !== "0"; } catch { this.net.shared = true; }
    transport.preferServiceBackend(this.net.shared, APP_ID);
    const author = this.author();
    try {
      await transport.start({
        deviceId: author,
        topics: [...this.games.values()].map((g) => g.topic),
        onReceive: (topic: string, candidates: Uint8Array[]) => this.onReceive(topic, candidates),
        onStatus: (s: string) => { this.net.status = s; this.emit(); },
      });
      this.net.started = true; this.net.status = "Connected";
      crumb("transport started " + (transport.usingServiceBackend() ? "shared" : "own"));
      for (const g of this.games.values()) this.hello(g);
      transport.storeSync((t: string, c: Uint8Array[]) => this.onReceive(t, c)).catch(() => {});
    } catch (e: any) {
      this.net.error = String(e?.message || e); this.net.status = "Offline"; crumb("start failed " + this.net.error);
    }
    this.emit();
    setInterval(() => { for (const g of this.games.values()) this.hello(g); }, HELLO_EVERY_MS);
  }

  private hello(g: CremaGame) { if (this.net.started) this.publish(g, [g.hello()]); }
  private publish(g: CremaGame, frames: Uint8Array[]) {
    for (const f of frames) transport.publishSealed(g.topic, f).catch((e: any) => crumb("send failed " + (e?.message || e)));
  }

  private onReceive(topic: string, candidates: Uint8Array[]): boolean {
    const g = [...this.games.values()].find((x) => x.topic === topic);
    if (!g) return false;
    let changed = false;
    for (const bytes of candidates) {
      const r = g.receive(bytes);
      if (r.out.length) this.publish(g, r.out);
      changed = changed || r.changed;
    }
    if (changed) { this.saveGame(g); this.emit(); }
    return changed;
  }

  // ── scanning: a join QR, a café's stamp, or a café setup QR ──
  async handleScan(text: string, getFix: () => Promise<{ lat: number; lon: number; accuracy: number }>):
    Promise<{ ok: boolean; message: string; kind: "join" | "stamp" | "cafe" | "unknown" }> {
    const join = P.parseJoinLink(text);
    if (join) return { ...(await this.join(join)), kind: "join" };
    const cafe = P.parseCafeLink(text);
    if (cafe) { await this.setCafeMode(cafe); return { ok: true, message: `This device is now ${cafe.name}'s stamp display`, kind: "cafe" }; }
    if (!P.parseStamp(text)) return { ok: false, message: "That isn't a Crema code", kind: "unknown" };
    const g = this.game();
    if (!g) return { ok: false, message: "Join an event first: scan the organizer's event QR", kind: "stamp" };
    let fix;
    try { fix = await getFix(); } catch (e: any) { return { ok: false, message: String(e?.message || "No location"), kind: "stamp" }; }
    const r = g.stamp(text, fix);
    if (!r.ok) return { ok: false, message: r.reason || "Not stamped", kind: "stamp" };
    await this.saveGame(g);                    // on disk before the send (charter rule 3)
    this.publish(g, r.out);
    this.emit();
    return { ok: true, message: r.already ? `${r.cafe.name} again today - it counts once. Enjoy the coffee ☕` : `Stamped: ${r.cafe.name} ☕`, kind: "stamp" };
  }

  async join(j: Join): Promise<{ ok: boolean; message: string }> {
    if (!this.games.has(j.eventId)) {
      const g = new CremaGame({ join: j, playerSecretHex: this.playerSecret });
      this.games.set(j.eventId, g);
      await this.saveGame(g);
      if (this.net.started) { await transport.join([g.topic]).catch(() => {}); this.hello(g); }
    }
    this.current = j.eventId;
    await this.saveIndex();
    this.emit();
    return { ok: true, message: `Joined ${j.name}` };
  }

  select(eventId: string) { if (this.games.has(eventId)) { this.current = eventId; this.saveIndex(); this.emit(); } }

  async setName(name: string) {
    const g = this.game(); if (!g) return;
    const r = g.setName(name); await this.saveGame(g); this.publish(g, r.out); this.emit();
  }
  async setShare(on: boolean) {
    const g = this.game(); if (!g) return;
    const r = g.setShare(on); await this.saveGame(g); this.publish(g, r.out); this.emit();
  }

  // Applies on next launch (the backend choice is locked at start)
  async setSharedNode(on: boolean) { await secretStore.set(K_SHARED, on ? "1" : "0"); this.net.shared = on; this.emit(); }

  // ── café mode: this device shows a café's rotating stamp ──
  // ── the event map ──
  maps = new Map<string, MapState>();          // sha256 -> state
  private mapJobs = new Set<string>();
  mapFor(g: CremaGame | null): MapState {
    const m = g ? P.mapOf(g.state().catalog) : null;
    if (!m) return { state: "none" };
    const st = this.maps.get(m.sha256);
    if (!st) { this.ensureMap(g!); return { state: "fetching", sha256: m.sha256, maxzoom: m.maxzoom }; }
    return { ...st, maxzoom: m.maxzoom };
  }
  private setMap(sha: string, st: MapState) { this.maps.set(sha, { ...st, sha256: sha }); this.emit(); }
  async ensureMap(g: CremaGame) {
    const m = P.mapOf(g.state().catalog);
    if (!m || this.mapJobs.has(m.sha256)) return;
    this.mapJobs.add(m.sha256);
    const dir = (FS.documentDirectory || "") + "maps/";
    const final = dir + m.sha256 + ".pmtiles";
    try {
      await FS.makeDirectoryAsync(dir, { intermediates: true }).catch(() => {});
      const have = await FS.getInfoAsync(final);
      if (have.exists && (have as any).size === m.size) { this.setMap(m.sha256, { state: "ready", path: final }); return; }
      if (!(await storage.available())) {
        this.setMap(m.sha256, { state: "no-storage", error: "The map comes over Logos Storage: install Loam and turn Storage on" });
        return;
      }
      this.setMap(m.sha256, { state: "fetching", bytes: 0, total: m.size });
      let job = await storage.fetch(m.cid, m.providers);
      const t0 = Date.now();
      while (job && job.state !== "done") {
        if (job.state === "failed" || job.state === "cancelled") throw new Error(job.error || job.state);
        if (Date.now() - t0 > 30 * 60_000) throw new Error("timed out");
        this.setMap(m.sha256, { state: "fetching", bytes: job.bytes || 0, total: job.total || m.size });
        await new Promise((r) => setTimeout(r, 2000));
        job = await storage.status(m.cid);
      }
      if (!job) throw new Error("Loam's Storage did not answer");
      const tmp = dir + m.sha256 + ".part";
      await storage.saveTo(m.cid, tmp.replace(/^file:\/\//, ""));
      // Loam knows CIDs only: the bytes must be the ones the organizer signed
      const b64 = await FS.readAsStringAsync(tmp, { encoding: FS.EncodingType.Base64 });
      const got = hexOf(sha256(new Uint8Array(Buffer.from(b64, "base64"))));
      if (got !== m.sha256) { await FS.deleteAsync(tmp, { idempotent: true }); throw new Error("the downloaded map isn't the one the organizer signed"); }
      await FS.moveAsync({ from: tmp, to: final });
      crumb("map ready " + m.sha256.slice(0, 12) + " " + m.size + "B");
      this.setMap(m.sha256, { state: "ready", path: final });
    } catch (e: any) {
      crumb("map failed " + (e?.message || e));
      this.setMap(m.sha256, { state: "failed", error: String(e?.message || e) });
    } finally {
      this.mapJobs.delete(m.sha256);
    }
  }
  retryMap() { const g = this.game(); const m = g && P.mapOf(g.state().catalog); if (m) { this.maps.delete(m.sha256); this.ensureMap(g!); } }

  async setCafeMode(c: CafeMode | null) {
    this.cafe = c;
    if (c) await secretStore.set(K_CAFE, JSON.stringify(c)); else await secretStore.set(K_CAFE, "");
    this.emit();
  }
  cafeStamp(now = Date.now()) { return this.cafe ? P.makeStamp(this.cafe.cafeId, this.cafe.secretHex, now) : ""; }
}

export const crema = new Crema();
export { P };
