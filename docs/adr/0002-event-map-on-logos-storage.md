# 0002 - The event map comes from Logos Storage, never a tile server

Status: accepted, 2026-11-08; amended for 0.3.0 the same day (vpavlin: "can we avoid hitting openstreetmaps ... fetch a few layers into
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

## Amendment 0.3.0 (2026-11-08): made in the core, fetched in pieces

**Make map in Basecamp.** `crema_core.makeMap(eventId, marginM)` does what `tools/make-map.sh` did, inside
the module (no external programs; tilemaker can't ship in an .lgx): one Overpass request (JSON with
geometry, only the features the style draws, ~22 MB for Brno; public mirrors tried in turn when one is
busy) -> `maptiles.hpp` (Web Mercator, per-tile clipping, Douglas-Peucker by zoom, MVT 2.1, gzip, PMTiles
v3, clustered). Same layers/classes as tilemaker's profile. Brno: 140-160 tiles, 1.3-1.5 MB, under a second.

**Why pieces.** Measured between two NAT-ed nodes over a logos.test circuit relay: Storage moved 0 bytes.
Two causes, both on the downloading side:
1. the swarm-admission bug (fixed upstream by PR #1540; storage_module 3.0.2 still predates it; Loam's own
   libstorage has the fix);
2. libstorage asks for blocks in 128 KiB batches (`TargetBatchBytes`), and nim-libp2p's relay allows
   128 KiB per circuit (`DefaultLimitData`): every multi-block batch overflows the circuit, which is
   closed, and the same batch is retried forever. One-block CIDs fit: 21 x 60 KiB pieces of a 1.2 MB
   file all arrived over the relay (16-90 s each), byte-identical. An upstream issue is drafted.

**Pieces.** Besides the whole file (0.2 phones), the core uploads the map as 60 KiB pieces, each its own
CID, and signs into the catalog `map.pieces` (CIDs in file order), `pieceSize`, `dirEnd`, `zoomEnds`
([zoom, byte offset where that zoom is complete]) and `minzoom`. A clustered PMTiles file stores
tiles by zoom, so any prefix holds the directory plus the coarse zooms: the phone fetches pieces in order
through Loam, streams each into the map page, and raises the source's maxzoom as each zoom completes
(MapLibre over-zooms the coarser tiles meanwhile). Brno: 1 piece = zoom 12 + 13 (a street-level
overview), 7 = buildings (z14), 21 = full detail. CIDs are content-addressed and signed, and the whole
file's sha256 is checked once all pieces are in. Imported unclustered files (tilemaker) still work, they
only appear at the end.

**Still open:** the reachable hub (cache-on-see, as Scala's) is the fast path; deferred by vpavlin.
