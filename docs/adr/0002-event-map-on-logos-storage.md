# 0002 - The event map comes from Logos Storage, never a tile server

Status: accepted, 2026-11-08 (vpavlin: "can we avoid hitting openstreetmaps ... fetch a few layers into
logos storage and serve it from the node of whoever is running the campaign?")

## Context

The 0.1 phone map loaded raster tiles from tile.openstreetmap.org and Leaflet from a CDN. That leaks every
player's position-of-interest to third parties, fails offline, and OSM's tile policy forbids the bulk
prefetch an event would need anyway.

## Decision

- **Our own vector tiles.** `tools/make-map.sh` makes ONE Overpass request for the raw OSM data in the
  event's box (cafés + margin) and renders it with tilemaker (OpenMapTiles schema) into a single
  `.pmtiles` file. Brno centre (4.3 x 3.1 km, zoom 0-16) is 1.2 MB. Data (c) OpenStreetMap contributors,
  ODbL; the credit is in the file and on the map.
- **Logos Storage carries it.** The organizer imports the file in the Basecamp studio; `crema_core`
  copies it (named by sha256), uploads it to the host's Storage node (`uploadUrl`, advertise on; the
  done event, else a manifest poll by file name), and reads `debug()` for a provider hint.
- **The signed catalog names it.** `catalog.map = {cid, sha256, size, bboxE6, maxzoom, providers}`;
  integers only, so JS and C++ sign identical bytes. A swapped CID breaks the signature. Old clients
  ignore the field. The map goes in on the next publish once Storage has the CID.
- **Phones fetch through Loam.** `loam-storage` (one Storage client per phone, background jobs) fetches
  the CID, dialling the providers first; Crema copies it out, checks the sha256 against the catalog, and
  keeps it. Without Loam Storage the map says so; it never falls back to a tile server.
- **Rendering is local.** MapLibre GL JS + the pmtiles reader are inlined into the map page (vendored at
  build time); the tiles are streamed into the page from the file. No glyphs: café names are HTML markers.

## Open

- **Reachability.** A NAT-ed organizer laptop announces no addresses, so phones elsewhere can't fetch from
  it. Needs a reachable Storage node that caches the event's CIDs (the Scala/Swamp hub pattern) and is
  listed in `providers`. To be decided.
- Street-name labels need glyph PBFs bundled with the app.
- The Basecamp view does not draw the map yet (big-screen leaderboard only).
