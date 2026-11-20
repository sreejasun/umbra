// "Best time to leave": the same walk at several departure times.

import { formatDuration, formatTemperature } from "./format.js";
import { formatMinutes, unixToZoned } from "./time.js";

const COMFORT_RANK = { pleasant: 0, fair: 1, caution: 2, dangerous: 3 };

/** Best option: the most comfortable, then the shadiest, then the earliest. */
export function bestDeparture(options) {
  const usable = options.filter((o) => o.found);
  if (!usable.length) return null;
  const rank = (o) => COMFORT_RANK[o.comfort] ?? 1;
  return usable.reduce((best, o) =>
    rank(o) < rank(best) || (rank(o) === rank(best) && o.shadePct > best.shadePct + 2) ? o : best,
  );
}

/** Render rows: time · shade bar · feels-like · duration. `onPick(time)` when a row is chosen. */
export function renderDepartures(list, data, timezone, onPick) {
  list.replaceChildren();
  const best = bestDeparture(data.options);
  for (const option of data.options) {
    const li = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "departure";
    if (option === best) button.classList.add("is-best");
    button.dataset.comfort = option.comfort ?? "unknown";

    const time = document.createElement("span");
    time.className = "departure__time";
    time.textContent = formatMinutes(unixToZoned(option.time, timezone).minutes);

    const bar = document.createElement("span");
    bar.className = "shade-bar";
    const fill = document.createElement("span");
    fill.className = "shade-bar__fill";
    fill.style.width = `${option.sunUp ? option.shadePct : 100}%`;
    bar.append(fill);

    const detail = document.createElement("span");
    detail.className = "departure__detail";
    const shade = option.sunUp ? `${Math.round(option.shadePct)}% shade` : "after dark";
    const feels = option.feelsLikeC === null ? "" : ` · ${formatTemperature(option.feelsLikeC)}`;
    detail.textContent = `${shade}${feels} · ${formatDuration(option.durationS)}`;

    button.append(time, bar, detail);
    button.setAttribute("aria-label", `${time.textContent}: ${detail.textContent}${option === best ? " (best)" : ""}`);
    button.addEventListener("click", () => onPick(option.time));
    li.append(button);
    list.append(li);
  }
  return best;
}
