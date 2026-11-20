// App controller: wires user input, the API and the map together.

import { AccountPanel } from "./account.js";
import { api } from "./api.js";
import { AssistantPanel } from "./assistant.js";
import { renderDepartures } from "./departures.js";
import {
  formatDistance,
  formatDuration,
  formatInteger,
  formatMillis,
  formatTemperature,
  percentFewer,
} from "./format.js";
import { LocationTracker } from "./location.js";
import { MapView } from "./map-view.js";
import { Navigator } from "./navigation.js";
import { SafetyMonitor } from "./safety.js";
import { SearchBox } from "./search-box.js";
import { formatMinutes, nowIn, unixToZoned, zonedToUnix } from "./time.js";

const $ = (id) => document.getElementById(id);

const SHADOW_MIN_ZOOM = 15;
// Sun elevations below which a driver facing the sun is dazzled (see solar.hpp).
const STANDARD_GLARE_DEG = 20;
const LOW_VISOR_GLARE_DEG = 35;
const ALGORITHM_NAMES = { dijkstra: "Dijkstra", astar: "A*" };

const state = {
  origin: null, // {lat, lng}
  destination: null,
  labels: { origin: null, destination: null }, // names picked from search (kept over snapped labels)
  access: { origin: null, destination: null }, // a building's entrances: route to whichever is best
  mode: "drive", // drive | walk
  metric: "time", // drive: time | distance
  shade: "balanced", // walk: shortest | balanced | max
  algorithm: "dijkstra",
  showExplored: true,
  showShadows: true,
  timezone: "America/Phoenix",
  date: null, // walk time, as a wall-clock date and minute in the map's timezone
  minutes: 720,
  followNow: true, // time picker tracks the current time until the user changes it
  walkingAvailable: false,
  coverage: null, // L.LatLngBounds of the map data
  weatherEnabled: false,
  avoidStairs: false,
  viaWater: false,
  avoidGlare: false, // drive: steer away from roads into a low sun
  lowVisor: false, // the sun visor doesn't shade the driver's eyes (short drivers)
  route: null, // last route shown (for navigation)
  centerOnNextFix: false,
  requestId: 0, // guards against out-of-order route responses
};

const view = new MapView("map", {
  onMapClick: handleMapClick,
  onMarkerMoved: (kind, latlng) => setWaypoint(kind, latlng),
  onViewChanged: () => debounce("shadows", refreshShadows, 250),
  onLocate: locateMe,
});

const searchBoxes = {
  origin: new SearchBox($("search-origin"), $("search-origin-list"), {
    onSelect: (result) => chooseSearchResult("origin", result),
  }),
  destination: new SearchBox($("search-destination"), $("search-destination-list"), {
    onSelect: (result) => chooseSearchResult("destination", result),
  }),
};

const navigator_ = new Navigator({
  onProgress: (step, distanceM) => {
    $("nav-distance").textContent = formatDistance(distanceM);
    $("nav-instruction").textContent = step.instruction;
  },
  onReroute: (fix) => setWaypoint("origin", fix, "My location"),
  onArrive: () => {
    stopNavigation();
    showToast("You have arrived.", "info");
  },
});

let account = null; // AccountPanel, when the server has accounts

const tracker = new LocationTracker({
  onUpdate: handleLocation,
  onError: (message) => {
    view.setLocateState("idle");
    showToast(message);
  },
});

// ------------------------------------------------------------------ helpers

const timers = {};
function debounce(key, fn, ms) {
  clearTimeout(timers[key]);
  timers[key] = setTimeout(fn, ms);
}

const toLatLng = ([lat, lng]) => ({ lat, lng });

function walkTime() {
  return zonedToUnix(state.date, state.minutes, state.timezone);
}

function hasTrip() {
  return Boolean(state.origin && state.destination);
}

// ----------------------------------------------------------------- startup

init();
registerServiceWorker();

/** Installable app + offline shell. Needs a secure origin (HTTPS or localhost). */
function registerServiceWorker() {
  if (!("serviceWorker" in navigator) || !window.isSecureContext) return;
  navigator.serviceWorker.register("sw.js").catch(() => {
    /* the app works without it, just not offline */
  });
}

const DRIVER_PREFS_KEY = "umbra.driver";

