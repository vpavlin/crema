# Crema - a coffee passport you can't fake from your sofa

Visit the good coffee places in a city, collect a stamp at each, climb the leaderboard. First city:
Brno. Built on Logos: no server decides who was where.

## How a stamp works

- Each café has a small display (for now: any old phone or tablet running `web/display.html`; later
  an e-ink device) that shows a QR code changing every **30 seconds**. The code is an **Ed25519
  signature by the café's own key** over (café, time step). Only that display can make it.
- The event's café list is **signed by the organizer**: names, places and each café's public key.
  Every player's app can check any stamp itself, offline, without asking a server.
- The app accepts a code only while it's fresh and only **within ~75 m of the café** (GPS, checked
  on the phone and never published). So a photo sent to a friend doesn't work.
- A stamp becomes a **claim signed with the player's own key** (secp256k1, the Loam/Keycard
  identity). The leaderboard counts **distinct cafés**, one stamp per café per day; anyone can
  recompute it from the claims.

Why not NFC tags: cheap tags (NTAG 424 DNA) use a symmetric key, so whoever verifies a tap needs
the same key and could forge taps - you'd need one trusted verifier. Public-key signatures let
everyone verify and only the café sign.

## How it's built (ADR 0001)

No server, no website. Two apps at parity on one wire contract, both on Loam:

| Part | Status |
|---|---|
| `packages/protocol` - stamps, signed catalog, player claims, geofence, leaderboard (JS reference, `npm test`) | done |
| `packages/engine` - the event log (loam-sync) and the fold: catalog, claims, profiles → passport + leaderboard | next |
| `mobile/` - Android (Expo, loam-transport): join an event by QR, map, scan + GPS check, passport, live leaderboard, café mode | planned |
| `crema_core` + `module/` - Basecamp (C++ on loam_core + QML): organizer studio, café mode, big-screen leaderboard | planned |
| e-ink café display (ESP32 + e-paper) | later |
| private leaderboard on LEZ | later |

Sync: one Logos Delivery topic per event, sealed with the event key from the join QR; plain relay
plus loam-sync RBSR catch-up. The leaderboard is computed on every device from signed claims.

`docs/spikes/web-poc` is the first proof of concept (a website); it proved the stamp crypto and is
kept as a record, not the product.

Café data: OpenStreetMap (ODbL). We'd like to partner with European Coffee Trip for the curated list.

## Licence

MIT.
