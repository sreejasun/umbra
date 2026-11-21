// Unit tests for the safety helpers: `node --test tests/web`
import assert from "node:assert/strict";
import { test } from "node:test";

import { cleanPhone, locationMessage, normalizePhrase, smsHref } from "../../web/js/safety.js";

test("danger phrases match regardless of case and punctuation", () => {
  assert.equal(normalizePhrase("  Umbra, HELP now!! "), "umbra help now");
  assert.ok(normalizePhrase("okay umbra help now please").includes(normalizePhrase("Umbra help now")));
  assert.ok(!normalizePhrase("umbra helps now").split(" ").includes("help"));
});

test("phone numbers are cleaned or rejected", () => {
  assert.equal(cleanPhone("+1 (480) 555-0100"), "+14805550100");
  assert.equal(cleanPhone("480.555.0100"), "4805550100");
  assert.equal(cleanPhone("911"), "911");
  assert.equal(cleanPhone(""), "");
  assert.equal(cleanPhone("12"), "", "too short");
  assert.equal(cleanPhone("call 4805550100"), "", "letters are not a number");
  assert.equal(cleanPhone("1234567890123456"), "", "longer than E.164");
});

test("sms links use the separator each platform expects", () => {
  assert.equal(smsHref("+14805550100", "Help & hurry", true), "sms:+14805550100&body=Help%20%26%20hurry");
  assert.equal(smsHref("+14805550100", "Help", false), "sms:+14805550100?body=Help");
});

test("the message carries a map link and the coordinates", () => {
  const message = locationMessage({ lat: 33.417634, lng: -111.934651, accuracy: 12.4 }, new Date(2026, 10, 20, 15, 4));
  assert.match(message, /^EMERGENCY: I need help\. My current location/);
  assert.match(message, /mlat=33\.41763&mlon=-111\.93465/);
  assert.match(message, /\(33\.41763, -111\.93465\)/);
  assert.match(message, /±12 m/);
});