/** Glare preferences are personal (seat height), so they are remembered on this device. */
function loadDriverPrefs() {
  try {
    const prefs = JSON.parse(localStorage.getItem(DRIVER_PREFS_KEY) ?? "{}");
    state.avoidGlare = Boolean(prefs.avoidGlare);
    state.lowVisor = Boolean(prefs.lowVisor);
  } catch {
    /* storage unavailable: defaults */
  }
  $("glare-toggle").checked = state.avoidGlare;
  $("visor-toggle").checked = state.lowVisor;
}

function saveDriverPrefs() {
  try {
    localStorage.setItem(DRIVER_PREFS_KEY, JSON.stringify({ avoidGlare: state.avoidGlare, lowVisor: state.lowVisor }));
  } catch {
    /* not critical */
  }
}

/** Resolve with a fresh-enough location fix, or reject after 8 s. */
function currentFix() {
  return Promise.race([
    tracker.current(),
    new Promise((_, reject) => setTimeout(() => reject(new Error("timeout")), 8000)),
  ]);
}

async function init() {
  bindControls();
  loadDriverPrefs();
  new SafetyMonitor({ getFix: currentFix, toast: (message) => showToast(message, "info") });
  try {
    const info = await api.info();
    $("region-name").textContent = info.region.name;
    $("graph-size").textContent =
      `${formatInteger(info.graph.nodes)} intersections and ${formatInteger(info.graph.edges)} directed road segments`;
    $("search-size").textContent = formatInteger(info.graph.searchable);
    state.timezone = info.region.timezone || state.timezone;
    state.coverage = L.latLngBounds(info.region.bounds);

    state.walkingAvailable = Boolean(info.walking);
    state.weatherEnabled = Boolean(info.features?.weather);
    $("water-switch").hidden = !info.walking?.waterStops;
    if (state.walkingAvailable) {
      $("shade-size").textContent =
        `${formatInteger(info.walking.buildings)} buildings and ${formatInteger(info.walking.trees)} trees`;
    } else {
      const button = $("walk-mode-btn");
      button.disabled = true;
      button.title = "Walking data not loaded (run scripts/fetch_walk.sh)";
    }

    setTimeToNow();
    view.showCoverage(info.region.bounds);
    restoreFromUrl();
    applyMode();
    updateTimeUi();
    refreshSun();
    $("weather-about").hidden = !state.weatherEnabled;
    $("weather-credit").hidden = !state.weatherEnabled;
    refreshWeather();
    startLocation();
    startOptionalFeatures(info.features ?? {});
  } catch (err) {
    $("region-name").textContent = "offline";
    showToast(err.message);
  }
}

/** Accounts and the assistant exist only when the server enables them. */
function startOptionalFeatures(features) {
  if (features.accounts) {
    account = new AccountPanel({
      onPickPlace: (place) => setWaypoint(state.origin ? "destination" : "origin", toLatLng(place.position), place.name),
      onPickTrip: (trip) => {
        selectOption("mode", trip.mode);
        applyMode();
        state.origin = toLatLng(trip.from);
        state.labels.origin = trip.fromName;
        view.setMarker("origin", state.origin);
        setWaypoint("destination", toLatLng(trip.to), trip.toName);
      },
      toast: (message) => showToast(message, "info"),
    });
    account.init();
  }
  if (features.assistant) {
    new AssistantPanel({
      onRoute: showAssistantRoute,
      getPosition: () => tracker.last,
      needsSignIn: () => Boolean(account) && !account.signedIn,
      openSignIn: () => account.open(),
    });
  }
}

