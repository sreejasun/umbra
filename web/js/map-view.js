// Leaflet map: base tiles, A/B markers, explored-node animation and the route line.
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
   * @param {{onMapClick: Function, onMarkerMoved: Function}} handlers
   */
  constructor(elementId, handlers) {
    this.handlers = handlers;
    this.map = L.map(elementId, { zoomControl: false, preferCanvas: true, zoomSnap: 0.25, zoomDelta: 0.5 });
    L.control.zoom({ position: "topright" }).addTo(this.map);
    L.control.scale({ position: "bottomright", imperial: true, metric: true }).addTo(this.map);

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

    this.addLocateControl();
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

  addLocateControl() {
    const Locate = L.Control.extend({
      options: { position: "topright" },
      onAdd: () => {
        const bar = L.DomUtil.create("div", "leaflet-bar");
        const button = L.DomUtil.create("a", "locate-btn", bar);
        button.href = "#";
        button.role = "button";
        button.title = "Show my location";
        button.setAttribute("aria-label", "Show my location");
        button.innerHTML =
          '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="3.5"/><circle cx="12" cy="12" r="8"/><path d="M12 1v3m0 16v3M1 12h3m16 0h3"/></svg>';
        L.DomEvent.disableClickPropagation(bar);
        L.DomEvent.on(button, "click", (e) => {
          L.DomEvent.preventDefault(e);
          this.handlers.onLocate?.();
        });
        this.locateButton = button;
        return bar;
      },
    });
    new Locate().addTo(this.map);
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

  drawRoute(geometry) {
    if (geometry.length < 2) return;
    const casing = L.polyline(geometry, {
      color: "#ffffff",
      weight: 9,
      opacity: 0.95,
      lineCap: "round",
      lineJoin: "round",
      interactive: false,
    });
    const line = L.polyline(geometry, {
      color: cssVar("--accent"),
      weight: 5,
      opacity: 1,
      lineCap: "round",
      lineJoin: "round",
      interactive: false,
    });
    this.routeLayer.addLayer(casing).addLayer(line);
    // Fade the search frontier so the final route reads clearly on top of it.
    this.exploredLayer.eachLayer((dot) => dot.setStyle({ fillOpacity: 0.22 }));

    const bounds = line.getBounds();
    if (!this.map.getBounds().contains(bounds)) {
      this.map.fitBounds(bounds, { padding: [60, 60], maxZoom: 16 });
    }
  }

  /** Bring a point into view (e.g. a place picked from search), zooming in if far out. */
  reveal(latlng) {
    if (this.map.getBounds().pad(-0.15).contains(latlng) && this.map.getZoom() >= 14) return;
    this.map.flyTo(latlng, Math.max(this.map.getZoom(), 15), { duration: 0.6 });
  }

  /**
   * Walking route coloured by shade, with the plain shortest route dotted underneath.
   * @param {{shaded: boolean, path: number[][]}[]} runs
   * @param {number[][] | null} shortestGeometry  null when it equals the chosen route
   */
  drawWalkRoute(runs, shortestGeometry) {
    if (shortestGeometry && shortestGeometry.length >= 2) {
      L.polyline(shortestGeometry, {
        color: "#ffffff",
        weight: 7,
        opacity: 0.85,
        lineCap: "round",
        interactive: false,
      }).addTo(this.routeLayer);
      L.polyline(shortestGeometry, {
        color: cssVar("--shortest"),
        weight: 4,
        opacity: 1,
        dashArray: "1 8",
        lineCap: "round",
        interactive: false,
      }).addTo(this.routeLayer);
    }

    const all = runs.flatMap((r) => r.path);
    if (all.length < 2) return;
    L.polyline(all, {
      color: "#ffffff",
      weight: 9,
      opacity: 0.95,
      lineCap: "round",
      lineJoin: "round",
      interactive: false,
    }).addTo(this.routeLayer);

    const shade = cssVar("--shade");
    const sun = cssVar("--sun");
    for (const run of runs) {
      L.polyline(run.path, {
        color: run.shaded ? shade : sun,
        weight: 5,
        opacity: 1,
        lineCap: "round",
        lineJoin: "round",
        interactive: false,
      }).addTo(this.routeLayer);
    }
    this.exploredLayer.eachLayer((dot) => dot.setStyle({ fillOpacity: 0.22 }));

    // Walks are short: always frame them closely so shade colours (and the
    // shadow layer, which needs zoom >= 15) are visible.
    const bounds = L.latLngBounds(all);
    if (shortestGeometry) bounds.extend(L.latLngBounds(shortestGeometry));
    this.map.fitBounds(bounds, { padding: [70, 70], maxZoom: 17 });
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
