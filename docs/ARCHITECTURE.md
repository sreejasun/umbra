# Architecture

This document explains how a click on the map becomes a route, and why each component is built the
way it is.

## Layers

```
web/ (Leaflet PWA: map, search, navigation, account, assistant)
   │ HTTP/JSON, session cookie
   ▼
src/server/  http_api ── security (rate limits, CSP, CSRF)      config (flags, env, .env)
   │            │
   │            ├── trip_queries ◀── assistant (Grok tool calls) ── http_client (libcurl)
   │            │      shared by the API and the assistant's tools
   │            └── UserStore (src/storage: SQLite + libsodium)
   ▼
core library (include/umbra, src/core, src/io, src/service): no network I/O
   RouteService (drive)          WalkPlanner                  WeatherService / TrafficService
   snap · search · directions    sun, light, stairs, stops     CachedResource<T> + injected HttpGet
        │                              │                        Open-Meteo forecast, HERE flow
   SpatialIndex  SearchIndex   find_shortest_path(_between)    ShadeMap ◀── buildings, trees
        └────────── RoadGraph (CSR) ◀── osm_loader (roads, places, campus, water: Overpass JSON)
```

The core library has no dependency on the HTTP layer and does no network I/O, so it is unit-tested
directly and could back a CLI, gRPC service or Python binding unchanged. Live data comes in through
injected functions (`HttpGet`, `HttpPost` in `http_types.hpp`); only `src/server/http_client.cpp`
talks to the network. `nlohmann/json` is a *private* dependency of the core library, so it never
leaks into public headers. The server code is a library (`umbra_server_lib`) plus a small `main.cpp`,
so its security, configuration and assistant logic are unit-tested too.

## Graph model

* **Nodes** are OSM nodes that lie on drivable ways (intersections *and* shape points), so the route
  geometry follows real road curvature.
* **Edges** are directed. A two-way road becomes two edges; `oneway=yes`, `oneway=-1`,
  `junction=roundabout` and motorways become a single edge in the legal direction. `oneway=reversible`
  roads are skipped because their direction depends on the time of day.
* Each edge stores `length_m`, `duration_s` and a street-name id (names are interned once).

### Compressed Sparse Row

```
offsets: [0, 2, 3, 5, ...]          size V + 1
edges:   [e0 e1 | e2 | e3 e4 | ...]  size E, grouped by source node
out_edges(v) = edges[offsets[v] .. offsets[v+1])
```

Built with a counting sort in O(V + E). Compared with `vector<vector<Edge>>` it needs a single
allocation and iterating a node's neighbours touches one contiguous block of memory.

### Largest strongly connected component

Map extracts are clipped by a bounding box, which creates dead ends and one-way fragments. If a user
snapped to one of those, there might be no route. `largest_strongly_connected_component()` runs
Kosaraju's algorithm (iterative DFS on G for finish order, then DFS on the reverse graph) and keeps only
the largest component. On the Tempe extract this keeps 13,025 of 13,696 nodes.

## Route search

`find_shortest_path()` implements Dijkstra and A\* with a single loop:

```
best[s] = 0; heap.push(s, h(s))
while heap not empty:
    u = heap.pop()              # smallest best[u] + h(u)
    settled[u] = true
    if u == target: break
    for edge (u → v, w):
        if not settled[v] and best[u] + w < best[v]:
            best[v] = best[u] + w
            parent[v] = edge
            heap.push_or_decrease(v, best[v] + h(v))
```

* **Dijkstra**: `h(v) = 0`.
* **A\***: `h(v) = haversine(v, target)` for distance, or `haversine(v, target) / max_speed` for time.

The heuristic is admissible (edge lengths are sums of great-circle segments, and no edge is faster than
`max_speed`) and consistent (it satisfies the triangle inequality), so a settled node is final and A\*
returns the same optimal cost as Dijkstra. The benchmark tool and a randomized unit test check this.

### Sets of start and end points

