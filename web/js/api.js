// Thin client for the C++ server's JSON API (see docs/API.md).

export class ApiError extends Error {
  constructor(message, status) {
    super(message);
    this.name = "ApiError";
    this.status = status;
  }
}

/**
 * One request. State-changing calls send JSON with the X-Umbra-Request header,
 * which the server requires as cross-site request forgery protection.
 */
async function request(method, path, { params = {}, body } = {}) {
  const query = new URLSearchParams(params).toString();
  const url = query ? `${path}?${query}` : path;
  const headers = { Accept: "application/json" };
  if (method !== "GET") {
    headers["Content-Type"] = "application/json";
    headers["X-Umbra-Request"] = "1";
  }

  let response;
  try {
    response = await fetch(url, {
      method,
      headers,
      credentials: "same-origin",
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch {
    throw new ApiError("Could not reach the routing server. Check your connection.", 0);
  }

  let data;
  try {
    data = await response.json();
  } catch {
    throw new ApiError(`Unexpected response from server (HTTP ${response.status})`, response.status);
  }
  if (!response.ok) throw new ApiError(data.error || `Request failed (HTTP ${response.status})`, response.status);
  return data;
}

const getJson = (path, params) => request("GET", path, { params });

const point = (p) => `${p.lat.toFixed(6)},${p.lng.toFixed(6)}`;
/** One point, or several alternatives (a building's entrances) joined by ";". */
const points = (p) => (Array.isArray(p) ? p.map(point).join(";") : point(p));
const flag = (value) => (value ? "true" : "false");

/** Shared trip parameters: ends, optional names. */
function tripParams(from, to, { fromName, toName }) {
  const params = { from: points(from), to: points(to) };
  if (fromName) params.fromName = fromName;
  if (toName) params.toName = toName;
  return params;
}

function walkParams(from, to, o) {
  return {
    ...tripParams(from, to, o),
    time: o.time,
    shade: o.shade,
    algorithm: o.algorithm ?? "dijkstra",
    explored: flag(o.explored),
    avoidStairs: flag(o.avoidStairs),
    viaWater: flag(o.viaWater),
    alternatives: flag(o.alternatives),
  };
}

export const api = {
  info: () => getJson("/api/info"),
  search: (query, limit = 8) => getJson("/api/search", { q: query, limit }),
  nearest: (p) => getJson("/api/nearest", { lat: p.lat.toFixed(6), lng: p.lng.toFixed(6) }),
  sun: (time) => getJson("/api/sun", { time }),
  weather: (time) => getJson("/api/weather", { time }),
  shadows: (bounds, time) => getJson("/api/shadows", { bbox: bounds.join(","), time }),

  walk: (from, to, options) => getJson("/api/walk", walkParams(from, to, options)),

  departures: (from, to, options, { step = 900, count = 12 } = {}) =>
    getJson("/api/walk/departures", { ...walkParams(from, to, { ...options, explored: false }), step, count }),

  route: (from, to, options) =>
    getJson("/api/route", {
      ...tripParams(from, to, options),
      metric: options.metric,
      algorithm: options.algorithm,
      explored: flag(options.explored),
      time: options.time,
      avoidGlare: flag(options.avoidGlare),
      glareMaxElevation: options.glareMaxElevation ?? 20,
      alternatives: flag(options.alternatives),
    }),

  // Accounts
  me: () => getJson("/api/me"),
  register: (email, password, name) => request("POST", "/api/auth/register", { body: { email, password, name } }),
  login: (email, password) => request("POST", "/api/auth/login", { body: { email, password } }),
  logout: () => request("POST", "/api/auth/logout", { body: {} }),
  deleteAccount: () => request("DELETE", "/api/me"),
  places: () => getJson("/api/me/places"),
  addPlace: (name, p) => request("POST", "/api/me/places", { body: { name, position: [p.lat, p.lng] } }),
  deletePlace: (id) => request("DELETE", `/api/me/places/${encodeURIComponent(id)}`),
  trips: () => getJson("/api/me/trips"),
  addTrip: (trip) => request("POST", "/api/me/trips", { body: trip }),
  clearTrips: () => request("DELETE", "/api/me/trips"),
  exportUrl: "/api/me/export",

  // Assistant
  ask: (question, position) =>
    request("POST", "/api/assistant", {
      body: position ? { question, position: [position.lat, position.lng] } : { question },
    }),
};
