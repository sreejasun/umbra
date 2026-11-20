// Display formatting (US units: miles / feet / Fahrenheit).

const METERS_PER_MILE = 1609.344;
const FEET_PER_METER = 3.28084;

export function formatDuration(seconds) {
  if (seconds < 60) return `${Math.max(1, Math.round(seconds))} sec`;
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes} min`;
  const hours = Math.floor(minutes / 60);
  return `${hours} h ${minutes % 60} min`;
}

export function formatDistance(meters) {
  const miles = meters / METERS_PER_MILE;
  if (miles < 0.1) return `${Math.max(10, Math.round((meters * FEET_PER_METER) / 10) * 10)} ft`;
  return `${miles < 10 ? miles.toFixed(1) : Math.round(miles)} mi`;
}

export function formatInteger(n) {
  return Number(n).toLocaleString("en-US");
}

export function formatMillis(ms) {
  if (ms < 1) return `${Math.max(1, Math.round(ms * 1000))} µs`;
  return `${ms.toFixed(ms < 10 ? 2 : 1)} ms`;
}

export function percentFewer(smaller, larger) {
  if (!larger) return 0;
  return Math.round((1 - smaller / larger) * 100);
}

export function formatTemperature(celsius) {
  return `${Math.round((celsius * 9) / 5 + 32)}°F`;
}