`find_shortest_path_between(graph, sources, targets)` is the same loop generalised: every source starts
in the heap at cost 0 (a virtual super-source joined to each one), and the search stops at the first
*target* it settles, which is the cheapest source–target pair. For A\*, `h(v)` is the distance to the
**nearest** target; the minimum of consistent heuristics is consistent, so optimality is kept. One search
replaces the |S| × |T| point-to-point queries of trying every pair, and the point-to-point API is now a
wrapper with one source and one target. A randomized test compares it with the best of all pairs for
both algorithms and both metrics.

This is how routes to buildings work: the UI sends a building's entrances as `to=lat,lng;lat,lng;…`,
`RouteService::snap_all()` snaps each one (skipping any too far from the network), and the route ends
at whichever door is quickest to reach. A building chosen as the start works the same way.

### Alternative routes

`find_alternative_paths()` returns up to three meaningfully different routes with the *penalty
method*: search, multiply the cost of every edge on the route found by 1.4, and search again. A
candidate is kept if its true cost (without penalties) is within 1.35× the best and it shares at most
75 % of its length with the routes already kept; at most six extra searches run. Penalties only raise
costs, so A\* stays admissible. A randomized test checks the first route is optimal and the others
respect both limits. The drive API labels each alternative by what is special about it (shorter,
less sun glare, fewer turns) or by its longest road the main route does not use ("via South Smith
Road"). Walks get their choices differently: one per shade preference, if the paths differ.

### Indexed min-heap

```
heap_: [(priority, id), ...]   binary heap array
pos_:  id → index in heap_      updated on every move
```

`decrease_key(id)` looks up `pos_[id]`, lowers the priority, and sifts up: O(log n). Ties break by id,
which keeps results deterministic across platforms.

## Snapping

`SpatialIndex` projects coordinates to a local equirectangular plane (metres), buckets nodes into
100 m cells stored in CSR form, and searches rings of cells around the query. After ring *r*, every
unvisited cell is at least *r × cell* metres away, so the search stops as soon as the best match is
closer than that. This also holds for queries outside the grid, because those are clamped to the
border cell and only get farther from the remaining cells.

## Directions

1. Merge consecutive edges with the same street name into *legs*.
2. Fold any leg shorter than 35 m that sits between two legs of the same street back into that street.
   This removes artefacts such as "turn onto Smith Rd for 14 m, turn back onto Rio Salado".
3. Classify the bearing change between the last edge of one leg and the first edge of the next:

| abs(Δ bearing) | Manoeuvre |
|---|---|
| < 20° | continue |
| 20–60° | bear left/right |
| 60–135° | turn left/right |
| 135–170° | sharp left/right |
| ≥ 170° | U-turn |

## Text search

`SearchIndex` powers the Start and Destination boxes. It holds four kinds of entries:

| Kind | Source | Example |
|---|---|---|
| Place | `scripts/fetch_places.sh` (amenities, shops, parks, stations, campuses) | Tempe Marketplace |
| Building | `scripts/fetch_campus.sh` (named buildings inside university grounds) | Hayden Library · LIB |
| Street | every street name in the graph, anchored at its node closest to the street's centroid | South Mill Avenue |
| Intersection | every distinct pair of street names meeting at a node | South Mill Avenue & West University Drive |

**Normalisation.** Text is lower-cased, apostrophes and periods are dropped ("McDonald's" →
`mcdonalds`), common abbreviations are expanded (`st`→`street`, `ave`→`avenue`, `n`→`north`), and
connector words (`and`, `at`, `&`) are removed. That makes "Mill & University", "mill and univ" and
"University Dr at Mill Ave" equivalent.

**Index.** The distinct tokens form a sorted vocabulary, and each vocabulary word has a sorted posting
list of entry ids. OSM `short_name`/`alt_name` values are indexed as hidden keywords, which is why "ASU"
finds Arizona State University.

**Query.**
1. Every query word finds the vocabulary range sharing its prefix, with `lower_bound` and a linear scan
   while the prefix still matches. "univ" covers `university` and `universal`.
2. If a word of 4+ characters matches nothing, it falls back to vocabulary words (or word prefixes) within
   Levenshtein distance 1, so "univrsity" still works.
3. Candidate sets are intersected, starting from the smallest (AND semantics).
4. Score = Σ match quality (exact 3, prefix 2, typo 1) + 1.5 if the first word matches the name's leading
   word + importance − 0.25 × unrequested words in the name.

**Importance.** Universities and malls rank above cafés, features with a Wikidata entry get +1, and
features mapped as areas rather than points get +0.5. That ranks ASU's campus above a single-building
college for "univ".

Places farther than the snap radius from any road are dropped, and each place's subtitle names the
nearest intersection ("Cafe · near South Rural Road") so that identically named chains are easy to
tell apart.

**Building codes.** A query that is exactly a building code (case and spaces ignored) is looked up in a
hash map first, so *"MU"* lists the Memorial Union ahead of prefix matches such as *Music Building*.
Codes shared by two buildings, and one-letter codes, are left to the ranked search.

## Campus buildings

`parse_campus_buildings()` reads the output of `scripts/fetch_campus.sh`: university/college grounds
(`amenity=university|college` ways and multipolygons), the named buildings inside them, and the
`entrance=*` nodes on those buildings. For each building:

1. The footprint's centroid decides which campus it belongs to (even-odd point-in-polygon against the
   campus's outer rings). Buildings outside every campus, such as a shop across the street, are dropped.
2. `ref` becomes the building code (*COOR*); `alt_name`, `short_name`, `old_name` and the campus's short
   name (*ASU*) become hidden keywords, so *"asu library"* works.
3. **Access points** are the outline vertices that are entrance nodes (`yes`, `main` or `secondary`,
   not `service`, `emergency` or private). Overpass prints a node's coordinates identically everywhere,
   so entrances are matched by exact coordinates, which works for multipolygon outlines too, whose member
   node ids are not in the output. A building without a mapped entrance gets 12 points spread evenly
   along its outline, so the route at least reaches the near side of the building.

`merge_places()` then drops entries from the general places file with the same name within 200 m, so a
building appears once, with its entrances. For Tempe this yields 194 ASU buildings: 34 have codes and
16 have mapped entrances.

## Shade-aware walking

### Pedestrian network

`load_overpass_file(..., {TravelMode::kWalk})` builds a second graph from sidewalks, footways, paths,
crossings, steps, cycleways (where walking is allowed) and ordinary streets. Freeways (`motorway`,
`trunk`) and ways tagged `foot=no` or `access=private` without a `foot=yes` override are excluded.
One-way restrictions don't apply to pedestrians. Speed is 5 km/h, or 2.5 km/h on stairs. Ways tagged
`covered=yes`, `tunnel=building_passage` or `indoor=yes` get the `kEdgeCovered` flag and are always in
shade. Steps and `wheelchair=no` ways get `kEdgeStairs`; `lit=yes` (or any other non-`no` value) gets
`kEdgeLit` and `lit=no` gets `kEdgeUnlit`. For Tempe this gives 62,936 nodes; 94 ways are stairs and
lighting is tagged on 1,613 of 14,555 ways (11 %).

### Sun position

`sun_position()` implements the NOAA Solar Calculator. It takes the Julian century, works out the sun's
geometric mean longitude and anomaly, the equation of centre, the apparent longitude, the obliquity of
the ecliptic and the declination, then the equation of time and the local hour angle, and finally the
zenith and azimuth, with a correction for atmospheric refraction. Tests check the solstice-noon
elevations (80° in June, 33° in December) and that the sun sits east in the morning and west in the
afternoon.

### Shadow test

Buildings are vertical prisms of height *h*. With **d** the horizontal unit vector toward the sun and
*e* its elevation, a ground point **P** is shaded iff the segment

    P → P + d · h / tan(e)

intersects the footprint. Along that segment the sun ray climbs from 0 to *h*, so any intersection
happens below the roof. The test is exact for non-convex footprints too. Trees are crowns of radius *r*
at about 0.65 × tree height. **P** is shaded if **P** + **d** · (crown height / tan e) lies within *r* of
a trunk.

**Candidate pruning.** Buildings are registered in every 40 m grid cell their bounding box overlaps. A
query walks only the cells the shadow segment crosses (Amanatides–Woo DDA), checks bounding boxes, and
only then runs the exact segment-polygon test. Shadows are capped at 250 m, and below 2° of elevation
the whole city counts as shaded.

**Missing heights.** Fewer than 3% of OSM buildings in Tempe have a `height` or `building:levels` tag. The estimate uses, in
order: `building:levels` × 3.3 m + 1 m; type defaults (carport or roof 3.5 m, house 4.5 m, apartments
10 m, university 12 m); otherwise footprint area (< 150 m² → 4 m, < 2,000 m² → 6 m, larger → 9 m).

### Route cost

    cost(edge) = length × (1 + penalty × sun_exposure(edge, t))

where `sun_exposure` is the fraction of 5 m samples in direct sun, and the penalty is 0, 1.5 or 5
(Shortest, Balanced, Most shade). Exposure is memoised per request and computed lazily inside the
search's cost callback, so only edges that Dijkstra/A* actually relax pay for shadow tests (about
15–20 ms per walk in Tempe). Since cost ≥ length, `haversine × 1.0` remains an admissible A* heuristic.
The response also returns the plain shortest walk and splits both routes into shaded and sunny runs
for drawing.

