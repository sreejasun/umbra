// The panel that holds every screen: a draggable bottom sheet on phones
// (peek / half / full, like Apple Maps) and a floating side panel on desktop.

const SNAPS = ["peek", "half", "full"];
const DESKTOP = "(min-width: 860px)";

export class Sheet {
  /**
   * @param {HTMLElement} element
   * @param {HTMLElement} handle
   * @param {(visibleHeightPx: number) => void} onChange  space the sheet covers at the bottom of the map
   */
  constructor(element, handle, onChange) {
    this.element = element;
    this.onChange = onChange;
    this.snap = "half";
    this.desktop = window.matchMedia(DESKTOP);
    this.desktop.addEventListener("change", () => this.apply());
    window.addEventListener("resize", () => this.apply());
    this.bindDrag(handle);
  }

  get open() {
    return !this.element.hidden;
  }

  show(snap) {
    this.element.hidden = false;
    this.snap = snap;
    this.apply();
    this.element.querySelector(".sheet__body").scrollTop = 0;
  }

  hide() {
    this.element.hidden = true;
    this.apply();
  }

  /** Height in pixels for a snap point (phones only). */
  heightFor(snap) {
    const viewport = window.innerHeight;
    const top = parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--top-h")) || 64;
    if (snap === "full") return viewport - top - 24;
    if (snap === "half") return Math.round(viewport * 0.5);
    // Peek: just enough for the visible page, at most 40 % of the screen.
    const body = this.element.querySelector(".sheet__body");
    const content = body ? body.scrollHeight + 30 : 200;
    return Math.min(Math.max(content, 140), Math.round(viewport * 0.4));
  }

  apply(height) {
    const phone = !this.desktop.matches;
    let covered = 0;
    if (this.open && phone) {
      const h = height ?? this.heightFor(this.snap);
      this.element.style.setProperty("--sheet-h", `${h}px`);
      covered = h;
    }
    document.documentElement.style.setProperty("--sheet-offset", `${covered}px`);
    this.onChange(covered);
  }

  /** Drag the handle to resize; release snaps to the nearest height. Tap cycles through heights. */
  bindDrag(handle) {
    let startY = 0;
    let startH = 0;
    let moved = false;
    handle.addEventListener("pointerdown", (e) => {
      if (this.desktop.matches) return;
      startY = e.clientY;
      startH = this.element.getBoundingClientRect().height;
      moved = false;
      handle.setPointerCapture(e.pointerId);
      this.element.classList.add("is-dragging");
    });
    handle.addEventListener("pointermove", (e) => {
      if (!handle.hasPointerCapture(e.pointerId)) return;
      const dy = startY - e.clientY;
      if (Math.abs(dy) > 4) moved = true;
      this.apply(Math.max(100, Math.min(this.heightFor("full"), startH + dy)));
    });
    handle.addEventListener("pointerup", (e) => {
      if (!handle.hasPointerCapture(e.pointerId)) return;
      handle.releasePointerCapture(e.pointerId);
      this.element.classList.remove("is-dragging");
      if (!moved) {
        this.snap = SNAPS[(SNAPS.indexOf(this.snap) + 1) % SNAPS.length];
      } else {
        const h = this.element.getBoundingClientRect().height;
        this.snap = SNAPS.reduce((best, s) =>
          Math.abs(this.heightFor(s) - h) < Math.abs(this.heightFor(best) - h) ? s : best,
        );
      }
      this.apply();
    });
  }
}
