// Account screen (#/account): sign in / create account, saved places and recent
// trips (stored on the server, see docs/API.md).

import { api } from "./api.js";

const $ = (id) => document.getElementById(id);

function dateLabel(unixSeconds) {
  return new Date(unixSeconds * 1000).toLocaleDateString(undefined, { month: "short", day: "numeric" });
}

export class AccountPanel {
  /**
   * @param {object} handlers
   *   onPickPlace(place)  a saved place was chosen ({name, position: [lat, lng]})
   *   onPickTrip(trip)    a recent trip was chosen
   *   toast(message)
   */
  constructor({ onPickPlace, onPickTrip, toast }) {
    this.onPickPlace = onPickPlace;
    this.onPickTrip = onPickTrip;
    this.toast = toast;
    this.user = null;
    this.mode = "login";
    this.bind();
  }

  get signedIn() {
    return this.user !== null;
  }

  async init() {
    $("profile-card").hidden = false;
    try {
      this.user = (await api.me()).user;
    } catch {
      this.user = null;
    }
    this.render();
  }

  /** Show the account screen (sign-in form, or places and trips). */
  open() {
    window.location.hash = "#/account";
  }

  /** Called when the account screen is shown. */
  show() {
    if (this.signedIn) this.refreshLists();
  }

  bind() {
    document.querySelectorAll("[data-auth]").forEach((button) =>
      button.addEventListener("click", () => this.setMode(button.dataset.auth)),
    );
    $("auth-form").addEventListener("submit", (e) => {
      e.preventDefault();
      this.submit();
    });
    $("logout-btn").addEventListener("click", async () => {
      await api.logout().catch(() => {});
      this.user = null;
      this.render();
      this.toast("Signed out.");
    });
    $("clear-trips").addEventListener("click", async () => {
      await api.clearTrips();
      this.refreshLists();
    });
    $("delete-account").addEventListener("click", async () => {
      // A destructive, irreversible action: confirm first.
      if (!window.confirm("Delete your account, saved places and trip history? This cannot be undone.")) return;
      await api.deleteAccount();
      this.user = null;
      this.render();
      window.location.hash = "#/";
      this.toast("Your account and data were deleted.");
    });
  }

  setMode(mode) {
    this.mode = mode;
    document.querySelectorAll("[data-auth]").forEach((b) => b.setAttribute("aria-pressed", String(b.dataset.auth === mode)));
    document.querySelectorAll("[data-register-only]").forEach((el) => (el.hidden = mode !== "register"));
    $("auth-name").required = mode === "register";
    $("auth-password").autocomplete = mode === "register" ? "new-password" : "current-password";
    $("auth-submit").textContent = mode === "register" ? "Create account" : "Sign in";
    $("auth-error").hidden = true;
  }

  async submit() {
    const email = $("auth-email").value;
    const password = $("auth-password").value;
    const error = $("auth-error");
    error.hidden = true;
    $("auth-submit").disabled = true;
    try {
      const { user } =
        this.mode === "register" ? await api.register(email, password, $("auth-name").value) : await api.login(email, password);
      this.user = user;
      $("auth-password").value = "";
      this.render();
      this.refreshLists();
    } catch (err) {
      error.textContent = err.message;
      error.hidden = false;
    } finally {
      $("auth-submit").disabled = false;
    }
  }

  render() {
    $("account-label").textContent = this.user ? this.user.name : "Sign in";
    $("account-sub").textContent = this.user ? `${this.user.email} · Saved places and trips` : "Save places and see your recent trips";
    $("profile-initial").textContent = this.user ? this.user.name.trim().charAt(0).toUpperCase() : "?";
    $("account-actions").hidden = !this.user;
    const initial = $("account-initial");
    initial.textContent = this.user ? this.user.name.trim().charAt(0).toUpperCase() : "";
    initial.hidden = !this.user;
    $("account-btn").querySelector("svg").toggleAttribute("hidden", Boolean(this.user));
    $("account-btn").classList.toggle("is-signed-in", Boolean(this.user));
    $("account-signed-out").hidden = this.signedIn;
    $("account-signed-in").hidden = !this.signedIn;
    if (this.user) {
      $("account-name").textContent = this.user.name;
      $("account-email").textContent = this.user.email;
    }
    document.body.dataset.signedIn = String(this.signedIn);
  }

  async refreshLists() {
    try {
      const [{ places }, { trips }] = await Promise.all([api.places(), api.trips()]);
      this.renderList($("places-list"), places, "No saved places yet. Use “Save place” after planning a trip.", (p) => ({
        title: p.name,
        meta: "",
        pick: () => this.onPickPlace(p),
        remove: async () => {
          await api.deletePlace(p.id);
          this.refreshLists();
        },
      }));
      this.renderList($("trips-list"), trips, "Trips you plan while signed in appear here.", (t) => ({
        title: `${t.fromName} → ${t.toName}`,
        meta: `${t.mode === "walk" ? "Walk" : "Drive"} · ${dateLabel(t.createdAt)}`,
        pick: () => this.onPickTrip(t),
      }));
    } catch (err) {
      this.toast(err.message);
    }
  }

  /** Map-data and user text goes in through textContent only. */
  renderList(list, items, emptyText, describe) {
    list.replaceChildren();
    if (!items.length) {
      const empty = document.createElement("li");
      empty.className = "list__empty";
      empty.textContent = emptyText;
      list.append(empty);
      return;
    }
    for (const item of items) {
      const { title, meta, pick, remove } = describe(item);
      const li = document.createElement("li");
      li.className = "list__item";
      const button = document.createElement("button");
      button.type = "button";
      button.className = "list__main";
      const name = document.createElement("span");
      name.textContent = title;
      button.append(name);
      if (meta) {
        const small = document.createElement("span");
        small.className = "list__meta";
        small.textContent = meta;
        button.append(small);
      }
      button.addEventListener("click", pick);
      li.append(button);
      if (remove) {
        const del = document.createElement("button");
        del.type = "button";
        del.className = "icon-btn";
        del.setAttribute("aria-label", `Remove ${title}`);
        del.textContent = "×";
        del.addEventListener("click", remove);
        li.append(del);
      }
      list.append(li);
    }
  }

  async savePlace(name, position) {
    if (!this.signedIn) {
      this.open();
      return;
    }
    try {
      await api.addPlace(name, position);
      this.toast(`Saved “${name}”.`);
    } catch (err) {
      this.toast(err.message);
    }
  }

  /** Remember a planned trip (signed-in users only; failures are not worth interrupting for). */
  recordTrip(trip) {
    if (this.signedIn) api.addTrip(trip).catch(() => {});
  }
}