`WalkPlanner::make_costs()` builds both cost functions for a request:

| Option | Preferred route | Comparison route |
|---|---|---|
| Daytime | `length × (1 + penalty × sun_strength × exposure)` | `length` |
| After dark, `prefer_lit` | `length × (1 lit, 1.25 unknown, 3 unlit)` | `length` |
| `avoid_stairs` | stairs cost ∞ (never relaxed) | stairs cost ∞ too: step-free is a hard constraint |

When both functions are the same (Shortest preference by day, or no lighting preference at night),
only one search runs.

### Water stops

`plan_via_stop()` routes through the drinking fountain that adds the least cost. A fountain more than
60 m from every walkway is ignored. Rather than routing through each of the *k* fountains (2*k*
searches), it runs two multi-source Dijkstra searches with the walk's own cost function, one from the
start points and one from the end points (the pedestrian graph is symmetric), and picks
argmin over *s* of d(start, *s*) + d(*s*, end). Two more searches build the legs, which are joined with
a `waypoint` step ("Stop for water at Bottle filler").

### Best time to leave

`compare_departures()` plans the same walk at up to 16 times, with the live sun strength of each
time. `/api/walk/departures` adds the forecast feels-like temperature and comfort of each time, and
the UI marks the best option (most comfortable, then shadiest, then earliest).

