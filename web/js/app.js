// App controller: screens (hash routes), the map, search and route planning.
//
// Screens, each with its own URL so Back works:
//   #/                  explore: the map with a search bar
//   #/place             a place picked in search or dropped on the map
//   #/directions        route choices (2-3 alternatives) for the current trip
//   #/directions/steps  turn-by-turn list of the selected route
//   #/options           leaving time and route preferences
//   #/departures        best time to leave (walks)
//   #/navigate          live navigation
//   #/settings          profile & settings (account, safety, theme, developer mode)
//   #/account, #/safety, #/assistant (help + Ask Umbra), #/about
// The trip itself (from, to, mode, ...) lives in the query string, so links can be shared.

import { AccountPanel } from "./account.js";
import { api } from "./api.js";
import { AssistantPanel } from "./assistant.js";
import { renderDepartures } from "./departures.js";
import { formatDistance, formatDuration, formatInteger, formatMillis, formatTemperature, percentFewer } from "./format.js";
import { LocationTracker } from "./location.js";
import { MapView } from "./map-view.js";
import { Navigator } from "./navigation.js";
import { SafetyMonitor } from "./safety.js";
import { SearchBox } from "./search-box.js";
import { Sheet } from "./sheet.js";
import { formatMinutes, nowIn, unixToZoned, zonedToUnix } from "./time.js";

const $ = (id) => document.getElementById(id);
const toLatLng = ([lat, lng]) => ({ lat, lng });

const SHADOW_MIN_ZOOM = 15;
// Sun elevations below which a driver facing the sun is dazzled (see solar.hpp).
const STANDARD_GLARE_DEG = 20;
const LOW_VISOR_GLARE_DEG = 35;
const PREFS_KEY = "umbra.prefs";

const state = {
  origin: null, // {lat, lng}
  destination: null,
  labels: { origin: null, destination: null }, // names picked from search (kept over snapped labels)
  access: { origin: null, destination: null }, // a building's entrances: the engine picks the best one
  place: null, // {name, subtitle, position: {lat, lng}, access} shown on #/place
  mode: "drive", // drive | walk
  metric: "time", // drive: time | distance
  shade: "balanced", // walk: shortest | balanced | max
  algorithm: "astar",
  showExplored: true,
  showShadows: true,
  avoidStairs: false,
  viaWater: false,
  avoidGlare: false,
  lowVisor: false, // the sun visor doesn't shade the driver's eyes (short drivers)
  dev: false, // developer mode: algorithm choice, animation, statistics
  theme: "system", // system | light | dark
  timezone: "America/Phoenix",
  date: null, // departure as a wall-clock date and minute in the map's timezone
  minutes: 720,
  followNow: true, // departure tracks the current time until the user picks one
  walkingAvailable: false,
  weatherEnabled: false,
  coverage: null, // L.LatLngBounds of the map data
  response: null, // last route response
  choices: [], // route choices derived from it
  selected: 0,
  centerOnNextFix: false,
  requestId: 0, // guards against out-of-order responses
};

let account = null; // AccountPanel, when the server has accounts
let assistant = null; // AssistantPanel, when the server has the assistant

// ------------------------------------------------------------------ helpers

const timers = {};
function debounce(key, fn, ms) {
  clearTimeout(timers[key]);
  timers[key] = setTimeout(fn, ms);
}

const departureTime = () => zonedToUnix(state.date, state.minutes, state.timezone);
const hasTrip = () => Boolean(state.origin && state.destination);
const hasRoute = () => state.choices.length > 0;
const selectedRoute = () => state.choices[state.selected];

function loadPrefs() {
  try {
    const prefs = JSON.parse(localStorage.getItem(PREFS_KEY) ?? "{}");
    for (const key of ["avoidGlare", "lowVisor", "dev"]) state[key] = Boolean(prefs[key]);
    if (["light", "dark"].includes(prefs.theme)) state.theme = prefs.theme;
  } catch {
    /* storage unavailable: defaults */
  }
  $("glare-toggle").checked = state.avoidGlare;
  $("visor-toggle").checked = state.lowVisor;
  $("dev-toggle").checked = state.dev;
  document.body.dataset.dev = String(state.dev);
  selectOption("theme", state.theme);
  applyTheme();
}

