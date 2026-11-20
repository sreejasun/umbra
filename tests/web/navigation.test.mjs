// Unit tests for turn-by-turn navigation: `node --test tests/web`
import assert from "node:assert/strict";
import { test } from "node:test";

import { Navigator, distanceM, distanceToLineM } from "../../web/js/navigation.js";

// A straight 300 m walk east along latitude 33.4, then a left turn.
const lat = 33.4;
const east = (m) => -111.94 + m / (111_195 * Math.cos((lat * Math.PI) / 180));
const route = {
  geometry: [[lat, east(0)], [lat, east(300)], [lat + 0.001, east(300)]],
  steps: [
    { instruction: "Head east on Main Street", maneuver: "depart", location: [lat, east(0)] },
    { instruction: "Turn left onto First Avenue", maneuver: "left", location: [lat, east(300)] },
    { instruction: "Arrive at the library", maneuver: "arrive", location: [lat + 0.001, east(300)] },
  ],
};
const fix = (m, north = 0) => ({ lat: lat + north / 111_195, lng: east(m), accuracy: 10 });

function navigator(events) {
  return new Navigator({
    onProgress: (step, d) => events.push(["progress", step.maneuver, Math.round(d)]),
    onReroute: () => events.push(["reroute"]),
    onArrive: () => events.push(["arrive"]),
  });
}

test("geometry helpers", () => {
  assert.ok(Math.abs(distanceM(fix(0), fix(300)) - 300) < 1);
  const line = route.geometry.map(([a, b]) => ({ lat: a, lng: b }));
  assert.ok(distanceToLineM(fix(150, 20), line) > 19 && distanceToLineM(fix(150, 20), line) < 21);
  assert.ok(distanceToLineM(fix(150), line) < 0.5);
});

test("reports the next turn and its distance", () => {
  const events = [];
  const nav = navigator(events);
  nav.start(route);
  nav.update(fix(100));
  assert.deepEqual(events.at(-1), ["progress", "left", 200]);
});

test("re-routes only after two fixes off the route", () => {
  const events = [];
  const nav = navigator(events);
  nav.start(route);
  nav.update(fix(100, 80));
  assert.equal(events.filter((e) => e[0] === "reroute").length, 0, "one stray fix is ignored");
  nav.update(fix(110, 80));
  assert.equal(events.filter((e) => e[0] === "reroute").length, 1);
});

test("ignores vague fixes", () => {
  const events = [];
  const nav = navigator(events);
  nav.start(route);
  nav.update({ ...fix(100, 300), accuracy: 200 });
  assert.equal(events.length, 0);
});

test("announces arrival and stops", () => {
  const events = [];
  const nav = navigator(events);
  nav.start(route);
  nav.update(fix(299));      // passes the turn
  nav.update(fix(300, 105)); // within 20 m of the destination
  assert.deepEqual(events.at(-1), ["arrive"]);
  assert.equal(nav.active, false);
});
