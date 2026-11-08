#!/usr/bin/env bash
# JS writes the vectors, C++ checks them (and writes a frame the JS side then opens).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
node "$ROOT/packages/engine/test/make-vectors.js"
NJ="${NLOHMANN_INCLUDE:-/usr/include}"
g++ -std=c++20 -O1 -Wno-deprecated-declarations -I"$NJ" -o /tmp/crema-parity "$ROOT/crema_core/test/parity.cpp" -lcrypto
cd "$ROOT/crema_core/test" && /tmp/crema-parity vectors.json