/** "system" follows the device; "light" / "dark" override it (see the tokens in app.css). */
function applyTheme() {
  const root = document.documentElement;
  if (state.theme === "system") delete root.dataset.theme;
  else root.dataset.theme = state.theme;
  const dark = state.theme === "dark" || (state.theme === "system" && window.matchMedia("(prefers-color-scheme: dark)").matches);
  document.querySelectorAll('meta[name="theme-color"]').forEach((m) => m.setAttribute("content", dark ? "#0e1116" : "#f4f5f7"));
}

/** Personal settings (seat height, developer mode) are remembered on this device. */
function savePrefs() {
  try {
    const { avoidGlare, lowVisor, dev, theme } = state;
    localStorage.setItem(PREFS_KEY, JSON.stringify({ avoidGlare, lowVisor, dev, theme }));
  } catch {
    /* not critical */
  }
}

// ------------------------------------------------------------- components

const sheet = new Sheet($("sheet"), $("sheet-handle"), updateMapPadding);

const view = new MapView(
  "map",
  {
    onMapClick: handleMapClick,
    onMarkerMoved: (kind, latlng) => setWaypoint(kind, latlng),
    onViewChanged: () => debounce("shadows", refreshShadows, 250),
  },
  $("locate-btn"),
);

const searchBoxes = {
  main: new SearchBox($("search-main"), $("search-main-list"), { onSelect: openPlace }),
  origin: new SearchBox($("search-origin"), $("search-origin-list"), {
    onSelect: (result) => chooseSearchResult("origin", result),
  }),
  destination: new SearchBox($("search-destination"), $("search-destination-list"), {
    onSelect: (result) => chooseSearchResult("destination", result),
  }),
};

const navigator_ = new Navigator({
  onProgress: (step, distanceM) => {
    $("nav-distance").textContent = `In ${formatDistance(distanceM)}`;
    $("nav-instruction").textContent = step.instruction;
  },
  onReroute: (fix) => setWaypoint("origin", fix, "My location"),
  onArrive: () => {
    go("/directions");
    showToast("You have arrived.", "info");
  },
});

const tracker = new LocationTracker({
  onUpdate: handleLocation,
  onError: (message) => {
    view.setLocateState("idle");
    showToast(message);
  },
});

// ------------------------------------------------------------------ screens

/** Which header and sheet page each route shows, and when it is allowed. */
const SCREENS = {
  "/": { header: "topbar" },
  "/place": { header: "topbar", page: "place", snap: "peek", needs: () => state.place },
  "/directions": { header: "dirbar", page: "directions", snap: "half" },
  "/directions/steps": { header: "dirbar", page: "steps", snap: "full", needs: hasRoute },
  "/options": { header: "dirbar", page: "options", snap: "full" },
  "/departures": { header: "dirbar", page: "departures", snap: "full", needs: hasTrip, enter: loadDepartures },
  "/navigate": { header: "nav", page: "navigate", snap: "peek", needs: hasRoute, enter: startNavigation, leave: stopNavigation },
  "/settings": { header: "topbar", page: "settings", snap: "full" },
  "/account": { header: "topbar", page: "account", snap: "full", needs: () => account, enter: () => account.show() },
  "/safety": { header: "topbar", page: "safety", snap: "full" },
  "/assistant": { header: "topbar", page: "assistant", snap: "full", enter: () => assistant?.show() },
  "/about": { header: "topbar", page: "about", snap: "half" },
};
const TRIP_SCREENS = new Set(["/directions", "/directions/steps", "/options", "/departures", "/navigate"]);

