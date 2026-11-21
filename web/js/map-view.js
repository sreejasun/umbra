// Leaflet map: base tiles, A/B markers, the location dot, explored-node animation and routes.
/* global L */

// Standard OpenStreetMap tiles: free with attribution, no API key required.
// (Dark mode is applied with a CSS filter on the tile pane.)
const TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
const ATTRIBUTION =
  '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors';

const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

function pinIcon(kind) {
  const letter = kind === "origin" ? "A" : "B";
  const modifier = kind === "origin" ? "a" : "b";
  return L.divIcon({
    className: "",
    html: `<div class="pin pin--${modifier}"><span>${letter}</span></div>`,
    iconSize: [30, 30],
    iconAnchor: [15, 36],
  });
}

export class MapView {
  /**
   * @param {string} elementId
   * @param {{onMapClick: Function, onMarkerMoved: Function, onViewChanged?: Function}} handlers
   * @param {HTMLElement} locateButton  shows the location state ("loading", "active")
   */
  constructor(elementId, handlers, locateButton) {
    this.handlers = handlers;
    this.locateButton = locateButton;
    // Space covered by floating UI, so routes are framed in the visible part of the map.
    this.padding = { top: 80, right: 70, bottom: 24, left: 24 };
    this.map = L.map(elementId, { zoomControl: false, preferCanvas: true, zoomSnap: 0.25, zoomDelta: 0.5 });
    L.control.zoom({ position: "topright" }).addTo(this.map); // desktop: under the round buttons; hidden on phones

    this.tiles = L.tileLayer(TILE_URL, { attribution: ATTRIBUTION, maxZoom: 19 }).addTo(this.map);

    // Building shadows sit below routes and markers.
    this.map.createPane("shadows").style.zIndex = "350";
    this.shadowRenderer = L.canvas({ pane: "shadows", padding: 0.2 });
    this.shadowLayer = L.layerGroup().addTo(this.map);

    this.canvas = L.canvas({ padding: 0.3 });
    this.exploredLayer = L.layerGroup().addTo(this.map);
    this.routeLayer = L.layerGroup().addTo(this.map);
    this.locationLayer = L.layerGroup().addTo(this.map);
    this.markers = { origin: null, destination: null };
    this.stepMarker = null;
    this.animationToken = null;

    this.map.on("click", (e) => this.handlers.onMapClick(e.latlng));
    this.map.on("moveend", () => this.handlers.onViewChanged?.());
  }

  // ------------------------------------------------------------ view info

  get zoom() {
    return this.map.getZoom();
  }

  /** [south, west, north, east] of the visible map. */
  viewBounds() {
    const b = this.map.getBounds();
    return [b.getSouth(), b.getWest(), b.getNorth(), b.getEast()].map((v) => Number(v.toFixed(5)));
  }

  // -------------------------------------------------------- my location

  /** Pixels hidden behind floating UI on each side. */
  setPadding(padding) {
    this.padding = { ...this.padding, ...padding };
  }

  fitOptions(maxZoom) {
    const { top, right, bottom, left } = this.padding;
    return { paddingTopLeft: [left + 30, top + 30], paddingBottomRight: [right + 30, bottom + 30], maxZoom };
  }

  /** "idle" | "loading" | "active" */
  setLocateState(stateName) {
    if (!this.locateButton) return;
    this.locateButton.classList.toggle("is-loading", stateName === "loading");
    this.locateButton.classList.toggle("is-active", stateName === "active");
  }

  showLocation({ lat, lng, accuracy }) {
    this.locationLayer.clearLayers();
    L.circle([lat, lng], {
      radius: Math.min(accuracy, 500),
      color: "#1a73e8",
      weight: 1,
      opacity: 0.4,
      fillColor: "#1a73e8",
      fillOpacity: 0.1,
      interactive: false,
    }).addTo(this.locationLayer);
    L.marker([lat, lng], {
      icon: L.divIcon({ className: "", html: '<div class="me-dot"></div>', iconSize: [16, 16] }),
      interactive: false,
      keyboard: false,
      zIndexOffset: 500,
    }).addTo(this.locationLayer);
  }

  flyTo(latlng, zoom = 16) {
    this.map.flyTo(latlng, Math.max(this.map.getZoom(), zoom), { duration: 0.8 });
  }

  // ----------------------------------------------------------- shadows

  setShadows(buildings) {
    this.shadowLayer.clearLayers();
    const shadowColor = cssVar("--shade");
    for (const b of buildings) {
      if (b.shadow.length >= 3) {
        L.polygon(b.shadow, {
          renderer: this.shadowRenderer,
          stroke: false,
          fillColor: shadowColor,
          fillOpacity: 0.16,
          interactive: false,
        }).addTo(this.shadowLayer);
      }
      L.polygon(b.footprint, {
        renderer: this.shadowRenderer,
        color: "#64748b",
        weight: 0.6,
        fillColor: "#94a3b8",
        fillOpacity: 0.35,
        interactive: false,
      }).addTo(this.shadowLayer);
    }
  }

  clearShadows() {
    this.shadowLayer.clearLayers();
  }

  showCoverage(bounds) {
    const box = L.latLngBounds(bounds);
    L.rectangle(box, {
      color: cssVar("--text-subtle"),
      weight: 1.5,
      dashArray: "6 6",
      fill: false,
      interactive: false,
    }).addTo(this.map);
    this.map.invalidateSize(); // container size is final once layout has run
    this.map.fitBounds(box, { padding: [24, 24], animate: false });
    this.map.setMaxBounds(box.pad(0.6));
    this.map.setMinZoom(Math.max(10, this.map.getZoom() - 2));
  }