function bindControls() {
  document.querySelectorAll(".segmented[data-setting]").forEach((group) => {
    group.addEventListener("click", (e) => {
      const button = e.target.closest("button[data-value]");
      if (!button || button.disabled || button.getAttribute("aria-checked") === "true") return;
      selectOption(group.dataset.setting, button.dataset.value);
      if (group.dataset.setting === "mode") applyMode();
      if (hasTrip()) computeRoute();
      else syncUrl();
    });
  });

  $("explore-toggle").addEventListener("change", (e) => {
    state.showExplored = e.target.checked;
    if (hasTrip()) computeRoute();
  });

  $("stairs-toggle").addEventListener("change", (e) => {
    state.avoidStairs = e.target.checked;
    if (hasTrip() && state.mode === "walk") computeRoute();
  });
  $("water-toggle").addEventListener("change", (e) => {
    state.viaWater = e.target.checked;
    if (hasTrip() && state.mode === "walk") computeRoute();
  });
  $("glare-toggle").addEventListener("change", (e) => {
    state.avoidGlare = e.target.checked;
    saveDriverPrefs();
    if (hasTrip() && state.mode === "drive") computeRoute();
  });
  $("visor-toggle").addEventListener("change", (e) => {
    state.lowVisor = e.target.checked;
    saveDriverPrefs();
    if (hasTrip() && state.mode === "drive") computeRoute();
  });
  $("departures-btn").addEventListener("click", loadDepartures);
  $("nav-start").addEventListener("click", startNavigation);
  $("nav-stop").addEventListener("click", stopNavigation);
  $("nav-voice").addEventListener("click", (e) => {
    const on = e.currentTarget.getAttribute("aria-pressed") !== "true";
    navigator_.setVoice(on);
    e.currentTarget.setAttribute("aria-pressed", String(on));
    e.currentTarget.textContent = on ? "Voice on" : "Voice off";
  });
  $("save-place").addEventListener("click", () => {
    if (state.destination) account?.savePlace(state.labels.destination ?? "Saved place", state.destination);
  });

  $("shadows-toggle").addEventListener("change", (e) => {
    state.showShadows = e.target.checked;
    refreshShadows();
  });

  // Time of the walk.
  $("walk-time").addEventListener("input", (e) => {
    state.minutes = Number(e.target.value);
    state.followNow = false;
    updateTimeUi();
    debounce("time", onTimeChanged, 300);
  });
  $("walk-date").addEventListener("change", (e) => {
    if (!e.target.value) return;
    state.date = e.target.value;
    state.followNow = false;
    updateTimeUi();
    onTimeChanged();
  });
  $("time-now").addEventListener("click", () => {
    setTimeToNow();
    updateTimeUi();
    onTimeChanged();
  });

  $("swap-btn").addEventListener("click", () => {
    if (!hasTrip()) return;
    [state.origin, state.destination] = [state.destination, state.origin];
    [state.labels.origin, state.labels.destination] = [state.labels.destination, state.labels.origin];
    [state.access.origin, state.access.destination] = [state.access.destination, state.access.origin];
    view.setMarker("origin", state.origin);
    view.setMarker("destination", state.destination);
    computeRoute();
  });

  $("use-location").addEventListener("click", useMyLocationAsStart);
  $("clear-btn").addEventListener("click", clearAll);
  document.addEventListener("keydown", (e) => {
    const typing = e.target instanceof HTMLInputElement;
    if (e.key === "Escape" && !typing) clearAll();
    if (e.key === "/" && !typing) {
      e.preventDefault(); // "/" focuses search, like many web apps
      searchBoxes[state.origin && !state.destination ? "destination" : "origin"].focus();
    }
  });
}

function selectOption(setting, value) {
  const group = document.querySelector(`.segmented[data-setting="${setting}"]`);
  const button = group?.querySelector(`button[data-value="${value}"]`);
  if (!button || button.disabled) return;
  group.querySelectorAll("button").forEach((b) => b.setAttribute("aria-checked", String(b === button)));
  state[setting] = value;
}

function applyMode() {
  document.body.dataset.mode = state.mode;
  refreshShadows();
}

// -------------------------------------------------------------- walk time

function setTimeToNow() {
  const now = nowIn(state.timezone);
  state.date = now.date;
  state.minutes = Math.floor(now.minutes / 15) * 15;
  state.followNow = true;
}

function updateTimeUi() {
  $("walk-date").value = state.date;
  $("walk-time").value = String(state.minutes);
  $("walk-time-label").textContent = formatMinutes(state.minutes);
  $("time-now").setAttribute("aria-pressed", String(state.followNow));
}

function onTimeChanged() {
  refreshSun();
  refreshWeather();
  refreshShadows();
  if (hasTrip()) computeRoute();
  else syncUrl();
}

let sunSeq = 0;
async function refreshSun() {
  const seq = ++sunSeq;
  try {
    const sun = await api.sun(walkTime());
    if (seq !== sunSeq) return;
    renderSun(sun);
  } catch {
    /* non-critical */
  }
}

// ---------------------------------------------------------------- weather

let weatherSeq = 0;
async function refreshWeather() {
  if (!state.weatherEnabled) return;
  const seq = ++weatherSeq;
  try {
    const weather = await api.weather(walkTime());
    if (seq === weatherSeq) renderWeather(weather);
  } catch {
    if (seq === weatherSeq) renderWeather(null);
  }
}

