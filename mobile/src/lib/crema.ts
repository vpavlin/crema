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
import { locationFix } from "./native";
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
export type MapState = {
  state: "none" | "no-storage" | "fetching" | "ready" | "failed"; path?: string; bytes?: number; total?: number; error?: string; sha256?: string; maxzoom?: number;
  // 0.3 progressive: pieces (one Storage block each) land in pieceDir as p0, p1, ...; the map page shows each zoom once its bytes are in
  pieceDir?: string; piecesDone?: number; piecesTotal?: number; meta?: { size: number; dirEnd: number; minzoom: number; zoomEnds: number[][] };
};

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
      if (this.game()) this.mapFor(this.game());   // keep fetching the map from where it stopped, whatever tab is open
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
    if (changed) { this.saveGame(g); this.emit(); if (g === this.game()) this.mapFor(g); }   // a new catalog may bring a map: start fetching now
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
  // ── where am I (one fix on request; never leaves the phone) ──
  here: { lat: number; lon: number; accuracy: number; at: number } | null = null;
  locating = false;
  async locate(): Promise<{ ok: boolean; message?: string }> {
    if (this.locating) return { ok: false, message: "Already locating…" };
    this.locating = true; this.emit();
    try {
      const f = await locationFix(20_000);
      this.here = { ...f, at: Date.now() };
      return { ok: true };
    } catch (e: any) {
      return { ok: false, message: String(e?.message || e) };
    } finally { this.locating = false; this.emit(); }
  }
  // the event's cafés by distance from `here` (closest first); [] without a fix
  nearby(g: CremaGame | null): { cafe: any; m: number; visited: boolean }[] {
    const s = g ? g.state() : null;
    if (!this.here || !s?.catalog) return [];
    const h = this.here;
    return s.catalog.cafes
      .map((c: any) => ({ cafe: c, m: P.distanceM(h.lat, h.lon, P.cafeLat(c), P.cafeLon(c)), visited: s.myVisited.has(c.id) }))
      .sort((a: any, b: any) => a.m - b.m);
  }

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
    const root = (FS.documentDirectory || "") + "maps/";
    try {
      await FS.makeDirectoryAsync(root, { intermediates: true }).catch(() => {});
      if (m.pieces) await this.fetchPieces(m, root);
      else await this.fetchWhole(m, root);
    } catch (e: any) {
      crumb("map failed " + (e?.message || e));
      this.setMap(m.sha256, { ...(this.maps.get(m.sha256) || {}), state: "failed", error: String(e?.message || e) });
    } finally {
      this.mapJobs.delete(m.sha256);
    }
  }
  // one CID through Loam's Storage into `path` (a file we own); relays drop circuits, so retry a few times
  private async fetchCid(cid: string, providers: any[], path: string, onProgress?: (b: number) => void) {
    let lastErr = "";
    for (let attempt = 0; attempt < 6; attempt++) {
      try {
        let job = await storage.fetch(cid, providers);
        const t0 = Date.now();
        while (job && job.state !== "done") {
          if (job.state === "failed" || job.state === "cancelled") throw new Error(job.error || job.state);
          if (Date.now() - t0 > 10 * 60_000) throw new Error("timed out");
          if (onProgress) onProgress(job.bytes || 0);
          await new Promise((r) => setTimeout(r, 1500));
          job = await storage.status(cid);
        }
        if (!job) throw new Error("Loam's Storage did not answer");
        await storage.saveTo(cid, path.replace(/^file:\/\//, ""));
        return;
      } catch (e: any) {
        lastErr = String(e?.message || e);
        crumb("piece " + cid.slice(-8) + " attempt " + (attempt + 1) + ": " + lastErr);
        await new Promise((r) => setTimeout(r, Math.min(30_000, 3000 * (attempt + 1))));
      }
    }
    throw new Error(lastErr || "fetch failed");
  }
  private async fetchWhole(m: any, root: string) {
    const final = root + m.sha256 + ".pmtiles";
    const have = await FS.getInfoAsync(final);
    if (have.exists && (have as any).size === m.size) { this.setMap(m.sha256, { state: "ready", path: final }); return; }
    if (!(await storage.available())) { this.setMap(m.sha256, { state: "no-storage", error: "The map comes over Logos Storage: install Loam and turn Storage on" }); return; }
    this.setMap(m.sha256, { state: "fetching", bytes: 0, total: m.size });
    const tmp = root + m.sha256 + ".part";
    await this.fetchCid(m.cid, m.providers, tmp, (b) => this.setMap(m.sha256, { state: "fetching", bytes: b, total: m.size }));
    const b64 = await FS.readAsStringAsync(tmp, { encoding: FS.EncodingType.Base64 });
    if (hexOf(sha256(new Uint8Array(Buffer.from(b64, "base64")))) !== m.sha256) {
      await FS.deleteAsync(tmp, { idempotent: true });
      throw new Error("the downloaded map isn't the one the organizer signed");
    }
    await FS.moveAsync({ from: tmp, to: final });
    this.setMap(m.sha256, { state: "ready", path: final });
  }
  // 0.3: pieces in file order (coarse zooms first); each shows on the map as soon as it lands
  private async fetchPieces(m: any, root: string) {
    const pc = m.pieces;
    const dir = root + m.sha256 + "/";
    await FS.makeDirectoryAsync(dir, { intermediates: true }).catch(() => {});
    const total = pc.cids.length;
    const meta = { size: m.size, dirEnd: pc.dirEnd, minzoom: m.minzoom, zoomEnds: pc.zoomEnds };
    const base = { pieceDir: dir, piecesTotal: total, meta, total: m.size };
    const expect = (i: number) => Math.min(pc.size, m.size - i * pc.size);
    let done = 0;
    while (done < total) {
      const info = await FS.getInfoAsync(dir + "p" + done);
      if (!(info.exists && (info as any).size === expect(done))) break;
      done++;
    }
    const okFile = dir + "verified";
    if (done === total && (await FS.getInfoAsync(okFile)).exists) {   // checked on an earlier run: no need to hash 1-6 MB again
      this.setMap(m.sha256, { ...base, piecesDone: total, state: "ready" });
      return;
    }
    if (done < total && !(await storage.available())) {
      this.setMap(m.sha256, { ...base, piecesDone: done, state: "no-storage", error: "The map comes over Logos Storage: install Loam and turn Storage on" });
      return;
    }
    this.setMap(m.sha256, { ...base, piecesDone: done, state: done < total ? "fetching" : "ready" });
    for (let i = done; i < total; i++) {
      const path = dir + "p" + i;
      await this.fetchCid(pc.cids[i], m.providers, path);
      const info = await FS.getInfoAsync(path);
      if (!info.exists || (info as any).size !== expect(i)) { await FS.deleteAsync(path, { idempotent: true }); throw new Error("piece " + (i + 1) + " has the wrong size"); }
      crumb("map piece " + (i + 1) + "/" + total);
      this.setMap(m.sha256, { ...base, piecesDone: i + 1, state: i + 1 < total ? "fetching" : "ready" });
    }
    // every piece is content-addressed by its signed CID; the whole file's sha256 is checked once more
    const h = sha256.create();
    for (let i = 0; i < total; i++) h.update(new Uint8Array(Buffer.from(await FS.readAsStringAsync(dir + "p" + i, { encoding: FS.EncodingType.Base64 }), "base64")));
    if (hexOf(h.digest()) !== m.sha256) {
      await FS.deleteAsync(dir, { idempotent: true });
      throw new Error("the downloaded map isn't the one the organizer signed");
    }
    await FS.writeAsStringAsync(okFile, m.sha256).catch(() => {});
    crumb("map verified " + m.sha256.slice(0, 12));
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