### Live weather

`WeatherService` (core library) owns one forecast for the map's centre, cached by `CachedResource`
(below):

* **Fetch.** `open_meteo_url()` asks for hourly temperature, "feels like" temperature, cloud cover,
  precipitation, WMO weather code, wind, UV index and direct normal irradiance (DNI), from yesterday to 7
  days ahead, with Unix timestamps. `parse_open_meteo()` validates it; hours without a temperature are
  treated as missing, not as 0 °C.
* **Cache.** `CachedResource<T>`, shared with traffic: refetch only when the value is older than the
  refresh interval (10 min for weather); after a failure, wait before trying again (60 s, no retry
  storm against a struggling upstream) and keep serving the previous value until it is too old (3 h).
  In the server the cache runs in **background mode**: `get()` never waits; when a refresh is due it
  starts one fetch on a worker thread and returns the current value (or none yet), so a slow or
  unreachable provider cannot delay a route. Without it, the first request after start-up waited up
  to the HTTP timeout while Open-Meteo was slow. The first fetch starts when the server starts.
  Requests share a `shared_ptr` to an immutable value. The fetch function and the clock are injected,
  so the tests use a fake upstream and a fake clock (and, for background mode, a fetch that blocks on
  a promise).
* **Lookup.** `WeatherForecast::at(t)` binary-searches the hourly samples and interpolates linearly.
  Radiation and precipitation are averages over the *preceding* hour, so they are interpolated as if
  stamped 30 minutes earlier. The weather code comes from the nearer hour. More than an hour outside the
  forecast returns nothing, and the planner assumes a clear sky.