/** Live conditions under the time picker; `null` = no forecast for that time. */
function renderWeather(weather) {
  const box = $("weather-readout");
  box.hidden = !state.weatherEnabled;
  if (!state.weatherEnabled) return;
  if (!weather) {
    box.dataset.level = "none";
    $("weather-text").textContent = "No forecast for this time, so a clear sky is assumed.";
    $("weather-badge").textContent = "";
    $("weather-advice").textContent = "";
    return;
  }
  box.dataset.level = weather.comfort.level;
  const parts = [
    weather.description,
    `${formatTemperature(weather.temperatureC)} (feels ${formatTemperature(weather.feelsLikeC)})`,
    `${Math.round(weather.cloudCoverPct)}% clouds`,
  ];
  if (weather.uvIndex >= 1) parts.push(`UV ${Math.round(weather.uvIndex)}`);
  $("weather-text").textContent = parts.join(" · ");
  $("weather-badge").textContent = weather.comfort.headline;
  $("weather-advice").textContent = weather.comfort.advice;
}

function renderSun(sun) {
  const readout = $("sun-readout");
  readout.classList.toggle("is-night", !sun.up);
  $("sun-text").textContent = sun.up
    ? `Sun ${Math.round(sun.elevationDeg)}° high, in the ${sun.direction}`
    : "Sun is down: every path is in shade";
}

// ---------------------------------------------------------------- shadows

let shadowSeq = 0;
async function refreshShadows() {
  const note = $("map-note");
  const wanted = state.mode === "walk" && state.showShadows && state.walkingAvailable;
  if (!wanted) {
    view.clearShadows();
    note.hidden = true;
    return;
  }
  if (view.zoom < SHADOW_MIN_ZOOM) {
    view.clearShadows();
    note.textContent = "Zoom in to see building shadows";
    note.hidden = false;
    return;
  }

  const seq = ++shadowSeq;
  try {
    const data = await api.shadows(view.viewBounds(), walkTime());
    if (seq !== shadowSeq) return;
    view.setShadows(data.buildings);
    note.hidden = !data.truncated;
    if (data.truncated) note.textContent = "Zoom in to see every shadow";
  } catch {
    /* keep the previous shadows */
  }
}

// ------------------------------------------------------- shareable links

function parsePoint(text) {
  const parts = (text || "").split(",").map(Number);
  return parts.length === 2 && parts.every(Number.isFinite) ? { lat: parts[0], lng: parts[1] } : null;
}

/** Load a trip from ?from=lat,lng&to=lat,lng&mode=walk&shade=max&t=<unix>&... */
function restoreFromUrl() {
  const params = new URLSearchParams(window.location.search);
  selectOption("metric", params.get("metric"));
  selectOption("algorithm", params.get("algorithm"));
  selectOption("shade", params.get("shade"));
  if (state.walkingAvailable) selectOption("mode", params.get("mode"));

  const t = Number(params.get("t"));
  if (params.has("t") && Number.isFinite(t)) {
    const zoned = unixToZoned(t, state.timezone);
    state.date = zoned.date;
    state.minutes = zoned.minutes;
    state.followNow = false;
  }

  const from = parsePoint(params.get("from"));
  const to = parsePoint(params.get("to"));
  if (from && to) {
    state.origin = from;
    view.setMarker("origin", from);
    setWaypoint("destination", to);
  } else if (from) {
    setWaypoint("origin", from);
  }
}

/** Keep the address bar in sync so the current route can be shared. */
function syncUrl() {
  const params = new URLSearchParams();
  const fmt = (p) => `${p.lat.toFixed(5)},${p.lng.toFixed(5)}`;
  if (state.origin) params.set("from", fmt(state.origin));
  if (state.destination) params.set("to", fmt(state.destination));
  if (state.mode === "walk") {
    params.set("mode", "walk");
    params.set("shade", state.shade);
    if (!state.followNow) params.set("t", String(walkTime()));
  } else if (state.origin && state.destination) {
    params.set("metric", state.metric);
    if (!state.followNow) params.set("t", String(walkTime()));
  }
  if (state.origin && state.destination) params.set("algorithm", state.algorithm);
  const query = params.toString();
  window.history.replaceState(null, "", query ? `?${query}` : window.location.pathname);
}

// ------------------------------------------------------------ interaction

function handleMapClick(latlng) {
  if (!state.origin) setWaypoint("origin", latlng);
  else setWaypoint("destination", latlng); // sets, or replaces, the destination
}

