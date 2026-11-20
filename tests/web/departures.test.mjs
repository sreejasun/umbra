// Unit tests for picking the best departure: `node --test tests/web`
import assert from "node:assert/strict";
import { test } from "node:test";

import { bestDeparture } from "../../web/js/departures.js";

const option = (time, comfort, shadePct, found = true) => ({ time, comfort, shadePct, found });

test("comfort beats shade, shade beats an earlier time", () => {
  assert.equal(bestDeparture([option(1, "caution", 90), option(2, "pleasant", 40)]).time, 2);
  assert.equal(bestDeparture([option(1, "pleasant", 40), option(2, "pleasant", 70)]).time, 2);
  assert.equal(bestDeparture([option(1, "pleasant", 40), option(2, "pleasant", 41)]).time, 1, "within 2 points: earlier");
});

test("ignores times without a route", () => {
  assert.equal(bestDeparture([option(1, "pleasant", 90, false)]), null);
  assert.equal(bestDeparture([option(1, null, 10), option(2, "dangerous", 99)]).time, 1);
});
