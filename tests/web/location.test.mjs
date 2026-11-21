// Emergency alerts must send where the user is now: `node --test tests/web`
import assert from "node:assert/strict";
import { test } from "node:test";

import { LocationTracker } from "../../web/js/location.js";

/** A fake browser geolocation that answers getCurrentPosition with `result`. */
function fakeGeolocation(result) {
  const geolocation = {
    getCurrentPosition(ok, fail, options) {
      assert.equal(options.maximumAge, 0, "never a cached position");
      assert.equal(options.enableHighAccuracy, true);
      if (result instanceof Error) fail(result);
      else ok({ coords: { latitude: result.lat, longitude: result.lng, accuracy: result.accuracy } });
    },
  };
  // Node has a read-only `navigator`; replace it for the test.
  Object.defineProperty(globalThis, "navigator", { configurable: true, value: { geolocation } });
}

test("fresh() takes a new fix instead of the last one", async () => {
  const tracker = new LocationTracker({ onUpdate() {}, onError() {} });
  tracker.last = { lat: 1, lng: 1, accuracy: 5 };
  tracker.lastAt = Date.now();
  fakeGeolocation({ lat: 33.4176, lng: -111.9346, accuracy: 12 });
  assert.deepEqual(await tracker.fresh(), { lat: 33.4176, lng: -111.9346, accuracy: 12 });
});

test("falls back to a recent fix when the GPS fails", async () => {
  const tracker = new LocationTracker({ onUpdate() {}, onError() {} });
  tracker.last = { lat: 33.41, lng: -111.93, accuracy: 20 };
  tracker.lastAt = Date.now() - 30_000;
  fakeGeolocation(new Error("timeout"));
  assert.deepEqual(await tracker.fresh(), tracker.last);
});

test("refuses a stale fix rather than send an old position", async () => {
  const tracker = new LocationTracker({ onUpdate() {}, onError() {} });
  tracker.last = { lat: 33.41, lng: -111.93, accuracy: 20 };
  tracker.lastAt = Date.now() - 5 * 60_000;
  fakeGeolocation(new Error("timeout"));
  await assert.rejects(tracker.fresh());
});
