// Service worker: makes Umbra installable and lets the app shell open offline.
//
//   - App files (HTML, CSS, JS, icons): network first, so a new deploy shows up
//     on the next load, with the cached copy as an offline fallback.
//   - Leaflet and fonts from their CDNs: cache first (versioned URLs never change).
//   - /api/* and map tiles: never cached. Routes and weather must be live, and the
//     OpenStreetMap tile policy discourages bulk offline caching.

const VERSION = "umbra-v5";
const SHELL = [
  "./",
  "index.html",
  "css/app.css",
  "js/account.js",
  "js/app.js",
  "js/api.js",
  "js/assistant.js",
  "js/departures.js",
  "js/format.js",
  "js/location.js",
  "js/map-view.js",
  "js/navigation.js",
  "js/safety.js",
  "js/search-box.js",
  "js/sheet.js",
  "js/time.js",
  "favicon.svg",
  "manifest.webmanifest",
  "icons/icon.svg",
  "icons/icon-192.png",
  "icons/icon-512.png",
  "icons/apple-touch-icon.png",
];
const CDN_HOSTS = ["unpkg.com", "fonts.googleapis.com", "fonts.gstatic.com"];

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(VERSION).then((cache) => cache.addAll(SHELL)));
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((key) => key !== VERSION).map((key) => caches.delete(key))))
      .then(() => self.clients.claim()),
  );
});

async function networkFirst(request) {
  const cache = await caches.open(VERSION);
  try {
    const response = await fetch(request);
    if (response.ok) cache.put(request, response.clone());
    return response;
  } catch (err) {
    const cached = await cache.match(request, { ignoreSearch: request.mode === "navigate" });
    if (cached) return cached;
    throw err;
  }
}

async function cacheFirst(request) {
  const cache = await caches.open(VERSION);
  const cached = await cache.match(request);
  if (cached) return cached;
  const response = await fetch(request);
  if (response.ok || response.type === "opaque") cache.put(request, response.clone());
  return response;
}

self.addEventListener("fetch", (event) => {
  const { request } = event;
  if (request.method !== "GET") return;
  const url = new URL(request.url);

  if (url.origin === self.location.origin) {
    if (url.pathname.startsWith("/api/")) return; // always live
    event.respondWith(networkFirst(request));
  } else if (CDN_HOSTS.includes(url.hostname)) {
    event.respondWith(cacheFirst(request));
  }
  // Anything else (map tiles) goes straight to the network.
});
