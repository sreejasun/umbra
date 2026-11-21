# HTTP API

All endpoints return JSON. Errors return `{"error": "..."}` with one of these status codes:

| Status | Meaning |
|---|---|
| `400` | Bad parameters, or a state-changing request without the `X-Umbra-Request: 1` header |
| `401` | Sign-in required |
| `422` | Valid request that cannot be served: point outside the map, wrong password, feature disabled |
| `429` | Rate limit: 20 requests/s per IP (configurable), 5 sign-in attempts then 1 per 12 s, 20 assistant questions/hour per account |
| `502` | The assistant's model provider failed |

Points are `lat,lng`. Wherever `from` / `to` is accepted, up to 32 points separated by `;` may be
given (for example a building's `accessPoints`); the route uses whichever pair is cheapest, found
with one multi-source / multi-target search. `fromName` / `toName` (optional) label the ends
(*"Arrive at Hayden Library"*). Times are Unix seconds (default: now).

## Map and search

| Endpoint | Description |
|---|---|
| `GET /api/health` | Liveness check |
| `GET /api/info` | Region, bounds, timezone, graph size, walking data, and `features` (`weather`, `traffic`, `accounts`, `assistant`) |
| `GET /api/nearest?lat=&lng=` | Snap a point to the nearest intersection |
| `GET /api/search?q=&limit=8` | Places, campus buildings, streets and intersections. Buildings add `code`, `accessPoints` (entrances, or outline points) and `entrancesMapped` |
| `GET /api/sun?time=` | Sun azimuth / elevation over the map |
| `GET /api/shadows?bbox=s,w,n,e&time=` | Building footprints and shadow outlines in a viewport |
| `GET /api/weather?time=` | Forecast at that time, `sunStrength` (0-1) and `comfort` (`pleasant`, `fair`, `caution`, `dangerous`) with a headline and advice |

## Drive: `GET /api/route`

| Parameter | Values (default first) |
|---|---|
| `from`, `to` | points (required) |
| `metric` | `time` \| `distance` |
| `algorithm` | `dijkstra` \| `astar` |
| `explored` | `true` \| `false`: include settled nodes for the search animation |
| `traffic` | `live` \| `off`: live HERE traffic (when the server has a key) |
| `weather` | `live` \| `off`: weather slowdowns (FHWA ranges) |
| `time` | departure, Unix seconds (default now). Live traffic applies only within 15 minutes of now |
| `avoidGlare` | `false` \| `true`: minutes driven into a low sun cost 3× |
| `glareMaxElevation` | sun elevation (5-60°) below which facing the sun counts as glare; default 20, the UI uses 35 for "my sun visor doesn't shade my eyes" |
| `alternatives` | `false` \| `true`: also return up to 2 different routes |

The response has `summary.durationS` under the conditions and `summary.freeFlowDurationS`
without them, `conditions` (`traffic`, `weather.slowdown`, `sun`, `glare`, `glareS` seconds driven
into the sun, and with `avoidGlare` `glareAvoided: {fastestDurationS, fastestGlareS}` for the plain
fastest route), the departure `time`, a `label` ("Fastest", "Shortest" or "Least sun glare") and
`via` (main road), turn-by-turn `steps`
(each with `glare: true` when the leg heads into a low sun), `geometry`, and a Dijkstra-vs-A\*
`comparison`. With `alternatives=true`, `alternatives[]` holds the other routes, each with `label`
("Shorter", "Fewer turns", "Less sun glare" or "via *road*"), `via`, `summary`, `conditions.glareS`,
`geometry` and `steps`.

<details>
<summary>Example (trimmed)</summary>

```json
{
  "found": true, "mode": "drive", "metric": "time", "algorithm": "astar",
  "origin":      { "label": "East Lemon Street", "position": [33.416112, -111.934847], "snapDistanceM": 167.0 },
  "destination": { "label": "East Rio Salado Parkway", "position": [33.429341, -111.901034], "snapDistanceM": 73.4 },
  "summary": { "distanceM": 5063.1, "durationS": 318.3, "freeFlowDurationS": 318.3, "nodesSettled": 5110, "computeMs": 0.77 },
  "conditions": { "traffic": null, "weather": { "slowdown": "", "description": "Mainly clear" }, "glare": false },
  "comparison": { "astar": { "nodesSettled": 5110 }, "dijkstra": { "nodesSettled": 11432 } },
  "steps": [
    { "instruction": "Head west on East Lemon Street", "maneuver": "depart", "distanceM": 10, "glare": false },
    { "instruction": "Arrive at East Rio Salado Parkway", "maneuver": "arrive", "distanceM": 0 }
  ],
  "geometry": [[33.416112, -111.934847], "..."]
}
```
</details>

## Walk: `GET /api/walk`

| Parameter | Values (default first) |
|---|---|
| `from`, `to` | points (required) |
| `time` | departure, Unix seconds |
| `shade` | `balanced` \| `shortest` \| `max` |
| `weather` | `live` \| `off`: live sun strength scales the shade penalty |
| `avoidStairs` | `false` \| `true`: step-free route (no stairs or `wheelchair=no` ways) |
| `preferLit` | `true` \| `false`: after dark, prefer lit ways |
| `viaWater` | `false` \| `true`: stop at the drinking fountain that adds the least cost |
| `alternatives` | `false` \| `true`: also return the walks for the other shade preferences |
| `algorithm`, `explored` | as for `/api/route` |

The response has `preferred` and `shortest` routes (each with `summary`: distance, duration,
`shadePct`, `litPct`, `unlitPct`; `geometry`; shaded/sunny `runs`; `steps` on `preferred`), plus
`sun`, `sunStrength`, `weather`, `lighting` (whether the night preference applied), `avoidStairs`
and `via` (`{name, position}` of the water stop), `label` (e.g. "Balanced shade"), and with
`alternatives=true` an `alternatives[]` array of walks (`label`, `preference`, `summary`, `geometry`,
`runs`, `steps`) for the other shade preferences that take different paths.

## Best time to leave: `GET /api/walk/departures`

The `/api/walk` parameters, plus `step` (seconds between departures, 300-21600, default 900) and
`count` (1-16, default 12), starting at `time`. Returns `options[]` with `time`, `found`,
`distanceM`, `durationS`, `shadePct`, `sunUp`, `sunStrength`, `feelsLikeC` and `comfort`.

## Accounts

Enabled when the server has a database (`UMBRA_DB`). Sessions use an `HttpOnly`, `SameSite=Strict`
cookie (`Secure` with `UMBRA_HTTPS=on`) valid for 30 days. **Every `POST` and `DELETE` must send
`X-Umbra-Request: 1`** and a JSON body where one is needed.

| Endpoint | Body | Description |
|---|---|---|
| `POST /api/auth/register` | `{email, password, name}` | Create an account (password 10-128 characters) and sign in |
| `POST /api/auth/login` | `{email, password}` | Sign in |
| `POST /api/auth/logout` | `{}` | Sign out |
| `GET /api/me` | | `{"user": {...}}`, or `{"user": null}` when signed out |
| `DELETE /api/me` | | Delete the account and all its data |
| `GET /api/me/export` | | Download everything stored about you (JSON) |
| `GET /api/me/places` | | Saved places |
| `POST /api/me/places` | `{name, position: [lat, lng]}` | Save a place (up to 100) |
| `DELETE /api/me/places/{id}` | | Remove a saved place |
| `GET /api/me/trips` | | Recent trips, newest first (the last 50 are kept) |
| `POST /api/me/trips` | `{mode, fromName, from, toName, to}` | Record a trip |
| `DELETE /api/me/trips` | | Clear trip history |

## Assistant: `POST /api/assistant`

Enabled when the server has `XAI_API_KEY`. Requires sign-in when accounts are enabled.

```json
{ "question": "Shadiest walk from Coor Hall to the MU after 4pm?", "position": [33.4176, -111.9346] }
```

`position` (optional) lets the user say "from here". The response is
`{"answer": "...", "route": <a /api/walk or /api/route response, or null>, "toolsUsed": ["plan_walk"]}`.
The model can call five tools, all answered by Umbra's own engine: `search_places`, `plan_walk`,
`plan_drive`, `get_weather` and `best_departure`.