function chooseSearchResult(kind, result) {
  const point = toLatLng(result.position);
  const access = result.accessPoints?.length ? result.accessPoints.map(toLatLng) : null;
  setWaypoint(kind, point, result.name, access);
  if (!hasTrip()) view.reveal(point); // with a full trip, the route view takes over
  if (kind === "origin" && !state.destination) searchBoxes.destination.focus();
}

/**
 * Set the start or destination. `label` is a name chosen in the search box (or
 * "My location"); map clicks and marker drags pass none, so the snapped
 * intersection is shown instead. `access` lists a building's entrances (or
 * outline points): the engine routes to whichever one is cheapest to reach.
 */
async function setWaypoint(kind, latlng, label = null, access = null) {
  hideToast();
  state[kind] = { lat: latlng.lat, lng: latlng.lng };
  state.labels[kind] = label;
  state.access[kind] = access;
  if (label) setWaypointLabel(kind, label);
  view.setMarker(kind, latlng);
  updateButtons();

  if (hasTrip()) {
    computeRoute();
    return;
  }

  // A building stays at its footprint until a route picks the entrance.
  if (access) {
    syncUrl();
    return;
  }

  // Only one point so far: snap it so the user sees which intersection was picked.
  const id = ++state.requestId;
  try {
    const snapped = await api.nearest(state[kind]);
    if (id !== state.requestId) return;
    const [lat, lng] = snapped.position;
    state[kind] = { lat, lng };
    view.setMarker(kind, state[kind]);
    setWaypointLabel(kind, snapped.label);
  } catch (err) {
    if (id !== state.requestId) return;
    state[kind] = null;
    state.labels[kind] = null;
    view.removeMarker(kind);
    setWaypointLabel(kind, null);
    updateButtons();
    showToast(err.message);
  }
  syncUrl();
}

async function computeRoute() {
  const id = ++state.requestId;
  const mode = state.mode;
  view.clearRoute();
  hideToast();
  setBusy(true, mode === "walk" ? "Finding a shady route…" : "Finding route…");

  const from = state.access.origin ?? state.origin;
  const to = state.access.destination ?? state.destination;
  const names = { fromName: state.labels.origin, toName: state.labels.destination };
  let route;
  try {
    route =
      mode === "walk"
        ? await api.walk(from, to, {
            time: walkTime(),
            shade: state.shade,
            algorithm: state.algorithm,
            explored: state.showExplored,
            avoidStairs: state.avoidStairs,
            viaWater: state.viaWater,
            ...names,
          })
        : await api.route(from, to, {
            metric: state.metric,
            algorithm: state.algorithm,
            explored: state.showExplored,
            time: walkTime(),
            avoidGlare: state.avoidGlare,
            glareMaxElevation: state.lowVisor ? LOW_VISOR_GLARE_DEG : STANDARD_GLARE_DEG,
            ...names,
          });
  } catch (err) {
    if (id === state.requestId) {
      setBusy(false);
      showToast(err.message);
    }
    return;
  }
  if (id !== state.requestId) return;
  setBusy(false);
  await displayRoute(route, id);
}

/** Show a route response (from the form or the assistant) on the map and in the sidebar. */
async function displayRoute(route, id) {
  const mode = route.mode;
  // Move markers onto the points the engine actually routed between (for a
  // building, the entrance it picked).
  state.origin = toLatLng(route.origin.position);
  state.destination = toLatLng(route.destination.position);
  view.setMarker("origin", state.origin);
  view.setMarker("destination", state.destination);
  setWaypointLabel("origin", route.origin.label);
  setWaypointLabel("destination", route.destination.label);
  syncUrl();

  if (!route.found) {
    renderResults(null);
    showToast(mode === "walk" ? "No walkable route connects these points." : "No drivable route connects these points.");
    return;
  }

  state.route = mode === "walk" ? { steps: route.preferred.steps, geometry: route.preferred.geometry } : route;
  renderResults(route);
  if (navigator_.active) {
    navigator_.start(state.route); // re-routed while navigating: no animation, no new history entry
  } else {
    account?.recordTrip({
      mode,
      fromName: state.labels.origin ?? route.origin.label,
      from: route.origin.position,
      toName: state.labels.destination ?? route.destination.label,
      to: route.destination.position,
    });
  }
  if (!navigator_.active && state.showExplored && route.explored.length) {
    const completed = await view.animateExplored(route.explored);
    if (!completed || id !== state.requestId) return;
  }

  if (mode === "walk") {
    const differs = Math.abs(route.preferred.summary.distanceM - route.shortest.summary.distanceM) > 1;
    view.drawWalkRoute(route.preferred.runs, differs ? route.shortest.geometry : null);
    renderDirections(route.preferred.steps);
  } else {
    view.drawRoute(route.geometry);
    renderDirections(route.steps);
  }
}