  setMarker(kind, latlng) {
    if (this.markers[kind]) {
      this.markers[kind].setLatLng(latlng);
      return;
    }
    const marker = L.marker(latlng, {
      icon: pinIcon(kind),
      draggable: true,
      autoPan: true,
      keyboard: true,
      title: kind === "origin" ? "Start" : "Destination",
      zIndexOffset: kind === "origin" ? 1000 : 900,
    }).addTo(this.map);
    marker.on("dragend", () => this.handlers.onMarkerMoved(kind, marker.getLatLng()));
    this.markers[kind] = marker;
  }

  removeMarker(kind) {
    if (this.markers[kind]) {
      this.markers[kind].remove();
      this.markers[kind] = null;
    }
  }

  clearRoute() {
    if (this.animationToken) this.animationToken.cancelled = true;
    this.animationToken = null;
    this.exploredLayer.clearLayers();
    this.routeLayer.clearLayers();
    this.clearStep();
  }

  /**
   * Reveal explored nodes in the order the algorithm settled them.
   * Resolves to false if cancelled by a newer request.
   */
  animateExplored(points) {
    const token = { cancelled: false };
    this.animationToken = token;
    const color = cssVar("--explored");
    const total = points.length;
    const duration = Math.min(1600, Math.max(600, 300 + total * 0.15));

    return new Promise((resolve) => {
      let drawn = 0;
      const start = performance.now();
      const frame = (now) => {
        if (token.cancelled) return resolve(false);
        const target = Math.min(total, Math.ceil((total * (now - start)) / duration));
        for (; drawn < target; drawn++) {
          L.circleMarker(points[drawn], {
            renderer: this.canvas,
            radius: 2.2,
            stroke: false,
            fillColor: color,
            fillOpacity: 0.5,
            interactive: false,
          }).addTo(this.exploredLayer);
        }
        if (drawn < total) requestAnimationFrame(frame);
        else resolve(true);
      };
      requestAnimationFrame(frame);
    });
  }

  /**
   * Draw route choices: the others in grey (click to select), the selected one
   * on top, in the accent colour or, for walks, by shade and sun.
   * @param {{geometry: number[][], runs?: {shaded: boolean, path: number[][]}[]}[]} routes
   * @param {number} selected  index into routes
   * @param {(index: number) => void} onSelect
   * @param {{fit?: boolean}} options
   */
  drawRoutes(routes, selected, onSelect, { fit = true } = {}) {
    this.routeLayer.clearLayers();
    const line = (path, style) =>
      L.polyline(path, { lineCap: "round", lineJoin: "round", interactive: false, ...style }).addTo(this.routeLayer);

    routes.forEach((route, i) => {
      if (i === selected || route.geometry.length < 2) return;
      line(route.geometry, { color: "#ffffff", weight: 9, opacity: 0.9 });
      const alt = line(route.geometry, { color: cssVar("--alt-route"), weight: 6, opacity: 1, interactive: true, className: "route-alt" });
      alt.on("click", (e) => {
        L.DomEvent.stop(e);
        onSelect(i);
      });
    });

    const chosen = routes[selected];
    if (chosen && chosen.geometry.length >= 2) {
      line(chosen.geometry, { color: "#ffffff", weight: 10, opacity: 0.95 });
      if (chosen.runs) {
        const shade = cssVar("--shade");
        const sun = cssVar("--sun");
        for (const run of chosen.runs) line(run.path, { color: run.shaded ? shade : sun, weight: 6, opacity: 1 });
      } else {
        line(chosen.geometry, { color: cssVar("--accent"), weight: 6, opacity: 1 });
      }
    }
    // Fade the search frontier so the routes read clearly on top of it.
    this.exploredLayer.eachLayer((dot) => dot.setStyle({ fillOpacity: 0.2 }));

    if (fit) {
      const bounds = L.latLngBounds(routes.flatMap((r) => r.geometry));
      // Walks are short: frame them closely so shade colours and shadows (zoom >= 15) show.
      if (bounds.isValid()) this.map.fitBounds(bounds, this.fitOptions(chosen?.runs ? 17 : 16));
    }
  }

  /**
   * Dotted lines from requested points to the route (the walk from your location to the nearest road),
   * drawn only when they are long enough to notice.
   * @param {[{lat: number, lng: number}, {lat: number, lng: number}][]} links
   */
  drawConnectors(links) {
    for (const [a, b] of links) {
      if (this.map.distance(a, b) < 10) continue;
      L.polyline([a, b], {
        color: cssVar("--text-subtle"),
        weight: 3,
        dashArray: "2 7",
        lineCap: "round",
        interactive: false,
      }).addTo(this.routeLayer);
    }
  }

  /** Bring a point into view (e.g. a place picked from search), zooming in if far out. */
  reveal(latlng) {
    if (this.map.getBounds().pad(-0.15).contains(latlng) && this.map.getZoom() >= 14) return;
    this.map.flyTo(latlng, Math.max(this.map.getZoom(), 15), { duration: 0.6 });
  }

  /** Follow the user while navigating. */
  follow(latlng) {
    this.map.setView(latlng, Math.max(this.map.getZoom(), 17), { animate: true });
  }

  showStep(latlng) {
    this.clearStep();
    this.stepMarker = L.marker(latlng, {
      icon: L.divIcon({ className: "", html: '<div class="step-dot"></div>', iconSize: [14, 14] }),
      interactive: false,
      zIndexOffset: 2000,
    }).addTo(this.map);
    if (!this.map.getBounds().pad(-0.1).contains(latlng)) this.map.panTo(latlng);
  }

  clearStep() {
    if (this.stepMarker) {
      this.stepMarker.remove();
      this.stepMarker = null;
    }
  }
}
