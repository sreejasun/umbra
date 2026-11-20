// Wall-clock time in the map's timezone (e.g. America/Phoenix) <-> Unix timestamps,
// independent of the viewer's own timezone.

function partsIn(timeZone, epochMs) {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone,
    hourCycle: "h23",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).formatToParts(new Date(epochMs));
  const get = (type) => Number(parts.find((p) => p.type === type).value);
  return {
    year: get("year"),
    month: get("month"),
    day: get("day"),
    hour: get("hour"),
    minute: get("minute"),
    second: get("second"),
  };
}

/** Offset of `timeZone` from UTC at an instant, in milliseconds. */
function offsetMs(timeZone, epochMs) {
  const p = partsIn(timeZone, epochMs);
  const asUtc = Date.UTC(p.year, p.month - 1, p.day, p.hour, p.minute, p.second);
  return asUtc - Math.floor(epochMs / 1000) * 1000;
}

/** "2026-07-15" + 900 minutes in America/Phoenix -> Unix seconds. Handles DST zones too. */
export function zonedToUnix(dateString, minutesOfDay, timeZone) {
  const [year, month, day] = dateString.split("-").map(Number);
  const wallClockAsUtc = Date.UTC(year, month - 1, day, Math.floor(minutesOfDay / 60), minutesOfDay % 60);
  let epoch = wallClockAsUtc - offsetMs(timeZone, wallClockAsUtc);
  const corrected = wallClockAsUtc - offsetMs(timeZone, epoch); // second pass settles DST edges
  if (corrected !== epoch) epoch = corrected;
  return Math.round(epoch / 1000);
}

/** Date ("YYYY-MM-DD") and minutes since midnight in `timeZone` for a Unix timestamp. */
export function unixToZoned(unixSeconds, timeZone) {
  const p = partsIn(timeZone, unixSeconds * 1000);
  const pad = (n) => String(n).padStart(2, "0");
  return { date: `${p.year}-${pad(p.month)}-${pad(p.day)}`, minutes: p.hour * 60 + p.minute };
}

/** Current date and minutes since midnight in `timeZone`. */
export function nowIn(timeZone) {
  return unixToZoned(Math.floor(Date.now() / 1000), timeZone);
}

/** 900 -> "3:00 PM" */
export function formatMinutes(minutes) {
  const h = Math.floor(minutes / 60);
  const m = minutes % 60;
  const suffix = h < 12 ? "AM" : "PM";
  return `${((h + 11) % 12) + 1}:${String(m).padStart(2, "0")} ${suffix}`;
}