/** A route planned by the assistant: switch to its mode and show it. */
function showAssistantRoute(route) {
  selectOption("mode", route.mode);
  applyMode();
  state.labels = { origin: route.origin.label, destination: route.destination.label };
  state.access = { origin: null, destination: null };
  if (route.mode === "walk") {
    const zoned = unixToZoned(route.time, state.timezone);
    state.date = zoned.date;
    state.minutes = zoned.minutes;
    state.followNow = false;
    updateTimeUi();
  }
  view.clearRoute();
  displayRoute(route, ++state.requestId);
}

// ------------------------------------------------- departures & navigation

async function loadDepartures() {
  if (!hasTrip()) return;
  const button = $("departures-btn");
  button.disabled = true;
  try {
    const data = await api.departures(state.access.origin ?? state.origin, state.access.destination ?? state.destination, {
      time: walkTime(),
      shade: state.shade,
      avoidStairs: state.avoidStairs,
    });
    const best = renderDepartures($("departures-list"), data, state.timezone, (time) => {
      const zoned = unixToZoned(time, state.timezone);
      state.date = zoned.date;
      state.minutes = Math.round(zoned.minutes / 15) * 15;
      state.followNow = false;
      updateTimeUi();
      onTimeChanged();
    });
    $("departures-note").textContent = best
      ? `Best: ${formatMinutes(unixToZoned(best.time, state.timezone).minutes)}. Tap a time to plan that walk.`
      : "No walk found at these times.";
    $("departures").hidden = false;
  } catch (err) {
    showToast(err.message);
  } finally {
    button.disabled = false;
  }
}

function startNavigation() {
  if (!state.route) return;
  if (!tracker.active) tracker.start();
  navigator_.start(state.route);
  $("nav-banner").hidden = false;
  $("nav-instruction").textContent = state.route.steps[0]?.instruction ?? "";
  $("nav-distance").textContent = "";
  document.body.dataset.navigating = "true";
  if (tracker.last) view.flyTo(tracker.last, 18);
}

function stopNavigation() {
  navigator_.stop();
  $("nav-banner").hidden = true;
  delete document.body.dataset.navigating;
}

function clearAll() {
  stopNavigation();
  state.route = null;
  state.requestId++;
  state.origin = null;
  state.destination = null;
  state.labels = { origin: null, destination: null };
  state.access = { origin: null, destination: null };
  view.clearRoute();
  view.removeMarker("origin");
  view.removeMarker("destination");
  setWaypointLabel("origin", null);
  setWaypointLabel("destination", null);
  renderResults(null);
  setBusy(false);
  hideToast();
  updateButtons();
  syncUrl();
}

// ------------------------------------------------------------ my location

function inCoverage(fix) {
  return !state.coverage || state.coverage.pad(0.05).contains([fix.lat, fix.lng]);
}

function handleLocation(fix) {
  view.showLocation(fix);
  view.setLocateState("active");
  if (navigator_.active) {
    navigator_.update(fix);
    view.flyTo(fix, 18);
  }
  if (state.centerOnNextFix) {
    state.centerOnNextFix = false;
    if (inCoverage(fix)) view.flyTo(fix);
    else showToast("You're outside the map area, so routes can't start from your location.");
  }
}

function locateMe() {
  state.centerOnNextFix = true;
  if (tracker.last) {
    handleLocation(tracker.last);
    return;
  }
  view.setLocateState("loading");
  tracker.start();
}

async function useMyLocationAsStart() {
  view.setLocateState("loading");
  let fix;
  try {
    fix = await tracker.current();
  } catch {
    return; // the tracker already showed an error
  }
  if (!inCoverage(fix)) {
    showToast("Your location is outside the map area.");
    return;
  }
  setWaypoint("origin", fix, "My location");
  if (!state.destination) {
    view.flyTo(fix);
    searchBoxes.destination.focus();
  }
}

/**
 * Show the blue dot whenever the app is open. The browser asks for permission
 * the first time; if the user has said no, stay quiet (the locate button still
 * explains how to allow it). The map centres on the first fix unless a shared
 * route is already on screen.
 */
