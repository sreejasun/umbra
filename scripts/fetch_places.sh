#!/usr/bin/env bash
#
# Download named places (campuses, shops, restaurants, parks, stations, ...)
# inside a bounding box from OpenStreetMap. Used by the search box.
#
# Usage:
#   scripts/fetch_places.sh                      # default: Tempe, AZ -> data/tempe.places.json
#   scripts/fetch_places.sh "<s>,<w>,<n>,<e>" <output.json>
#
# Use the same bounding box as scripts/fetch_osm.sh.
# Map data (c) OpenStreetMap contributors, licensed under the ODbL.

set -euo pipefail
source "$(dirname "$0")/lib/overpass.sh"

BBOX="${1:-33.390,-111.965,33.440,-111.895}"
OUT="${2:-data/tempe.places.json}"

# `out center` gives one coordinate per feature, including buildings and areas.
QUERY="[out:json][timeout:180];
(
  nwr[\"name\"][\"amenity\"](${BBOX});
  nwr[\"name\"][\"shop\"](${BBOX});
  nwr[\"name\"][\"tourism\"](${BBOX});
  nwr[\"name\"][\"leisure\"~\"^(park|stadium|sports_centre|golf_course|water_park|nature_reserve)$\"](${BBOX});
  nwr[\"name\"][\"railway\"~\"^(station|halt|tram_stop)$\"](${BBOX});
  nwr[\"name\"][\"building\"~\"^(university|college|stadium|hospital)$\"](${BBOX});
);
out center tags;"

echo "Fetching places in ${BBOX}"
overpass_fetch "$QUERY" "$OUT"
