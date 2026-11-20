# Map data

| File | Contents | Script |
|---|---|---|
| `tempe.osm.json` | Drivable road network of Tempe, Arizona | [`scripts/fetch_osm.sh`](../scripts/fetch_osm.sh) |
| `tempe.places.json` | Named places for search (campuses, shops, restaurants, parks, stations) | [`scripts/fetch_places.sh`](../scripts/fetch_places.sh) |
| `tempe.walk.osm.json` | Pedestrian network: sidewalks, footways, crossings, paths, streets | [`scripts/fetch_walk.sh`](../scripts/fetch_walk.sh) |
| `tempe.shade.json` | Building footprints (with height / levels) and trees, for shadows | [`scripts/fetch_walk.sh`](../scripts/fetch_walk.sh) |
| `tempe.water.json` | Drinking fountains and bottle fillers (heat-relief stops) | [`scripts/fetch_water.sh`](../scripts/fetch_water.sh) |
| `tempe.campus.json` | University grounds (ASU Tempe), their named buildings with building codes, and entrances | [`scripts/fetch_campus.sh`](../scripts/fetch_campus.sh) |

All cover the bounding box `33.390,-111.965,33.440,-111.895` and were downloaded from the
OpenStreetMap Overpass API.

It is committed so the project runs straight after cloning, without depending on the public
Overpass servers, which are often overloaded.

## License

© OpenStreetMap contributors. The data is available under the
[Open Database License (ODbL) 1.0](https://opendatacommons.org/licenses/odbl/1-0/).
See <https://www.openstreetmap.org/copyright>.

## Refreshing or adding a city

```bash
scripts/fetch_all.sh   # re-download every Tempe extract
scripts/fetch_osm.sh    "<south>,<west>,<north>,<east>" data/<city>.osm.json
scripts/fetch_places.sh "<south>,<west>,<north>,<east>" data/<city>.places.json
```

Other `*.osm.json` / `*.places.json` / `*.shade.json` / `*.campus.json` / `*.water.json` files in this folder are git-ignored by default.
Files are minified JSON; together the Tempe extracts are about 21 MB.

Campus buildings come from OpenStreetMap, not from ASU's own campus map, which is a hosted service
without an open data license. OpenStreetMap maps only 15 drinking-water points in this area,
so water stops are sparse. Weather and traffic are not stored here: the server fetches it live from
[Open-Meteo](https://open-meteo.com/) (CC BY 4.0) and, with a key, HERE Traffic.