async function startLocation() {
  if (!tracker.supported || !window.isSecureContext) return;
  let permission = null;
  try {
    permission = await navigator.permissions?.query({ name: "geolocation" });
  } catch {
    /* Permissions API unsupported (older Safari): just ask */
  }
  if (permission?.state === "denied") return;
  state.centerOnNextFix = !hasTrip();
  tracker.start();

  // Phones pause location updates for background apps; resume on return.
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible" && !tracker.active) tracker.start();
  });
  permission?.addEventListener?.("change", () => {
    if (permission.state === "granted" && !tracker.active) tracker.start();
  });
}

// -------------------------------------------------------------- rendering

/** Show a waypoint's name; a name picked from search wins over the snapped intersection. */
function setWaypointLabel(kind, label) {
  searchBoxes[kind].setValue(state.labels[kind] ?? label ?? "");
}

function updateButtons() {
  $("swap-btn").disabled = !hasTrip();
  $("clear-btn").disabled = !state.origin && !state.destination;
}

function renderStats(summary, totalNodes) {
  const settled = $("res-settled");
  settled.textContent = formatInteger(summary.nodesSettled);
  const total = document.createElement("span");
  total.className = "stat__sub";
  total.textContent = `of ${formatInteger(totalNodes)}`;
  settled.append(total);
  $("res-compute").textContent = formatMillis(summary.computeMs);
  $("res-segments").textContent = formatInteger(summary.segments);
}

function renderResults(route) {
  $("results").hidden = !route;
  $("directions-card").hidden = true;
  $("departures").hidden = true;
  if (!route) return;
  $("save-place").hidden = !account;
  $("nav-start").hidden = !tracker.supported;

  $("res-algorithm").textContent = ALGORITHM_NAMES[route.algorithm];
  if (route.mode === "walk") {
    renderWalkResults(route);
    return;
  }

  const { summary, comparison } = route;
  renderDriveConditions(route);
  $("res-duration").textContent = formatDuration(summary.durationS);
  $("res-distance").textContent = formatDistance(summary.distanceM);
  $("res-metric").textContent = route.metric === "time" ? "fastest route" : "shortest route";
  renderStats(summary, summary.totalNodes);

  const d = comparison.dijkstra.nodesSettled;
  const a = comparison.astar.nodesSettled;
  const max = Math.max(d, a, 1);
  $("cmp-dijkstra").textContent = formatInteger(d);
  $("cmp-astar").textContent = formatInteger(a);
  $("cmp-dijkstra-bar").style.width = `${(d / max) * 100}%`;
  $("cmp-astar-bar").style.width = `${(a / max) * 100}%`;
  const saved = percentFewer(a, d);
  $("cmp-note").textContent =
    saved > 0
      ? `A* settled ${saved}% fewer nodes than Dijkstra and found a route of the same cost.`
      : "Both algorithms explored a similar number of nodes for this trip.";
}

/** Traffic delay, weather slowdown and sun glare under the drive summary. */
function renderDriveConditions(route) {
  const { conditions, summary } = route;
  const parts = [];
  const delay = summary.durationS - summary.freeFlowDurationS;
  if (conditions.traffic) {
    parts.push(delay >= 30 ? `Traffic adds ${formatDuration(delay)}` : "Live traffic: roads are clear");
  }
  if (conditions.weather?.slowdown) parts.push(`${conditions.weather.slowdown}: speeds reduced`);
  const avoided = conditions.glareAvoided;
  if (avoided && avoided.fastestGlareS - conditions.glareS >= 30) {
    const extra = summary.durationS - avoided.fastestDurationS;
    parts.push(
      `Avoids ${formatDuration(avoided.fastestGlareS - conditions.glareS)} of driving into the sun` +
        (extra >= 30 ? ` for ${formatDuration(extra)} extra` : ""),
    );
  } else if (conditions.avoidGlare && !conditions.glare && conditions.sun.up) {
    parts.push("No low sun ahead on this route");
  }
  if (conditions.glare) parts.push(`Sun in your eyes for about ${formatDuration(conditions.glareS)}: watch for glare`);
  const box = $("drive-conditions");
  box.hidden = parts.length === 0;
  box.textContent = parts.join(" · ");
  box.classList.toggle("is-warning", conditions.glare || Boolean(conditions.weather?.slowdown));
}

