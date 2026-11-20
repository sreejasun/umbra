// Turn-by-turn navigation: follows the user's position along a route, speaks
// the next instruction (Web Speech API) and asks for a new route when the user
// leaves it.

import { formatDistance } from "./format.js";

const ANNOUNCE_AHEAD_M = 150; // "In 500 feet, turn left onto ..."
const ANNOUNCE_NOW_M = 30; // "Turn left onto ..."
const STEP_DONE_M = 15;
const ARRIVED_M = 20;
const OFF_ROUTE_M = 40;
const OFF_ROUTE_FIXES = 2; // consecutive fixes before re-routing
const REROUTE_COOLDOWN_MS = 15_000;
const MAX_ACCURACY_M = 50; // ignore fixes too vague to judge

const R = 6_371_008.8;
const rad = (d) => (d * Math.PI) / 180;

export function distanceM(a, b) {
  const dLat = rad(b.lat - a.lat);
  const dLng = rad(b.lng - a.lng);
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.sin(dLng / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(h));
}

/** Distance from p to the polyline, on a local flat projection (fine at city scale). */
export function distanceToLineM(p, line) {
  const kx = R * rad(1) * Math.cos(rad(p.lat));
  const ky = R * rad(1);
  let best = Infinity;
  for (let i = 0; i + 1 < line.length; i++) {
    const ax = (line[i].lng - p.lng) * kx;
    const ay = (line[i].lat - p.lat) * ky;
    const bx = (line[i + 1].lng - p.lng) * kx;
    const by = (line[i + 1].lat - p.lat) * ky;
    const dx = bx - ax;
    const dy = by - ay;
    const len2 = dx * dx + dy * dy;
    const t = len2 > 0 ? Math.max(0, Math.min(1, -(ax * dx + ay * dy) / len2)) : 0;
    best = Math.min(best, Math.hypot(ax + t * dx, ay + t * dy));
  }
  return best;
}

export class Navigator {
  /**
   * @param {object} handlers
   *   onProgress(step, distanceM)  next manoeuvre and the distance to it
   *   onReroute(fix)               the user left the route
   *   onArrive()
   */
  constructor({ onProgress, onReroute, onArrive }) {
    this.onProgress = onProgress;
    this.onReroute = onReroute;
    this.onArrive = onArrive;
    this.voice = this.voiceSupported;
    this.route = null;
  }

  get active() {
    return this.route !== null;
  }

  get voiceSupported() {
    return typeof window !== "undefined" && "speechSynthesis" in window;
  }

  /** route: { steps: [{instruction, maneuver, location: [lat, lng]}], geometry: [[lat, lng], ...] } */
  start(route) {
    const steps = route.steps.map((s) => ({ ...s, at: { lat: s.location[0], lng: s.location[1] } }));
    this.route = {
      steps,
      line: route.geometry.map(([lat, lng]) => ({ lat, lng })),
      next: steps.length > 1 ? 1 : 0, // step 0 is "depart"
      announced: new Set(),
      offRoute: 0,
    };
    this.say(steps[0]?.instruction ?? "Starting navigation");
  }

  stop() {
    this.route = null;
    if (this.voice) window.speechSynthesis.cancel();
  }

  setVoice(on) {
    this.voice = on && this.voiceSupported;
    if (!this.voice && this.voiceSupported) window.speechSynthesis.cancel();
  }

  say(text) {
    if (!this.voice || !text) return;
    window.speechSynthesis.cancel();
    window.speechSynthesis.speak(new SpeechSynthesisUtterance(text));
  }

  /** Feed every location fix while navigating. */
  update(fix) {
    const r = this.route;
    if (!r || fix.accuracy > MAX_ACCURACY_M) return;

    // Off the route? Ask for a new one, at most every 15 s.
    if (distanceToLineM(fix, r.line) > OFF_ROUTE_M) {
      r.offRoute += 1;
      const now = Date.now();
      if (r.offRoute >= OFF_ROUTE_FIXES && (!r.reroutedAt || now - r.reroutedAt > REROUTE_COOLDOWN_MS)) {
        r.reroutedAt = now;
        r.offRoute = 0;
        this.say("Re-routing");
        this.onReroute(fix);
      }
      return;
    }
    r.offRoute = 0;

    // Advance past steps the user has reached.
    while (r.next < r.steps.length - 1 && distanceM(fix, r.steps[r.next].at) < STEP_DONE_M) r.next += 1;
    const step = r.steps[r.next];
    const d = distanceM(fix, step.at);

    if (step.maneuver === "arrive" && d < ARRIVED_M) {
      this.say(step.instruction);
      this.stop();
      this.onArrive();
      return;
    }
    if (d < ANNOUNCE_NOW_M && !r.announced.has(`${r.next}:now`)) {
      r.announced.add(`${r.next}:now`);
      this.say(step.instruction);
    } else if (d < ANNOUNCE_AHEAD_M && !r.announced.has(`${r.next}:ahead`)) {
      r.announced.add(`${r.next}:ahead`);
      this.say(`In ${formatDistance(d)}, ${step.instruction.charAt(0).toLowerCase()}${step.instruction.slice(1)}`);
    }
    this.onProgress(step, d);
  }
}