**Sun strength.** `direct_sun_strength() = clamp(DNI_forecast / DNI_clear, 0, 1)`, where the clear-sky
beam comes from the Meinel model, `1353 · 0.7^(AM^0.678)` W/m², with air mass `AM = 1 / sin(elevation)`.
The walk cost becomes

    cost(edge) = length × (1 + penalty × sun_strength × sun_exposure(edge, t))

so clouds shrink the detour worth taking for shade, and with no direct beam (overcast, no shadows) the
planner returns the shortest walk. Since cost ≥ length still holds, A\* stays admissible.

**Walk comfort.** `assess_walk_comfort()` applies, in order: thunderstorms → *dangerous*; feels-like
≥ 103 °F → *dangerous*; ≥ 90 °F → *caution*; snow, rain (≥ 0.5 mm/h or a rain code), cold (< 40 °F),
wind ≥ 40 km/h → *caution*; ≥ 80 °F, chilly (< 50 °F), drizzle, fog, or UV ≥ 8 while the sun is up →
*fair*; otherwise *pleasant*. The heat thresholds are the US National Weather Service heat-index bands.

Privacy: only the centre of the map is sent to Open-Meteo, never a user's position.

### Location

The browser's Geolocation API (`watchPosition`) drives a blue dot with an accuracy circle. It starts as
soon as the app opens: the browser asks for permission the first time, and if the user has said no the
app stays quiet (the locate button still explains how to allow it). The map centres on the first fix
unless a shared route is on screen. Phones pause location for background apps, so tracking resumes
when the page becomes visible again. Browsers allow location only on secure origins: HTTPS, or
`localhost` during development.

## Driving conditions

`/api/route` minimises `conditioned_duration_s()` = free-flow time ÷ (traffic factor × weather factor).
Both factors are ≤ 1, so conditioned times are never shorter than free-flow times and the A\* time
heuristic (straight-line distance ÷ top speed) stays admissible.

**Traffic.** `TrafficService` fetches HERE Traffic v7 flow for the map's bounding box
(`locationReferencing=shape`): one request returns every reported stretch with its polyline, current
speed and free-flow speed. `match_traffic()` buckets the stretches' segments in a uniform grid
(cells ≥ 40 m), and gives each road edge the speed ratio of the nearest segment within 15 m of its
midpoint whose bearing is within 30° (so the two carriageways of a divided road get their own speeds),
clamped to [0.05, 1]. The match runs once per refresh (every 15 min, about 2,900 requests a month for
the whole map), not per request. Without a key, or if the provider fails, routes use free flow.

**Weather.** `drive_weather_slowdown()` applies the FHWA Road Weather Management Program's published
free-flow speed reductions (midpoints): freeways −7.5 % in light rain (2–13 %), −11.5 % in heavy rain
(6–17 %), −34.5 % in snow (5–64 %); arterials −17.5 % on wet pavement (10–25 %), −35 % in snow
(30–40 %); −11 % in low visibility (10–12 %). Roads with a free-flow speed of at least 22 m/s (about
50 mph) count as freeways. Heavy rain means more than 7.6 mm/h (American Meteorological Society).

