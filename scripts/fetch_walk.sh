#!/usr/bin/env bash
#
# Download the data for shade-aware walking routes:
#   1. the pedestrian network (sidewalks, footways, paths, crossings, streets)
#   2. shade casters: building footprints (with height / levels) and trees
#
# Usage:
#   scripts/fetch_walk.sh                               # default: Tempe, AZ
#   scripts/fetch_walk.sh "<s>,<w>,<n>,<e>" <walk.json> <shade.json>
#
# Map data (c) OpenStreetMap contributors, licensed under the ODbL.

set -euo pipefail
source "$(dirname "$0")/lib/overpass.sh"

BBOX="${1:-33.390,-111.965,33.440,-111.895}"
WALK_OUT="${2:-data/tempe.walk.osm.json}"
SHADE_OUT="${3:-data/tempe.shade.json}"

WALKABLE="primary|primary_link|secondary|secondary_link|tertiary|tertiary_link|unclassified|residential|living_street|service|pedestrian|footway|path|steps|cycleway|track"

WALK_QUERY="[out:json][timeout:240];
way[\"highway\"~\"^(${WALKABLE})$\"][\"service\"!~\"^(driveway|drive-through)$\"](${BBOX});
(._;>;);
out body;"

# `out geom` inlines coordinates, so no separate node lookup is needed.
SHADE_QUERY="[out:json][timeout:240];
(
  way[\"building\"](${BBOX});
  relation[\"building\"][\"type\"=\"multipolygon\"](${BBOX});
  node[\"natural\"=\"tree\"](${BBOX});
  way[\"natural\"=\"tree_row\"](${BBOX});
);
out geom;"

echo "Fetching walking network in ${BBOX}"
overpass_fetch "$WALK_QUERY" "$WALK_OUT"

echo "Fetching buildings and trees in ${BBOX}"
overpass_fetch "$SHADE_QUERY" "$SHADE_OUT"
