// Thin wrapper around the browser Geolocation API.

const MESSAGES = {
  1: "Location permission was denied. Allow it in your browser's site settings.",
  2: "Your location is unavailable right now.",
  3: "Finding your location timed out. Please try again.",
};

export class LocationTracker {
  constructor({ onUpdate, onError }) {
    this.onUpdate = onUpdate;
    this.onError = onError;
    this.watchId = null;
    this.last = null; // {lat, lng, accuracy}
    this.lastAt = 0; // Date.now() of the last fix
  }

  get supported() {
    return "geolocation" in navigator;
  }

  get active() {
    return this.watchId !== null;
  }

  start() {
    if (!this.supported) {
      this.onError("This browser does not support location.");
      return;
    }
    if (!window.isSecureContext) {
      this.onError("Location needs a secure page (HTTPS or localhost).");
      return;
    }
    if (this.active) return;
    this.watchId = navigator.geolocation.watchPosition(
      (pos) => {
        this.last = { lat: pos.coords.latitude, lng: pos.coords.longitude, accuracy: pos.coords.accuracy };
        this.lastAt = Date.now();
        this.onUpdate(this.last);
      },
      (err) => {
        this.stop();
        this.onError(MESSAGES[err.code] ?? "Could not get your location.");
      },
      { enableHighAccuracy: true, maximumAge: 10_000, timeout: 15_000 },
    );
  }

  stop() {
    if (this.watchId !== null) navigator.geolocation.clearWatch(this.watchId);
    this.watchId = null;
  }

  /**
   * A new high-accuracy fix taken now (never a cached one), for emergencies.
   * Falls back to the last fix only if it is under `maxAgeMs` old.
   */
  fresh(maxAgeMs = 60_000, timeoutMs = 8000) {
    return new Promise((resolve, reject) => {
      const fallback = (error) => {
        if (this.last && Date.now() - this.lastAt < maxAgeMs) resolve(this.last);
        else reject(error);
      };
      if (!this.supported) {
        fallback(new Error("Location is not supported"));
        return;
      }
      navigator.geolocation.getCurrentPosition(
        (pos) => resolve({ lat: pos.coords.latitude, lng: pos.coords.longitude, accuracy: pos.coords.accuracy }),
        fallback,
        { enableHighAccuracy: true, maximumAge: 0, timeout: timeoutMs },
      );
    });
  }

  /** Resolve with the next (or latest) fix. */
  current() {
    if (this.last) return Promise.resolve(this.last);
    return new Promise((resolve, reject) => {
      const previousUpdate = this.onUpdate;
      const previousError = this.onError;
      this.onUpdate = (fix) => {
        this.onUpdate = previousUpdate;
        this.onError = previousError;
        previousUpdate(fix);
        resolve(fix);
      };
      this.onError = (message) => {
        this.onUpdate = previousUpdate;
        this.onError = previousError;
        previousError(message);
        reject(new Error(message));
      };
      this.start();
    });
  }
}