let currentPath = null;
const pathFromHash = () => window.location.hash.replace(/^#/, "") || "/";

function go(path, { replace = false } = {}) {
  const url = `${window.location.pathname}${window.location.search}#${path}`;
  if (replace) window.history.replaceState(null, "", url);
  else window.history.pushState(null, "", url);
  showScreen();
}

function showScreen() {
  let path = pathFromHash();
  let screen = SCREENS[path];
  if (!screen || (screen.needs && !screen.needs())) {
    path = TRIP_SCREENS.has(path) && hasTrip() ? "/directions" : "/";
    window.history.replaceState(null, "", `${window.location.pathname}${window.location.search}#${path}`);
    screen = SCREENS[path];
  }
  const previous = currentPath;
  if (previous === path) return;
  currentPath = path;
  SCREENS[previous]?.leave?.();
  // Leaving the trip screens (or a place card) for the map closes them, like in Google Maps.
  if (path === "/" && (TRIP_SCREENS.has(previous) || previous === "/place")) clearTrip();

  document.body.dataset.view = path.slice(1).replace("/", "-") || "explore";
  $("topbar").hidden = screen.header !== "topbar";
  $("dirbar").hidden = screen.header !== "dirbar";
  $("nav-banner").hidden = screen.header !== "nav";
  document.querySelectorAll(".page").forEach((page) => (page.hidden = page.dataset.page !== screen.page));
  measureHeader();
  if (screen.page) sheet.show(screen.snap);
  else sheet.hide();
  screen.enter?.();
}

/** Keep --top-h equal to the visible header, which the sheet and buttons are placed under. */
function measureHeader() {
  const header = ["topbar", "dirbar", "nav-banner"].map($).find((el) => !el.hidden);
  const height = header ? header.getBoundingClientRect().height : 0;
  document.documentElement.style.setProperty("--top-h", `${Math.round(height)}px`);
  updateMapPadding();
}

function updateMapPadding() {
  const top = parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--top-h")) || 64;
  if (window.matchMedia("(min-width: 860px)").matches) {
    view.setPadding({ top: 16, left: 16 + 400 + 16, bottom: 16, right: 70 });
  } else {
    const covered = parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--sheet-offset")) || 0;
    view.setPadding({ top: top + 24, left: 12, bottom: covered + 12, right: 64 });
  }
}

// ------------------------------------------------------------------ startup

init();
registerServiceWorker();

/** Installable app + offline shell. Needs a secure origin (HTTPS or localhost). */
function registerServiceWorker() {
  if (!("serviceWorker" in navigator) || !window.isSecureContext) return;
  navigator.serviceWorker.register("sw.js").catch(() => {
    /* the app works without it, just not offline */
  });
}



async function init() {
  loadPrefs();
  bindControls();
  // Emergencies use a fix taken at that moment: never a stale one, never a trip's start or end.
  new SafetyMonitor({ getFix: () => tracker.fresh(), toast: (message) => showToast(message, "info") });
  new ResizeObserver(measureHeader).observe($("dirbar"));
  window.addEventListener("hashchange", showScreen);
  window.addEventListener("popstate", showScreen);
  try {
    const info = await api.info();
    $("region-name").textContent = info.region.name;
    state.timezone = info.region.timezone || state.timezone;
    state.coverage = L.latLngBounds(info.region.bounds);
    state.walkingAvailable = Boolean(info.walking);
    state.weatherEnabled = Boolean(info.features?.weather);
    $("water-switch").hidden = !info.walking?.waterStops;
    $("weather-credit").hidden = !state.weatherEnabled;
    $("traffic-credit").hidden = !info.features?.traffic;
    if (!state.walkingAvailable) {
      $("walk-mode-btn").disabled = true;
      $("walk-mode-btn").title = "Walking data is not loaded on this server";
    }

    setTimeToNow();
    view.showCoverage(info.region.bounds);
    restoreFromUrl();
    applyMode();
    updateTimeUi();
    refreshSun();
    startLocation();
    startOptionalFeatures(info.features ?? {});
  } catch (err) {
    showToast(err.message);
  }
  showScreen();
}

/** Accounts and the assistant exist only when the server enables them. */
function startOptionalFeatures(features) {
  if (features.accounts) {
    account = new AccountPanel({
      onPickPlace: (place) => openPlace({ name: place.name, subtitle: "Saved place", position: place.position }),
      onPickTrip: (trip) => {
        selectOption("mode", trip.mode);
        applyMode();
        state.origin = toLatLng(trip.from);
        state.labels.origin = trip.fromName;
        view.setMarker("origin", state.origin);
        setWaypoint("destination", toLatLng(trip.to), trip.toName);
        go("/directions");
      },
      toast: (message) => showToast(message, "info"),
    });
    account.init();
  }
  if (!features.assistant) {
    $("assistant-intro").textContent =
      "Use a quick link above. (The AI assistant is off on this server: it needs an XAI_API_KEY.)";
  } else {
    assistant = new AssistantPanel({
      onRoute: showAssistantRoute,
      getPosition: () => tracker.last,
      needsSignIn: () => Boolean(account) && !account.signedIn,
      openSignIn: () => go("/account"),
    });
  }
  if (currentPath === "/account" || currentPath === "/assistant") SCREENS[currentPath].enter?.();
  $("profile-card").hidden = !features.accounts;
}

function bindControls() {
  document.querySelectorAll(".segmented[data-setting]").forEach((group) => {
    group.addEventListener("click", (e) => {
      const button = e.target.closest("button[data-value]");
      if (!button || button.disabled || button.getAttribute("aria-checked") === "true") return;
      selectOption(group.dataset.setting, button.dataset.value);
      if (group.dataset.setting === "theme") {
        applyTheme();
        savePrefs();
        return;
      }
      if (group.dataset.setting === "mode") applyMode();
      if (hasTrip()) computeRoute();
      else syncUrl();
    });
  });

  const toggles = {
    "explore-toggle": "showExplored",
    "stairs-toggle": "avoidStairs",
    "water-toggle": "viaWater",
    "glare-toggle": "avoidGlare",
    "visor-toggle": "lowVisor",
  };
  for (const [id, key] of Object.entries(toggles)) {
    $(id).addEventListener("change", (e) => {
      state[key] = e.target.checked;
      savePrefs();
      if (hasTrip()) computeRoute();
    });
  }
  $("shadows-toggle").addEventListener("change", (e) => {
    state.showShadows = e.target.checked;
    refreshShadows();
  });
  $("dev-toggle").addEventListener("change", (e) => {
    state.dev = e.target.checked;
    document.body.dataset.dev = String(state.dev);
    if (!state.dev) selectOption("algorithm", "astar");
    savePrefs();
  });

  // Departure time
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

  // Place card
  $("place-directions").addEventListener("click", directionsToPlace);
  $("place-start").addEventListener("click", () => {
    const p = state.place;
    setWaypoint("origin", p.position, p.name, p.access);
    go("/directions");
    searchBoxes.destination.focus();
  });
  $("place-save").addEventListener("click", () => account?.savePlace(state.place.name, state.place.position));

  // Directions
  $("swap-btn").addEventListener("click", () => {
    [state.origin, state.destination] = [state.destination, state.origin];
    [state.labels.origin, state.labels.destination] = [state.labels.destination, state.labels.origin];
    [state.access.origin, state.access.destination] = [state.access.destination, state.access.origin];
    for (const kind of ["origin", "destination"]) {
      if (state[kind]) view.setMarker(kind, state[kind]);
      else view.removeMarker(kind);
      setWaypointLabel(kind, null);
    }
    if (hasTrip()) computeRoute();
  });
  $("use-location").addEventListener("click", useMyLocationAsStart);
  $("save-place").addEventListener("click", () => {
    if (state.destination) account?.savePlace(state.labels.destination ?? "Saved place", state.destination);
  });

  // Navigation
  $("nav-start").addEventListener("click", () => go("/navigate"));
  $("nav-stop").addEventListener("click", () => go("/directions"));
  $("nav-voice").addEventListener("click", (e) => {
    const on = e.currentTarget.getAttribute("aria-pressed") !== "true";
    navigator_.setVoice(on);
    e.currentTarget.setAttribute("aria-pressed", String(on));
    e.currentTarget.textContent = on ? "Voice on" : "Voice off";
  });

  $("locate-btn").addEventListener("click", locateMe);

  document.addEventListener("keydown", (e) => {
    const typing = e.target instanceof HTMLInputElement;
    if (e.key === "/" && !typing && currentPath === "/") {
      e.preventDefault(); // "/" focuses search, like many web apps
      searchBoxes.main.focus();
    }
    if (e.key === "Escape" && !typing && currentPath !== "/") window.history.back();
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

// -------------------------------------------------------- departure time

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
  const today = nowIn(state.timezone).date === state.date;
  const [, month, day] = state.date.split("-").map(Number);
  const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  $("time-chip").textContent = state.followNow
    ? "Leave now"
    : `${today ? "Today" : `${MONTHS[month - 1]} ${day}`}, ${formatMinutes(state.minutes)}`;
}

function onTimeChanged() {
  refreshSun();
  refreshShadows();
  if (hasTrip()) computeRoute();
  else syncUrl();
}

let sunSeq = 0;
async function refreshSun() {
  const seq = ++sunSeq;
  try {
    const sun = await api.sun(departureTime());
    if (seq === sunSeq) renderSun(sun);
  } catch {
    /* non-critical */
  }
}

function renderSun(sun) {
  $("sun-readout").classList.toggle("is-night", !sun.up);
  $("sun-text").textContent = sun.up
    ? `Sun ${Math.round(sun.elevationDeg)}° high, in the ${sun.direction}`
    : "Sun is down: every path is in shade";
}

/** One line of live weather above the route choices; hidden when there is no forecast for that time. */
function renderWeather(weather) {
  const box = $("weather-readout");
  box.hidden = !state.weatherEnabled || !weather;
  if (box.hidden) return;
  box.dataset.level = weather.comfort.level;
  const parts = [
    `${formatTemperature(weather.temperatureC)} (feels ${formatTemperature(weather.feelsLikeC)})`,
    weather.description,
  ];
  if (weather.uvIndex >= 3) parts.push(`UV ${Math.round(weather.uvIndex)}`);
  $("weather-text").textContent = parts.join(" · ");
  $("weather-badge").textContent = weather.comfort.headline;
  $("weather-advice").textContent = state.mode === "walk" ? weather.comfort.advice : "";
}

let weatherSeq = 0;
async function refreshWeather() {
  if (!state.weatherEnabled) return;
  const seq = ++weatherSeq;
  try {
    const weather = await api.weather(departureTime());
    if (seq === weatherSeq) renderWeather(weather);
  } catch {
    if (seq === weatherSeq) renderWeather(null);
  }
}

// ---------------------------------------------------------------- shadows

let shadowSeq = 0;
async function refreshShadows() {
  const note = $("map-note");
  const wanted = state.mode === "walk" && state.showShadows && state.walkingAvailable && TRIP_SCREENS.has(currentPath);
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
    const data = await api.shadows(view.viewBounds(), departureTime());
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
  selectOption("shade", params.get("shade"));
  if (state.dev) selectOption("algorithm", params.get("algorithm"));
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
  if (from) {
    state.origin = from;
    view.setMarker("origin", from);
  }
  if (to) {
    state.destination = to;
    view.setMarker("destination", to);
  }
  if (from && to) {
    if (!window.location.hash) window.history.replaceState(null, "", `${window.location.search}#/directions`);
    computeRoute();
  }
}

/** Keep the query string in sync so the trip can be shared; the hash keeps the screen. */
function syncUrl() {
  const params = new URLSearchParams();
  const fmt = (p) => `${p.lat.toFixed(5)},${p.lng.toFixed(5)}`;
  if (state.origin) params.set("from", fmt(state.origin));
  if (state.destination) params.set("to", fmt(state.destination));
  if (state.origin || state.destination) {
    params.set("mode", state.mode);
    if (state.mode === "walk") params.set("shade", state.shade);
    else params.set("metric", state.metric);
    if (!state.followNow) params.set("t", String(departureTime()));
    if (state.dev) params.set("algorithm", state.algorithm);
  }
  const query = params.toString();
  window.history.replaceState(null, "", `${window.location.pathname}${query ? `?${query}` : ""}${window.location.hash}`);
}

// ------------------------------------------------------------------ places

/** Show a search result (or a dropped pin) on its own card. */
function openPlace(result) {
  const position = Array.isArray(result.position) ? toLatLng(result.position) : result.position;
  state.place = {
    name: result.name,
    subtitle: result.subtitle ?? "",
    position,
    access: result.accessPoints?.length ? result.accessPoints.map(toLatLng) : null,
  };
  $("place-name").textContent = result.name;
  $("place-subtitle").textContent = result.subtitle ?? "";
  $("place-save").hidden = !account;
  searchBoxes.main.setValue(result.name);
  view.setMarker("destination", position);
  view.reveal(position);
  if (currentPath === "/place") sheet.show("peek");
  else go("/place");
}

/** "Directions" on a place: from my location if known, else ask for a start. */
function directionsToPlace() {
  const p = state.place;
  setWaypoint("destination", p.position, p.name, p.access);
  const fix = tracker.last;
  if (!state.origin && fix && inCoverage(fix)) {
    setWaypoint("origin", fix, "My location");
  }
  go("/directions");
  if (!state.origin) searchBoxes.origin.focus();
}

async function handleMapClick(latlng) {
  if (TRIP_SCREENS.has(currentPath) && currentPath !== "/navigate") {
    setWaypoint(state.origin ? "destination" : "origin", latlng);
    return;
  }
  if (currentPath === "/navigate") return;
  // Explore: drop a pin and show what is there.
  try {
    const snapped = await api.nearest(latlng);
    openPlace({ name: snapped.label, subtitle: "Dropped pin", position: snapped.position });
  } catch (err) {
    showToast(err.message);
  }
}

function chooseSearchResult(kind, result) {
  const point = toLatLng(result.position);
  const access = result.accessPoints?.length ? result.accessPoints.map(toLatLng) : null;
  setWaypoint(kind, point, result.name, access);
  if (!hasTrip()) view.reveal(point);
  if (kind === "origin" && !state.destination) searchBoxes.destination.focus();
}

/**
 * Set the start or destination. `label` is a name chosen in search (or "My
 * location"); map clicks and marker drags pass none, so the snapped
 * intersection is shown. `access` lists a building's entrances: the engine
 * routes to whichever is cheapest.
 */
async function setWaypoint(kind, latlng, label = null, access = null) {
  hideToast();
  state[kind] = { lat: latlng.lat, lng: latlng.lng };
  state.labels[kind] = label;
  state.access[kind] = access;
  setWaypointLabel(kind, label);
  view.setMarker(kind, latlng);
  if (hasTrip()) {
    computeRoute();
    return;
  }
  syncUrl();
  if (access || label) return;
  // Only one point so far: show which intersection was picked.
  const id = ++state.requestId;
  try {
    const snapped = await api.nearest(state[kind]);
    if (id !== state.requestId) return;
    state[kind] = toLatLng(snapped.position);
    view.setMarker(kind, state[kind]);
    setWaypointLabel(kind, snapped.label);
  } catch (err) {
    if (id !== state.requestId) return;
    state[kind] = null;
    view.removeMarker(kind);
    setWaypointLabel(kind, null);
    showToast(err.message);
  }
}

/** Show a waypoint's name; a name picked from search wins over the snapped intersection. */
function setWaypointLabel(kind, label) {
  searchBoxes[kind].setValue(state.labels[kind] ?? label ?? "");
}

function clearTrip() {
  state.requestId++;
  state.origin = null;
  state.destination = null;
  state.labels = { origin: null, destination: null };
  state.access = { origin: null, destination: null };
  state.response = null;
  state.choices = [];
  state.place = null;
  view.clearRoute();
  view.clearShadows();
  view.removeMarker("origin");
  view.removeMarker("destination");
  for (const box of Object.values(searchBoxes)) box.setValue("");
  setBusy(false);
  syncUrl();
}

// ------------------------------------------------------------------ routing

async function computeRoute() {
  const id = ++state.requestId;
  const mode = state.mode;
  view.clearRoute();
  hideToast();
  setBusy(true, mode === "walk" ? "Finding shady routes…" : "Finding routes…");
  if (currentPath !== "/navigate" && !TRIP_SCREENS.has(currentPath)) go("/directions");

  const from = state.access.origin ?? state.origin;
  const to = state.access.destination ?? state.destination;
  const common = {
    fromName: state.labels.origin,
    toName: state.labels.destination,
    algorithm: state.dev ? state.algorithm : "astar",
    explored: state.dev && state.showExplored,
    time: departureTime(),
    alternatives: true,
  };
  let route;
  try {
    route =
      mode === "walk"
        ? await api.walk(from, to, { ...common, shade: state.shade, avoidStairs: state.avoidStairs, viaWater: state.viaWater })
        : await api.route(from, to, {
            ...common,
            metric: state.metric,
            avoidGlare: state.avoidGlare,
            glareMaxElevation: state.lowVisor ? LOW_VISOR_GLARE_DEG : STANDARD_GLARE_DEG,
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

/** The route and its alternatives as uniform choices. */
function routeChoices(r) {
  if (r.mode === "walk") {
    const walk = (w, label) => ({ label, summary: w.summary, geometry: w.geometry, runs: w.runs, steps: w.steps });
    return [walk(r.preferred, r.label), ...r.alternatives.map((a) => walk(a, a.label))];
  }
  const drive = (d) => ({ label: d.label, via: d.via, summary: d.summary, geometry: d.geometry, steps: d.steps, glareS: d.conditions.glareS });
  return [drive(r), ...r.alternatives.map(drive)];
}

/** Show a route response (from planning or the assistant) on the map and in the sheet. */
async function displayRoute(route, id) {
  // Move markers onto the points the engine routed between (for a building, the entrance it picked).
  state.origin = toLatLng(route.origin.position);
  state.destination = toLatLng(route.destination.position);
  view.setMarker("origin", state.origin);
  view.setMarker("destination", state.destination);
  setWaypointLabel("origin", route.origin.label);
  setWaypointLabel("destination", route.destination.label);
  syncUrl();

  if (!route.found) {
    state.choices = [];
    renderRouteList(route);
    showToast(route.mode === "walk" ? "No walkable route connects these points." : "No drivable route connects these points.");
    return;
  }
  state.response = route;
  state.choices = routeChoices(route);
  state.selected = 0;
  renderRouteList(route);
  renderWeather(route.weather ?? null);
  if (route.mode === "drive" && state.weatherEnabled) refreshWeather();

  if (navigator_.active) {
    navigator_.start(selectedRoute()); // re-routed while navigating: no animation, no new history entry
  } else {
    account?.recordTrip({
      mode: route.mode,
      fromName: state.labels.origin ?? route.origin.label,
      from: route.origin.position,
      toName: state.labels.destination ?? route.destination.label,
      to: route.destination.position,
    });
    if (state.dev && state.showExplored && route.explored.length) {
      const completed = await view.animateExplored(route.explored);
      if (!completed || id !== state.requestId) return;
    }
  }
  drawChoices(true);
  refreshShadows();
}

function drawChoices(fit) {
  view.drawRoutes(state.choices, state.selected, selectRoute, { fit });
}

function selectRoute(index) {
  if (index === state.selected || !state.choices[index]) return;
  state.selected = index;
  drawChoices(false);
  renderRouteList(state.response);
}

/** Route cards (time, label, distance, what is special about it), the notes and the steps. */
function renderRouteList(route) {
  const list = $("route-list");
  list.replaceChildren();
  state.choices.forEach((choice, i) => {
    const li = document.createElement("li");
    const card = document.createElement("button");
    card.type = "button";
    card.className = "route";
    card.setAttribute("aria-pressed", String(i === state.selected));
    const add = (cls, text) => {
      const span = document.createElement("span");
      span.className = cls;
      span.textContent = text; // street names come from map data: never innerHTML
      card.append(span);
      return span;
    };
    add("route__time", formatDuration(choice.summary.durationS));
    add("route__label", choice.label);
    const meta = [formatDistance(choice.summary.distanceM)];
    if (choice.runs) meta.push(route.sun?.up ? `${Math.round(choice.summary.shadePct)}% in shade` : "after dark");
    else if (choice.via && !choice.label.startsWith("via ")) meta.push(`via ${choice.via}`);
    add("route__meta", meta.join(" · "));
    const extra = routeExtra(choice, route, i);
    if (extra) add(`route__extra${extra.warn ? " route__extra--warn" : ""}`, extra.text);
    card.addEventListener("click", () => selectRoute(i));
    li.append(card);
    list.append(li);
  });

  const note = routeNote(route);
  $("route-note").hidden = !note;
  $("route-note").textContent = note;
  $("save-place").hidden = !account;
  $("nav-start").hidden = !tracker.supported || !state.choices.length;
  renderStats(route);
  const chosen = selectedRoute();
  renderDirections(chosen ? chosen.steps : []);
  if (chosen) {
    $("nav-eta").textContent = formatDuration(chosen.summary.durationS);
    $("nav-summary").textContent = `${formatDistance(chosen.summary.distanceM)} · ${chosen.label}`;
  }
  if (currentPath === "/directions") sheet.show(sheet.snap);
}

/** One line about a choice: glare for drives, lighting for night walks. */
function routeExtra(choice, route, index) {
  if (route.mode === "drive") {
    if (choice.glareS >= 30) return { text: `Sun in your eyes for ${formatDuration(choice.glareS)}`, warn: true };
    const delay = choice.summary.durationS - choice.summary.freeFlowDurationS;
    if (index === 0 && route.conditions.traffic && delay >= 60) return { text: `Traffic adds ${formatDuration(delay)}`, warn: true };
    return null;
  }
  if (route.lighting && choice.summary.litPct !== undefined) return { text: `${Math.round(choice.summary.litPct)}% on lit streets` };
  return null;
}

/** Notes that apply to the whole trip. */
function routeNote(route) {
  if (!route?.found) return "";
  const parts = [];
  if (route.mode === "drive") {
    const c = route.conditions;
    const avoided = c.glareAvoided;
    if (avoided && avoided.fastestGlareS - c.glareS >= 30) {
      const extra = route.summary.durationS - avoided.fastestDurationS;
      parts.push(
        `Avoids ${formatDuration(avoided.fastestGlareS - c.glareS)} of driving into the sun` +
          (extra >= 30 ? ` for ${formatDuration(extra)} extra.` : "."),
      );
    }
    if (c.weather?.slowdown) parts.push(`${c.weather.slowdown}: speeds are reduced.`);
    if (!c.traffic && Math.abs(route.time - Date.now() / 1000) > 900) parts.push("Times assume normal traffic.");
  } else {
    if (route.via) parts.push(`Stops for water at ${route.via.name}.`);
    if (!route.sun.up) parts.push("After dark, so the route prefers lit streets.");
    else if (route.sunStrength === 0) parts.push("Clouds block the sun: shade makes no difference right now.");
    else if (route.sunStrength < 0.5) parts.push(`Clouds weaken the sun to ${Math.round(route.sunStrength * 100)}%.`);
    if (route.avoidStairs) parts.push("Step-free.");
  }
  return parts.join(" ");
}

/** Developer mode: nodes settled, timing and Dijkstra vs A*. */
function renderStats(route) {
  if (!route?.found) return;
  const summary = route.mode === "walk" ? route.preferred.summary : route.summary;
  const total = route.mode === "walk" ? route.totalNodes : summary.totalNodes;
  const settled = $("res-settled");
  settled.textContent = formatInteger(summary.nodesSettled);
  const of = document.createElement("span");
  of.className = "stat__sub";
  of.textContent = `of ${formatInteger(total)}`;
  settled.append(of);
  $("res-compute").textContent = formatMillis(summary.computeMs);
  $("res-algorithm").textContent = route.algorithm === "astar" ? "A*" : "Dijkstra";
  if (route.comparison) {
    const saved = percentFewer(route.comparison.astar.nodesSettled, route.comparison.dijkstra.nodesSettled);
    $("cmp-note").textContent =
      saved > 0
        ? `A* settled ${saved}% fewer nodes than Dijkstra (${formatInteger(route.comparison.dijkstra.nodesSettled)}) for the same route.`
        : "Both algorithms explored a similar number of nodes.";
  } else {
    $("cmp-note").textContent = "";
  }
}

/** A route planned by the assistant: switch to its mode and show it. */
function showAssistantRoute(route) {
  selectOption("mode", route.mode);
  applyMode();
  state.labels = { origin: route.origin.label, destination: route.destination.label };
  state.access = { origin: null, destination: null };
  const zoned = unixToZoned(route.time, state.timezone);
  state.date = zoned.date;
  state.minutes = zoned.minutes;
  state.followNow = false;
  updateTimeUi();
  view.clearRoute();
  go("/directions");
  displayRoute({ ...route, alternatives: route.alternatives ?? [] }, ++state.requestId);
}

// ------------------------------------------------- departures & navigation

async function loadDepartures() {
  $("departures-note").textContent = "Comparing departure times…";
  $("departures-list").replaceChildren();
  try {
    const data = await api.departures(state.access.origin ?? state.origin, state.access.destination ?? state.destination, {
      time: departureTime(),
      shade: state.shade,
      avoidStairs: state.avoidStairs,
    });
    const best = renderDepartures($("departures-list"), data, state.timezone, (time) => {
      const zoned = unixToZoned(time, state.timezone);
      state.date = zoned.date;
      state.minutes = Math.round(zoned.minutes / 15) * 15;
      state.followNow = false;
      updateTimeUi();
      go("/directions");
      onTimeChanged();
    });
    $("departures-note").textContent = best
      ? `Best: ${formatMinutes(unixToZoned(best.time, state.timezone).minutes)}. Tap a time to plan that walk.`
      : "No walk found at these times.";
  } catch (err) {
    $("departures-note").textContent = err.message;
  }
}

function startNavigation() {
  const route = selectedRoute();
  if (!route) return;
  if (!tracker.active) tracker.start();
  navigator_.start(route);
  $("nav-distance").textContent = "";
  $("nav-instruction").textContent = route.steps[0]?.instruction ?? "";
  if (tracker.last) view.follow(tracker.last);
}

function stopNavigation() {
  navigator_.stop();
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
    view.follow(fix);
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
  if (!state.destination) searchBoxes.destination.focus();
}

/**
 * Show the blue dot whenever the app is open. The browser asks for permission
 * the first time; if the user has said no, stay quiet (the locate button still
 * explains how to allow it). The map centres on the first fix unless a trip is shown.
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
    `<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">${inner}</svg>`;
  if (type === "depart") return svg('<circle cx="12" cy="12" r="4" fill="currentColor"/>');
  if (type === "arrive") return svg('<path d="M6 21V4m0 0h10l-2 4 2 4H6"/>');
  if (type === "waypoint") return svg('<path d="M12 3s6 6.5 6 11a6 6 0 0 1-12 0c0-4.5 6-11 6-11z"/>');
  return svg('<path d="M12 20V5m0 0-5 5m5-5 5 5"/>');
}

function renderDirections(steps) {
  const list = $("directions");
  list.replaceChildren();
  for (const step of steps) {
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
  }
}

function setBusy(busy, text = "Finding routes…") {
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
