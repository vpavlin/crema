# ADR 0001: Crema runs on Logos only: Android + Basecamp, no server, no website

Status: Accepted · Date: 2026-11-08

## Context

Crema is a coffee-passport game: visit the good coffee places in a city, collect a stamp at each,
climb the leaderboard. Brno first. A web proof of concept (now `docs/spikes/web-poc`) proved the
stamp cryptography, but it ran on a website with a downloaded catalog and passports exported as
files: it used nothing of Logos. The point of Crema is to show that this kind of app works on Logos.

## Decision

1. **Two apps at parity, one wire contract** (charter rule 1):
   - **Android** (Expo/React Native, `loam-transport`, shared Loam node) for players, and café mode
     on a spare phone or tablet at the counter;
   - **Basecamp** module `crema_core` (C++ on `loam_core`) + `crema` view for organizers (studio,
     big-screen leaderboard) and café mode on a laptop.
   The JS protocol (`packages/protocol`) is the reference; the C++ core matches it byte for byte,
   checked by shared test vectors.
2. **Proof of presence = a rotating signed QR + the phone's GPS.**
   - The café's display signs (café id, 30 s step) with the café's Ed25519 key. Everyone verifies
     with the public key in the organizer-signed catalog; only the display can sign. It needs no
     network, only a correct clock.
   - The player's app accepts a code only while it is fresh (±1 step) and within ~75 m of the café
     (GPS, checked on the phone, never published). A claim made more than ~60 s after its step is
     invalid everywhere, so codes passed around don't count.
   - Rejected: NFC tags (NTAG 424 DNA SUN). They are symmetric: whoever verifies needs the key and
     can forge, so they need one trusted verifier. Receipt scans: easy to fake (maybe a low-value
     fallback later).
3. **Sync over Logos Delivery**, one topic per event derived from (organizer public key, event id).
   Plain relay plus the `loam-sync` event log with RBSR catch-up (as our other apps do since
   loam_core 0.7.3). Events:
   - `catalog`: the organizer-signed café list and event window, last version wins;
   - `stamp.claim`: signed by the player (secp256k1, the Loam/Keycard identity);
   - `player.profile`: the player's leaderboard name, signed, last write wins.
   The leaderboard is a fold over the claims, computed on every device; nobody is trusted to rank.
   The organizer's Basecamp keeps the history; a headless `logosctl` hub with the same core can
   keep it always on.
4. **Privacy.** Payloads are sealed with an event key that comes in the join QR, so only
   participants can read the event. A player's claims are published only if they join the
   leaderboard (asked at join); otherwise the passport stays on the phone. GPS positions never leave
   the phone. Later: a private leaderboard on LEZ (prove "N distinct cafés" without revealing which).
5. **Signing parity:** coordinates in the signed catalog are integer microdegrees (`latE6`,
   `lonE6`), so the canonical JSON has no floating-point formatting to disagree on.
6. **Message budget (RLN):** one message per stamp and per profile change; the catalog rarely
   changes; catch-up is RBSR, budgeted per peer.

## Consequences

- No website to host and nothing to trust but signatures; the event keeps working if the organizer
  goes offline (any participant serves the log).
- Café mode needs an Android device or a laptop with Basecamp at the counter until the e-ink
  device exists (ESP32 + e-paper, key in read-protected eFuses).
- Two implementations of the protocol must stay in step: shared vectors in CI.
