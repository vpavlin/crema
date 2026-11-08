#!/usr/bin/env bash
# make-map.sh - draw an event's own map (vector tiles, one .pmtiles file) from OpenStreetMap DATA.
#
# We never download OSM's map tiles (their tile policy forbids bulk downloads). Instead: ONE Overpass
# request for the raw data inside the event's box, then tilemaker renders vector tiles (OpenMapTiles
# schema) into a single .pmtiles file. The organizer imports it in the Basecamp studio, which puts it on
# Logos Storage; phones fetch it by CID through Loam. Data (c) OpenStreetMap contributors, ODbL.
#
# usage: tools/make-map.sh CAFES.json OUT.pmtiles [margin_deg]
#   CAFES.json: a seed file ({cafes:[{latE6,lonE6}]}) or a plain list; the box = the cafés + margin.
set -euo pipefail
IN=${1:?cafes json}; OUT=${2:?out.pmtiles}; M=${3:-0.006}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
BBOX=$(python3 - "$IN" "$M" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); m = float(sys.argv[2])
c = d["cafes"] if isinstance(d, dict) else d
la = [x["latE6"] / 1e6 for x in c]; lo = [x["lonE6"] / 1e6 for x in c]
print(f"{min(la)-m:.4f},{min(lo)-1.5*m:.4f},{max(la)+m:.4f},{max(lo)+1.5*m:.4f}")
PY
)
echo "box (s,w,n,e): $BBOX"
curl -sSf -A "crema-make-map/0.1 (github.com/vpavlin/crema)" --data-urlencode \
  "data=[out:xml][timeout:180];(way($BBOX);node($BBOX);rel($BBOX)[type=multipolygon];);(._;>;);out;" \
  https://overpass-api.de/api/interpreter -o "$W/area.osm"
echo "osm data: $(du -h "$W/area.osm" | cut -f1)"
OSMIUM=$(nix build --no-link --print-out-paths nixpkgs#osmium-tool)/bin/osmium
TM=$(nix build --no-link --print-out-paths nixpkgs#tilemaker)
"$OSMIUM" cat "$W/area.osm" -o "$W/area.osm.pbf" --overwrite
IFS=, read -r S Wd N E <<<"$BBOX"
# the OpenMapTiles profile, limited to our box and zoom 16 (MapLibre over-zooms beyond)
python3 - "$TM/share/tilemaker/config-openmaptiles.json" "$W/config.json" "$Wd,$S,$E,$N" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); w, s, e, n = map(float, sys.argv[3].split(","))
s_ = c["settings"]; s_["maxzoom"] = 16; s_["basezoom"] = 16; s_["minzoom"] = 0
s_["bounding_box"] = [w, s, e, n]; s_["name"] = "Crema event map"; s_["description"] = "(c) OpenStreetMap contributors, ODbL"
s_["compress"] = "gzip"
json.dump(c, open(sys.argv[2], "w"))
PY
(cd "$TM/share/tilemaker" && "$TM/bin/tilemaker" --input "$W/area.osm.pbf" --output "$(realpath -m "$OUT")" \
   --config "$W/config.json" --process process-openmaptiles.lua --skip-integrity --quiet)
echo "map: $OUT $(du -h "$OUT" | cut -f1)  sha256 $(sha256sum "$OUT" | cut -c1-64)"
