#!/usr/bin/env bash
# Re-download every OpenStreetMap extract for one area (default: Tempe, AZ).
#
# Usage: scripts/fetch_all.sh [bbox] [name]
#   bbox  "south,west,north,east"   name  file prefix in data/ (default: tempe)
#
# Extract: Overpass API queries (scripts/fetch_*.sh, retried across mirrors).
# Transform + load happen in the C++ loaders at server start-up (src/io/osm_loader.cpp).
set -euo pipefail
cd "$(dirname "$0")/.."

BBOX="${1:-33.390,-111.965,33.440,-111.895}"
NAME="${2:-tempe}"

scripts/fetch_osm.sh    "$BBOX" "data/${NAME}.osm.json"
scripts/fetch_places.sh "$BBOX" "data/${NAME}.places.json"
scripts/fetch_walk.sh   "$BBOX" "data/${NAME}.walk.osm.json" "data/${NAME}.shade.json"
scripts/fetch_campus.sh "$BBOX" "data/${NAME}.campus.json"
scripts/fetch_water.sh  "$BBOX" "data/${NAME}.water.json"
echo "Done. Validate with: ctest --preset release && ./build/release/umbra_server"
