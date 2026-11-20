// Accessible autocomplete combobox (WAI-ARIA 1.2 combobox pattern) backed by /api/search.

import { api } from "./api.js";

const DEBOUNCE_MS = 120;

const ICONS = {
  place:
    '<path d="M12 21s-7-6.2-7-11.5A7 7 0 0 1 19 9.5C19 14.8 12 21 12 21z"/><circle cx="12" cy="9.5" r="2.5"/>',
  building:
    '<path d="M4 21V5l8-3 8 3v16"/><path d="M9 21v-4h6v4M8 8h.01M12 8h.01M16 8h.01M8 12h.01M12 12h.01M16 12h.01"/>',
  street: '<path d="M8 3 5 21M16 3l3 18M12 4v3m0 5v3m0 5v1"/>',
  intersection: '<path d="M12 3v18M3 12h18"/><circle cx="12" cy="12" r="2.5"/>',
};

function icon(kind) {
  return `<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[kind] ?? ICONS.place}</svg>`;
}

/** Words of the query, lower-cased, for highlighting matched prefixes. */
function queryWords(query) {
  return query.toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);
}

/** Append `text` to `parent`, wrapping prefixes that match a query word in <mark>. */
function appendHighlighted(parent, text, words) {
  for (const part of text.split(/(\s+)/)) {
    const lower = part.toLowerCase().replace(/[^a-z0-9]/g, "");
    const word = words.find((w) => lower.startsWith(w));
    if (word && part.trim()) {
      // Map the matched alphanumeric prefix back onto the original characters.
      let consumed = 0;
      let cut = 0;
      while (cut < part.length && consumed < word.length) {
        if (/[a-z0-9]/i.test(part[cut])) consumed++;
        cut++;
      }
      const mark = document.createElement("mark");
      mark.textContent = part.slice(0, cut);
      parent.append(mark, part.slice(cut));
    } else {
      parent.append(part);
    }
  }
}

export class SearchBox {
  /**
   * @param {HTMLInputElement} input
   * @param {HTMLUListElement} list
   * @param {{onSelect: (result: object) => void}} handlers
   */
  constructor(input, list, handlers) {
    this.input = input;
    this.list = list;
    this.onSelect = handlers.onSelect;
    this.results = [];
    this.active = -1;
    this.requestSeq = 0;
    this.timer = null;
    this.committedValue = "";

    input.addEventListener("input", () => this.scheduleSearch());
    input.addEventListener("keydown", (e) => this.handleKey(e));
    input.addEventListener("focus", () => {
      input.select();
      if (this.results.length && input.value.trim()) this.open();
    });
    input.addEventListener("blur", () => {
      // Delay so a click on an option registers first.
      setTimeout(() => {
        this.close();
        if (!this.input.value.trim()) this.input.value = this.committedValue;
      }, 120);
    });
    list.addEventListener("mousedown", (e) => e.preventDefault()); // keep focus in the input
    list.addEventListener("click", (e) => {
      const option = e.target.closest("[role=option]");
      if (option) this.select(Number(option.dataset.index));
    });
  }

  /** Show text in the box (e.g. after a map click) without searching. */
  setValue(text) {
    this.committedValue = text ?? "";
    this.input.value = this.committedValue;
    this.results = [];
    this.close();
  }

  focus() {
    this.input.focus();
  }

  // -------------------------------------------------------------- internals

  scheduleSearch() {
    clearTimeout(this.timer);
    const query = this.input.value.trim();
    if (!query) {
      this.results = [];
      this.close();
      return;
    }
    this.timer = setTimeout(() => this.runSearch(query), DEBOUNCE_MS);
  }

  async runSearch(query) {
    const seq = ++this.requestSeq;
    let response;
    try {
      response = await api.search(query);
    } catch {
      return; // network hiccup: keep the previous suggestions
    }
    if (seq !== this.requestSeq || this.input.value.trim() !== query) return; // stale
    this.results = response.results;
    this.render(query);
  }

  render(query) {
    this.list.replaceChildren();
    this.active = -1;
    const words = queryWords(query);

    if (!this.results.length) {
      const empty = document.createElement("li");
      empty.className = "search__empty";
      empty.textContent = "No matching places, buildings, streets or intersections";
      this.list.append(empty);
      this.open();
      return;
    }

    this.results.forEach((result, index) => {
      const li = document.createElement("li");
      li.id = `${this.list.id}-opt-${index}`;
      li.className = "search__option";
      li.setAttribute("role", "option");
      li.setAttribute("aria-selected", "false");
      li.dataset.index = String(index);

      const glyph = document.createElement("span");
      glyph.className = `search__icon search__icon--${result.kind}`;
      glyph.innerHTML = icon(result.kind); // static markup only

      const text = document.createElement("span");
      text.className = "search__text";
      const name = document.createElement("span");
      name.className = "search__name";
      appendHighlighted(name, result.name, words); // map data rendered as text nodes
      const sub = document.createElement("span");
      sub.className = "search__sub";
      sub.textContent = result.subtitle;
      text.append(name, sub);

      li.append(glyph, text);
      this.list.append(li);
    });
    this.open();
  }

  handleKey(e) {
    const isOpen = !this.list.hidden && this.results.length > 0;
    switch (e.key) {
      case "ArrowDown":
        e.preventDefault();
        if (!isOpen && this.results.length) this.open();
        this.setActive(Math.min(this.active + 1, this.results.length - 1));
        break;
      case "ArrowUp":
        e.preventDefault();
        this.setActive(Math.max(this.active - 1, 0));
        break;
      case "Enter":
        if (isOpen) {
          e.preventDefault();
          this.select(this.active >= 0 ? this.active : 0);
        }
        break;
      case "Escape":
        if (!this.list.hidden) {
          e.preventDefault();
          e.stopPropagation(); // don't clear the whole trip
          this.close();
        } else {
          this.input.value = this.committedValue;
          this.input.blur();
        }
        break;
      case "Tab":
        this.close();
        break;
      default:
    }
  }

  setActive(index) {
    const options = this.list.querySelectorAll("[role=option]");
    options.forEach((o, i) => o.setAttribute("aria-selected", String(i === index)));
    this.active = index;
    if (options[index]) {
      this.input.setAttribute("aria-activedescendant", options[index].id);
      options[index].scrollIntoView({ block: "nearest" });
    }
  }

  select(index) {
    const result = this.results[index];
    if (!result) return;
    this.committedValue = result.name;
    this.input.value = result.name;
    this.close();
    this.onSelect(result);
  }

  open() {
    this.list.hidden = false;
    this.input.setAttribute("aria-expanded", "true");
  }

  close() {
    this.list.hidden = true;
    this.input.setAttribute("aria-expanded", "false");
    this.input.removeAttribute("aria-activedescendant");
    this.active = -1;
  }
}
