#!/usr/bin/env bash
#
# Download the drivable road network inside a bounding box from OpenStreetMap
# (via the public Overpass API) and save it as Overpass JSON.
#
# Usage:
#   scripts/fetch_osm.sh                         # default: Tempe, AZ -> data/tempe.osm.json
#   scripts/fetch_osm.sh "<s>,<w>,<n>,<e>" <output.json>
#
# Example (downtown San Francisco):
#   scripts/fetch_osm.sh "37.765,-122.430,37.800,-122.385" data/sf.osm.json
#
# Keep the box to roughly 5 km x 5 km so the request stays fast and polite.
# Map data (c) OpenStreetMap contributors, licensed under the ODbL.

set -euo pipefail
source "$(dirname "$0")/lib/overpass.sh"

BBOX="${1:-33.390,-111.965,33.440,-111.895}"
OUT="${2:-data/tempe.osm.json}"

ROAD_CLASSES="motorway|motorway_link|trunk|trunk_link|primary|primary_link|secondary|secondary_link|tertiary|tertiary_link|unclassified|residential|living_street"

QUERY="[out:json][timeout:180];
way[\"highway\"~\"^(${ROAD_CLASSES})$\"](${BBOX});
(._;>;);
out body;"

echo "Fetching roads in ${BBOX}"
overpass_fetch "$QUERY" "$OUT"