function renderWalkResults(route) {
  const preferred = route.preferred.summary;
  const shortest = route.shortest.summary;
  renderSun(route.sun);
  if (state.weatherEnabled) renderWeather(route.weather);

  $("res-duration").textContent = formatDuration(preferred.durationS);
  $("res-distance").textContent = formatDistance(preferred.distanceM);
  $("res-metric").textContent = route.sun.up
    ? `${Math.round(preferred.shadePct)}% in shade`
    : "walking after sunset";
  renderStats(preferred, route.totalNodes);

  const row = (key, s) => {
    $(`shade-${key}-bar`).style.width = `${s.shadePct}%`;
    $(`shade-${key}`).textContent = `${Math.round(s.shadePct)}% · ${formatDistance(s.distanceM)}`;
  };
  row("preferred", preferred);
  row("shortest", shortest);

  const extra = preferred.distanceM - shortest.distanceM;
  const gain = Math.round(preferred.shadePct - shortest.shadePct);
  let note;
  if (!route.sun.up) note = "The sun is down, so every path is shaded. This is the shortest walk.";
  else if (route.preference !== "shortest" && route.sunStrength === 0)
    note = "Clouds block the direct sun right now, so shade makes no difference. This is the shortest walk.";
  else if (route.preference === "shortest") note = "Shortest walk, ignoring the sun. Try “Balanced” or “Most shade”.";
  else if (extra < 1) note = "The shortest walk is already the shadiest option at this time.";
  else {
    const pct = Math.round((extra / shortest.distanceM) * 100);
    note = `Adds ${formatDistance(extra)} (+${pct}%) to walk ${gain} more percentage points in the shade.`;
  }
  if (route.sun.up && route.sunStrength > 0 && route.sunStrength < 0.5) {
    note += ` Clouds weaken the sun (${Math.round(route.sunStrength * 100)}% of full strength), so the route trades less distance for shade.`;
  }
  $("shade-note").textContent = note;

  const extras = [];
  if (route.via) extras.push(`Water stop: ${route.via.name}`);
  if (route.avoidStairs) extras.push("Step-free route");
  if (route.lighting) extras.push(`${Math.round(preferred.litPct)}% on lit streets (where lighting is mapped)`);
  $("walk-conditions").hidden = extras.length === 0;
  $("walk-conditions").textContent = extras.join(" · ");
}

const MANEUVER_ROTATION = {
  straight: 0,
  "slight-right": 45,
  right: 90,
  "sharp-right": 135,
  "slight-left": -45,
  left: -90,
  "sharp-left": -135,
  uturn: 180,
};

/** Static icon markup; turn arrows are rotated with CSSOM (no inline style attribute, for the CSP). */
function maneuverIcon(type) {
  const svg = (inner) =>
    `<svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">${inner}</svg>`;
  if (type === "depart") return svg('<circle cx="12" cy="12" r="4" fill="currentColor"/>');
  if (type === "arrive") return svg('<path d="M6 21V4m0 0h10l-2 4 2 4H6"/>');
  return svg('<path d="M12 20V5m0 0-5 5m5-5 5 5"/>');
}

function renderDirections(steps) {
  const list = $("directions");
  list.replaceChildren();
  steps.forEach((step) => {
    const item = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";

    const icon = document.createElement("span");
    icon.className = `maneuver maneuver--${step.maneuver}`;
    icon.innerHTML = maneuverIcon(step.maneuver); // static markup only
    const rotation = MANEUVER_ROTATION[step.maneuver];
    if (rotation) icon.firstElementChild.style.transform = `rotate(${rotation}deg)`;

    const text = document.createElement("span");
    text.className = "directions__text";
    text.textContent = step.instruction; // street names come from map data: never innerHTML
    if (step.glare) {
      const glare = document.createElement("span");
      glare.className = "glare";
      glare.textContent = " · sun glare";
      text.append(glare);
    }

    const meta = document.createElement("span");
    meta.className = "directions__meta";
    meta.textContent = step.distanceM > 0 ? formatDistance(step.distanceM) : "";

    button.append(icon, text, meta);
    button.addEventListener("click", () => view.showStep(toLatLng(step.location)));
    item.append(button);
    list.append(item);
  });
  $("directions-card").hidden = steps.length === 0;
}

function setBusy(busy, text = "Finding route…") {
  $("map-status").hidden = !busy;
  if (busy) $("map-status-text").textContent = text;
}

let toastTimer = null;
/** kind: "error" (default) or "info". */
function showToast(message, kind = "error") {
  const toast = $("toast");
  toast.textContent = message;
  toast.classList.toggle("is-info", kind === "info");
  toast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(hideToast, 6000);
}

function hideToast() {
  $("toast").hidden = true;
}
