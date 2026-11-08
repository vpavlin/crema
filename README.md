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

## What's here (proof of concept)

| Part | What it is |
|---|---|
| `protocol/` | stamps, signed catalog, player claims, geofence, leaderboard (JS, `@noble/curves`); `npm test` |
| `web/display.html` | the café display: rotating QR, keeps the screen awake, works offline |
| `web/index.html` | the player (web POC): scan, GPS check, passport, map (OpenStreetMap), export passport |
| `tools/crema-admin.js` | organizer: create the event, add cafés (generates their keys), sign + publish the catalog, print display setup links, rank passports |

```sh
cd protocol && npm install && npm test
cd ../web && npm install && npm run build && cp node_modules/leaflet/dist/leaflet.css dist/
node tools/crema-admin.js init --event brno-coffee-test --name "Brno Coffee Passport (test)" --city Brno --start 2026-11-08 --days 21
node tools/crema-admin.js cafe --id kafec --name Kafec --lat 49.192982 --lon 16.61154 --address "Orlí 16"
node tools/crema-admin.js publish --base https://<where web/ is hosted>/
node tools/crema-admin.js board passport-*.json
```

The camera and GPS need HTTPS on phones, so host `web/` on HTTPS (e.g. GitHub Pages). Display
setup links carry a café's **secret** key: hand them over privately, never publish them. Secrets
live in `admin/` (git-ignored).

## Next

1. The Android app (Loam): claims sync over Logos Delivery, a live leaderboard, Keycard identity.
2. The Basecamp companion: organizer studio (cafés, keys, displays, events) and a big-screen leaderboard.
3. The e-ink display (ESP32 + e-paper, key in read-protected eFuses).
4. A private leaderboard on LEZ: prove "N distinct cafés" without revealing which.

Café data: OpenStreetMap (ODbL). We'd like to partner with European Coffee Trip for the curated list.

## Licence

MIT.