**Glare.** `sun_glare(heading, sun, max_elevation)` is true when the sun is up, at most
`max_elevation` high (20°; 35° for drivers whose eyes sit below the lowered visor) and within 25° of
the heading. `edge_into_sun()` applies it to an edge's bearing. With glare avoidance on,
`drive_cost_s()` multiplies the time of such edges by 3, so the search accepts up to two extra
minutes for every minute out of the sun; the cost is still ≥ the free-flow time, so A\* stays
admissible. The response reports the seconds driven into the sun and, for comparison, those of the
plain fastest route (one extra search). Directions legs are flagged with the straight-line heading
of each leg. Glare uses the departure time, so a drive can be planned for later; live traffic only
applies within 15 minutes of now because there are no historical speeds.

## Accounts

`UserStore` (`src/storage`) keeps four tables in SQLite: `users`, `sessions`, `places`, `trips`, with
foreign keys and `ON DELETE CASCADE`, WAL journaling and a schema version in `PRAGMA user_version`.

* **Passwords:** `crypto_pwhash_str` (Argon2id, libsodium's interactive limits: 64 MiB, 2 passes),
  10-128 characters. The slow hash runs outside the store's mutex.
* **Sessions:** 32 random bytes from `randombytes_buf`, given to the browser as an `HttpOnly`,
  `SameSite=Strict` cookie (`Secure` over HTTPS) and stored only as a BLAKE2b hash, valid for 30 days.
* **No account enumeration:** a login with an unknown email verifies against a dummy hash, so it takes
  as long as a wrong password and returns the same message.
* **Limits:** 100 places per user, the 50 most recent trips, 80-character names, every value validated
  and every query parameterised.
* **Data rights:** `/api/me/export` returns everything stored about the user; `DELETE /api/me`
  removes it.

## Trip assistant

`TripAssistant` implements a tool-calling loop against xAI's Responses API (`POST /v1/responses`):

1. Send the instructions, a context line (current time, timezone, the user's position if shared) and
   the question, with the JSON Schemas of five tools.
2. For every `function_call` in the reply, run the tool on Umbra's engine (`trip_queries`, the same
   code as the HTTP API) and send `function_call_output` items back with `previous_response_id`.
3. Stop at a text answer, or after 6 model calls.

The model only ever sees tool results: route summaries, weather and search hits, so its answers rest
on real computations. Tool errors are returned to the model (for example, "No place matches …") so it
can recover by searching first. The last planned route is passed to the UI and drawn on the map. Text
is rendered with `textContent`. The API key stays on the server, questions are limited to 500
characters, and each account may ask 20 questions an hour.

## Web app

`web/` is plain ES modules with no build step, laid out like a maps app:

* **Screens** are hash routes (`#/`, `#/place`, `#/directions`, `#/directions/steps`, `#/options`,
  `#/departures`, `#/navigate`, `#/settings`, `#/account`, `#/safety`, `#/assistant`, `#/about`). A
  table in `app.js` gives each its header (search bar, From / To card or navigation banner), its sheet
  page and height, a guard (e.g. Steps needs a route) and enter / leave hooks (Navigation starts and
  stops the navigator). Back and forward work because every screen is a URL; the trip itself stays in
  the query string, so a link to `#/directions` reproduces the route.
* **The sheet** (`sheet.js`) is a bottom sheet on phones with three heights (peek, half, full), dragged
  by its handle with pointer events; on desktop the same element is a floating left panel. It reports
  how much of the map it covers, so routes are framed in the visible part and map controls move above it.
* **Route choices** come from the API's `alternatives`; `map-view.js` draws the others in grey
  (clickable) and the selected one on top, by shade and sun for walks.
* **Settings** hold the account, emergency settings, the theme (System follows the device; Light and
  Dark set `data-theme` on `<html>`, which the CSS tokens key off) and Developer mode, which reveals
  the algorithm switch, the search animation and the engine statistics. Device preferences live in
  `localStorage`.
* **Help**: the Ask Umbra button opens quick links to common settings and, with an xAI key, the
  assistant, whose instructions include a short guide to where everything is in the app.

## Navigation

`web/js/navigation.js` follows the user's location fixes (ignoring fixes less accurate than 50 m). It
speaks the next instruction 150 m ahead and again at 30 m using the browser's Web Speech API,
advances past steps within 15 m, announces arrival within 20 m, and after two consecutive fixes more
than 40 m from the route asks the server for a new route from the current position (at most every
15 s).

## Safety (emergency)

`web/js/safety.js` runs entirely in the browser; nothing is sent to the Umbra server.

1. **Opt in.** The danger phrase (at least two words, so it is not said by accident), the contact and
   the *Call 911* choice are stored in `localStorage` on the device.
2. **Listen.** The Web Speech API (`SpeechRecognition`) runs continuously with interim results; the
   transcript and the phrase are normalised (lower case, letters and digits only) and matched as a
   substring. Recognition stops after silence or when the page is hidden, so it restarts on `end` and
   on `visibilitychange`. Denied microphone access turns the option off with an explanation. The
   server's `Permissions-Policy` allows the microphone for this origin only.
3. **Countdown.** The phrase or the SOS button opens a full-screen alert dialog with a 10-second
   countdown, a large Cancel button (focused) and a vibration on Android.
4. **Alert.** The location must be where the user is *now*: `LocationTracker.fresh()` asks for a new
   high-accuracy fix (`maximumAge: 0`) as soon as the countdown starts, so the 10 seconds double as
   GPS time; only if that fails is the last fix used, and only if it is under a minute old. Then Umbra
   opens the Messages app with an
   `sms:` link (`&body=` on iOS, `?body=` elsewhere) addressed to the contact, containing the time,
   accuracy, coordinates and an OpenStreetMap link, and shows a `tel:` Call button for the contact or
   911. The text goes first, so the location is shared whether or not the call is answered.

Browsers do not let a page send a text or start a call without a tap, or listen while it is in the
background; the README states these limits plainly.

## Security

| Threat | Mitigation |
|---|---|
| Abuse / scraping / brute force | Token-bucket rate limits: 20 req/s per IP for `/api` (burst 80), 5 sign-in attempts then 1 per 12 s, 20 assistant questions/hour per account (`429` + `Retry-After`). Memory per limiter is bounded. |
| Cross-site scripting | Strict CSP: scripts only from this origin and Leaflet's pinned, SRI-checked CDN file; no `unsafe-inline` or `unsafe-eval`. Map data and model output are inserted with `textContent` only. |
| Cross-site request forgery | `SameSite=Strict` cookies plus a required `X-Umbra-Request: 1` header on every state-changing request (browsers cannot send it cross-site without a CORS preflight, which the server never grants). |
| Clickjacking | `frame-ancestors 'none'` and `X-Frame-Options: DENY`. |
| Slow or huge requests | 64 KB body limit, 10 s read/write timeouts, 5 s keep-alive. |
| Stolen database | Argon2id password hashes; only hashes of session tokens. |
| Leaked secrets | API keys only from the environment / git-ignored `.env`; never logged; error messages never include upstream URLs (HERE puts the key in the query string). |
| Privacy | Logs omit query strings; only the map's centre and bounding box are sent upstream. |
| Spoofed client IPs | `X-Forwarded-For` is ignored unless `--trust-proxy`, and then only the entry the proxy added last is used. |

## Installable web app (iPhone)

`web/manifest.webmanifest` (name, icons, `display: standalone`) and the Apple meta tags in
`index.html` (`apple-touch-icon`, `apple-mobile-web-app-capable`) let Safari add Umbra to the Home
Screen, where it opens full-screen. `viewport-fit=cover` plus `env(safe-area-inset-*)` padding keeps the
UI clear of the notch and the home indicator, and inputs are 16 px on phones so Safari does not zoom in
when a search box gets focus.

`web/sw.js` is a service worker: app files are network-first (a new deploy shows on the next load, with
the cached copy as an offline fallback), Leaflet and fonts from their CDNs are cache-first, and
`/api/*` and map tiles are never cached (routes and weather must be live, and the OpenStreetMap tile
policy discourages bulk caching). The server maps `.webmanifest` to `application/manifest+json`.

The `Dockerfile` builds the server on Debian (GCC) and ships it with `web/` and `data/` in a slim
runtime image, running as a non-root user and honouring `$PORT`, so it can be hosted behind HTTPS on any
container platform. CI builds the image and smoke-tests it.

## Concurrency

After startup every structure is immutable. Each request allocates its own `best`, `parent`, `settled`
and heap arrays (O(V), about 300 KB for Tempe), so cpp-httplib's worker threads share the graph without
locks. The mutable pieces are small and guarded: the live-data caches (a mutex around the refresh;
readers keep a `shared_ptr` to the immutable snapshot they got, so a refresh never changes data under a
running request), the rate limiters (a mutex around a hash map) and the user store (one SQLite
connection behind a mutex, with password hashing outside it). The assistant blocks its worker thread
while waiting for the model (up to 60 s per call), which is why it is rate-limited per account.

## Testing strategy

| Level | What |
|---|---|
| Unit | Heap operations and error cases, CSR layout, SCC pruning, maxspeed/oneway parsing, clear-sky model, comfort rules |
| Property | Heap vs a reference model (20k random ops); Dijkstra vs Bellman-Ford; A\* vs Dijkstra; multi-source/multi-target search vs the best of all pairs; grid vs brute-force nearest neighbour |
| Fixture | Hand-written OSM extracts covering one-way, private, footway, missing-node and island cases, plus places (parking filtered, duplicates merged, way centres), a campus (entrances on ways and multipolygons, service doors ignored, outline fallback, off-campus buildings dropped) and an Open-Meteo response (missing hours, interpolation, preceding-hour radiation) |
| Weather service | Fake HTTP client and fake clock: caching, back-off after failures, stale-data limit, only the map centre is requested |
| Drive scenarios | No traffic → freeway; a jam → the street; rain / heavy rain / snow / fog factors; jam + storm combined; conditions never speed a road up; glare only for a low sun ahead; HERE parsing and same-direction matching |
| Walk scenarios | Sunny, overcast (sun strength 0), night with lit / unlit / unknown lighting, step-free, water stop chosen and legs joined, departures across day and night |
| Accounts | Sign-up validation, case-insensitive email, identical errors for unknown email and wrong password, forged and logged-out tokens, ownership of places, trip pruning, cascade on delete |
| Server | Token-bucket limiter with a fake clock, proxy-aware client IP, CSP contents, config flags; the assistant against a scripted fake model (tool round trip, unknown tools, max rounds, provider failure) |
| Glare | Sunset drive takes the off-axis dog-leg only when avoiding glare; no detour at night; the low-visor threshold catches a 30° sun; the penalty never undercuts free-flow time |
| Web (Node) | Navigation: distance to the next turn, re-route only after two off-route fixes, vague fixes ignored, arrival. Emergency location: a fresh fix is used, a recent one is the fallback, a stale one is refused. Safety: phrase normalisation, phone validation, iOS/Android `sms:` formats, the location message. Departures: best-option ordering |
| Live data | Background-mode cache: `get()` returns immediately while a slow fetch runs, one fetch at a time, the value appears when it completes |
| Alternatives | Two-road graph returns both roads best first with true costs; the stretch limit drops a too-long road; on random graphs the first route is optimal and the others respect both limits |
| Search | Tokenisation and abbreviations, prefix-while-typing, intersections in any word order, typo tolerance, ranking, limits |
| Integration | `umbra_bench` on the real Tempe data in CI (fails if Dijkstra and A\* ever disagree) |
| Memory safety | Debug preset with AddressSanitizer + UndefinedBehaviorSanitizer |
