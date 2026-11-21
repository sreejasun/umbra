# Changelog

All notable changes to Umbra. Dates are in UTC.

## [Unreleased]

### Added
- **Maps-app interface:** full-screen map with a floating search bar, screens with their own URLs
  (place, directions, steps, options, best time, navigation, settings, account, safety, help, about),
  a bottom sheet on phones and a side panel on desktop, 2-3 **route choices** (penalty method for
  drives, shade preferences for walks), Profile & settings with a **light / dark / system theme**,
  Developer mode for the engine statistics, and an Ask Umbra button that also helps find settings.
- Live weather and traffic refresh on background threads, so a slow provider never delays a route.
- **Sun-glare avoidance for drivers** (3× cost for minutes into a low sun, a 35° setting for drivers
  whose visor doesn't shade their eyes) and departure times for drives.
- **Safety:** opt-in danger phrase (speech recognition) and SOS button with a 10-second countdown,
  then a prefilled text of your location to your emergency contact and one-tap calling (contact or 911).
- JavaScript unit tests (Node's test runner), run by `ctest` and CI.
- **Live traffic** (HERE Traffic v7, optional API key): flow for the whole map refreshed every
  15 minutes and matched onto road edges by position and direction; drive times include it.
- **Weather-aware driving:** FHWA road-weather speed reductions for rain, heavy rain, snow and fog,
  and **sun-glare warnings** when a leg heads into a low sun.
- **Accounts** (SQLite + libsodium): sign-up/sign-in, saved places, recent trips, data export and
  account deletion.
- **Trip assistant** "Ask Umbra" (xAI Grok, optional API key) that plans with Umbra's own engine
  through five tools.
- **Best time to leave:** the same walk at up to 16 departure times with shade, feels-like
  temperature and comfort.
- **Heat-relief stops:** "Stop for water" routes via the drinking fountain that adds the least
  cost (two multi-source Dijkstra runs).
- **Step-free walks** (no stairs) and **lit streets after dark**.
- **Live navigation with voice:** follow-me map, spoken turns, automatic re-routing.
- **Security hardening:** per-IP rate limits, Content-Security-Policy and other headers, CSRF
  protection, request size and time limits.
- Directions to ASU buildings (entrances via multi-target search), live weather (Open-Meteo),
  installable web app (PWA), Dockerfile, CodeQL and Dependabot.

### Fixed
- Switching between Drive and Walk no longer starts the walk from the drive route's road: the points
  you choose (your location, a map click) are kept, and each mode snaps them to its own network.
  A dotted line links your point to the route where they differ.
- Emergency alerts take a fresh, high-accuracy location fix (started when the countdown starts)
  instead of reusing the last known position, which could be stale.

### Changed
- The HTTP layer is split into a shared query layer used by both the API and the assistant.
- Weather and traffic share one cache (`CachedResource`) with refresh, back-off and max age.

## [1.0.0]

- Dijkstra / A\* on a CSR graph with an indexed heap, SCC pruning, spatial index, search index,
  turn-by-turn directions, shade-aware walking (NOAA sun, building/tree shadows) and the web UI.
