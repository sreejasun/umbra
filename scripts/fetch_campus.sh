#!/usr/bin/env bash
# Download campus buildings (name, building code, entrances) from OpenStreetMap.
#
# Usage: scripts/fetch_campus.sh [bbox] [output]
#   bbox    "south,west,north,east" (default: Tempe, AZ, which includes ASU's Tempe campus)
#   output  default data/tempe.campus.json
#
# Fetches every university/college campus outline in the box, every named
# building inside one, and the entrance nodes on those buildings' outlines, so
# that a walk to "Hayden Library" ends at a door instead of the roof's centre.
# Buildings without a mapped entrance fall back to points on their outline.
set -euo pipefail
source "$(dirname "$0")/lib/overpass.sh"

BBOX="${1:-33.390,-111.965,33.440,-111.895}"
OUT="${2:-data/tempe.campus.json}"

QUERY="[out:json][timeout:180];
(
  way[\"amenity\"~\"^(university|college)$\"](${BBOX});
  relation[\"amenity\"~\"^(university|college)$\"][\"type\"=\"multipolygon\"](${BBOX});
)->.campus;
.campus out body geom;
.campus map_to_area->.grounds;
(
  way[\"building\"][\"name\"](area.grounds);
  relation[\"building\"][\"name\"][\"type\"=\"multipolygon\"](area.grounds);
)->.buildings;
.buildings out body geom;
node(w.buildings)[\"entrance\"];
out body;"

echo "Fetching campus buildings in ${BBOX}"
overpass_fetch "$QUERY" "$OUT"
