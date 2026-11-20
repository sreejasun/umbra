#!/usr/bin/env bash
# Download drinking-water points (fountains, bottle fillers, taps) from OpenStreetMap,
# used as heat-relief stops for "walk via water".
#
# Usage: scripts/fetch_water.sh [bbox] [output]
#   bbox    "south,west,north,east" (default: Tempe, AZ)
#   output  default data/tempe.water.json
set -euo pipefail
source "$(dirname "$0")/lib/overpass.sh"

BBOX="${1:-33.390,-111.965,33.440,-111.895}"
OUT="${2:-data/tempe.water.json}"

QUERY="[out:json][timeout:120];
(
  nwr[\"amenity\"=\"drinking_water\"](${BBOX});
  nwr[\"amenity\"=\"water_point\"][\"drinking_water\"!=\"no\"](${BBOX});
  nwr[\"man_made\"=\"water_tap\"][\"drinking_water\"=\"yes\"](${BBOX});
);
out center tags;"

echo "Fetching drinking water in ${BBOX}"
overpass_fetch "$QUERY" "$OUT"
