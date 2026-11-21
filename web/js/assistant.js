// Help & Ask Umbra (#/assistant): quick links to settings, plus a chat with the
// server's Grok-powered assistant, which can find settings and plan trips.

import { api } from "./api.js";

const $ = (id) => document.getElementById(id);

export class AssistantPanel {
  /**
   * @param {object} handlers
   *   onRoute(route)       the assistant planned a route: show it on the map
   *   getPosition()        the user's last location fix, or null
   *   needsSignIn()        true if the user must sign in first
   *   openSignIn()
   */
  constructor({ onRoute, getPosition, needsSignIn, openSignIn }) {
    this.onRoute = onRoute;
    this.getPosition = getPosition;
    this.needsSignIn = needsSignIn;
    this.openSignIn = openSignIn;
    this.log = $("assistant-log");
    $("assistant-form").hidden = false;
    $("assistant-form").addEventListener("submit", (e) => {
      e.preventDefault();
      this.ask();
    });
  }

  /** Called when the assistant screen is shown. */
  show() {
    $("assistant-input").focus();
  }

  add(role, text) {
    const item = document.createElement("li");
    item.className = `assistant__msg assistant__msg--${role}`;
    item.textContent = text; // model output is plain text: never innerHTML
    this.log.append(item);
    this.log.scrollTop = this.log.scrollHeight;
    return item;
  }

  async ask() {
    const input = $("assistant-input");
    const question = input.value.trim();
    if (!question) return;
    if (this.needsSignIn()) {
      this.openSignIn();
      return;
    }
    input.value = "";
    this.add("user", question);
    const pending = this.add("bot", "Thinking…");
    pending.classList.add("is-pending");
    $("assistant-send").disabled = true;
    try {
      const reply = await api.ask(question, this.getPosition());
      pending.textContent = reply.answer;
      if (reply.route?.found) this.onRoute(reply.route);
    } catch (err) {
      pending.textContent = err.message;
    } finally {
      pending.classList.remove("is-pending");
      $("assistant-send").disabled = false;
    }
  }
}
