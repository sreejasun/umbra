// Safety: a spoken danger phrase (or the SOS button) starts a cancellable
// countdown, then texts the emergency contact your location through the
// phone's own Messages app and offers a one-tap call to the contact or 911.
//
// Browser limits, by design of iOS and Android: a web page cannot send a text
// or place a call by itself (the user taps Send / Call), and it can only
// listen while it is open on screen.

const $ = (id) => document.getElementById(id);

const STORAGE_KEY = "umbra.safety";
const COUNTDOWN_S = 10;
const RESTART_DELAY_MS = 1000;

const SpeechRecognition =
  typeof window === "undefined" ? undefined : window.SpeechRecognition || window.webkitSpeechRecognition;

/** Lower-case words only, so "Umbra, help now!" matches "umbra help now". */
export function normalizePhrase(text) {
  return text
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\s]/gu, " ")
    .replace(/\s+/g, " ")
    .trim();
}

/** Digits with an optional leading +, or "" if it does not look like a phone number. */
export function cleanPhone(raw) {
  const trimmed = (raw ?? "").trim();
  const digits = trimmed.replace(/[^\d]/g, "");
  if (digits.length < 3 || digits.length > 15 || /[^\d\s()+.-]/.test(trimmed)) return "";
  return (trimmed.startsWith("+") ? "+" : "") + digits;
}

const isIOS = () => /iPad|iPhone|iPod/.test(navigator.userAgent) || (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);

/** sms: link with a body. iOS separates the body with "&", everyone else with "?". */
export function smsHref(phone, body, ios = isIOS()) {
  return `sms:${phone}${ios ? "&" : "?"}body=${encodeURIComponent(body)}`;
}

export function locationMessage(fix, now = new Date()) {
  const lat = fix.lat.toFixed(5);
  const lng = fix.lng.toFixed(5);
  const time = now.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
  return (
    `EMERGENCY: I need help. My location at ${time} (±${Math.round(fix.accuracy ?? 0)} m): ` +
    `https://www.openstreetmap.org/?mlat=${lat}&mlon=${lng}#map=18/${lat}/${lng} (${lat}, ${lng}). Sent from Umbra.`
  );
}

export class SafetyMonitor {
  /**
   * @param {object} deps
   *   getFix()   promise of the current location fix {lat, lng, accuracy}
   *   toast(message)
   */
  constructor({ getFix, toast }) {
    this.getFix = getFix;
    this.toast = toast;
    this.settings = { listen: false, phrase: "", contactName: "", contactPhone: "", call911: false };
    this.recognition = null;
    this.countdown = null;
    this.alarm = false;
    this.load();
    this.bind();
    this.render();
    if (this.settings.listen) this.startListening();
  }

  // ---------------------------------------------------------------- settings

  load() {
    try {
      Object.assign(this.settings, JSON.parse(localStorage.getItem(STORAGE_KEY) ?? "{}"));
    } catch {
      /* private mode or corrupt data: start with defaults */
    }
  }

  save() {
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(this.settings));
    } catch {
      this.toast("Safety settings could not be saved on this device (private browsing?).");
    }
  }

  bind() {
    const fields = { "safety-phrase": "phrase", "safety-contact-name": "contactName", "safety-contact-phone": "contactPhone" };
    for (const [id, key] of Object.entries(fields)) {
      $(id).value = this.settings[key];
      $(id).addEventListener("change", (e) => {
        this.settings[key] = e.target.value.trim();
        this.save();
        this.render();
      });
    }
    $("safety-911").checked = this.settings.call911;
    $("safety-911").addEventListener("change", (e) => {
      this.settings.call911 = e.target.checked;
      this.save();
    });
    $("safety-listen").checked = this.settings.listen;
    $("safety-listen").addEventListener("change", (e) => {
      if (e.target.checked && !this.canListen()) {
        e.target.checked = false;
        return;
      }
      this.settings.listen = e.target.checked;
      this.save();
      if (this.settings.listen) this.startListening();
      else this.stopListening();
    });

    $("sos-btn").addEventListener("click", () => this.trigger("SOS pressed"));
    $("emergency-cancel").addEventListener("click", () => this.cancel());
    $("emergency-close").addEventListener("click", () => this.close());
    document.addEventListener("visibilitychange", () => {
      // Browsers stop the microphone in the background; resume when visible again.
      if (document.visibilityState === "visible" && this.settings.listen && !this.alarm) this.startListening();
    });
  }

  canListen() {
    if (!SpeechRecognition) {
      this.status("This browser can't listen for a phrase. Use the SOS button instead (Chrome and Safari can listen).");
      return false;
    }
    if (normalizePhrase(this.settings.phrase).split(" ").length < 2) {
      this.status("Choose a danger phrase of at least two words first, something you would never say by accident.");
      return false;
    }
    return true;
  }

  status(text) {
    $("safety-status").textContent = text;
  }

  render() {
    const who = cleanPhone(this.settings.contactPhone) ? this.settings.contactName || "your contact" : null;
    if (this.settings.listen) {
      this.status(
        `Listening for “${this.settings.phrase}” while Umbra is open.` +
          (who ? ` ${who} will be texted your location.` : " Add a contact to text your location."),
      );
    }
  }

  // --------------------------------------------------------------- listening

  startListening() {
    if (!this.canListen() || this.recognition) return;
    const recognition = new SpeechRecognition();
    recognition.continuous = true;
    recognition.interimResults = true;
    recognition.lang = navigator.language || "en-US";
    const phrase = normalizePhrase(this.settings.phrase);

    recognition.onresult = (event) => {
      for (let i = event.resultIndex; i < event.results.length; i++) {
        if (normalizePhrase(event.results[i][0].transcript).includes(phrase)) {
          this.trigger("Danger phrase heard");
          return;
        }
      }
    };
    recognition.onerror = (event) => {
      if (event.error === "not-allowed" || event.error === "service-not-allowed") {
        this.settings.listen = false;
        $("safety-listen").checked = false;
        this.save();
        this.status("Microphone access was denied. Allow it in your browser's site settings to listen for your phrase.");
      }
    };
    // Recognition stops after silence or a time limit: keep restarting while enabled.
    recognition.onend = () => {
      this.recognition = null;
      document.body.dataset.listening = "false";
      if (this.settings.listen && !this.alarm && document.visibilityState === "visible") {
        setTimeout(() => this.startListening(), RESTART_DELAY_MS);
      }
    };
    try {
      recognition.start();
      this.recognition = recognition;
      document.body.dataset.listening = "true";
      this.render();
    } catch {
      this.status("Could not start listening. Tap the switch again.");
    }
  }

  stopListening() {
    const recognition = this.recognition;
    this.recognition = null;
    document.body.dataset.listening = "false";
    recognition?.abort();
  }

  // ------------------------------------------------------------------- alarm

  /** Start the cancellable countdown. `reason` is shown, e.g. "SOS pressed". */
  trigger(reason) {
    if (this.alarm) return;
    this.alarm = true;
    this.stopListening();
    let left = COUNTDOWN_S;
    $("emergency").hidden = false;
    $("emergency-actions").hidden = true;
    $("emergency-cancel").hidden = false;
    $("emergency-count").hidden = false;
    $("emergency-text").textContent = `${reason}. Alerting in ${left} seconds unless you cancel.`;
    $("emergency-count").textContent = String(left);
    $("emergency-cancel").focus();
    navigator.vibrate?.([300, 200, 300]);

    this.countdown = setInterval(() => {
      left -= 1;
      $("emergency-count").textContent = String(left);
      $("emergency-text").textContent = `Alerting in ${left} seconds unless you cancel.`;
      if (left <= 0) {
        clearInterval(this.countdown);
        this.fire();
      }
    }, 1000);
  }

  cancel() {
    clearInterval(this.countdown);
    this.close();
    this.toast("Emergency cancelled.");
  }

  close() {
    $("emergency").hidden = true;
    this.alarm = false;
    if (this.settings.listen) this.startListening();
  }

  /** Countdown over: text the location first, then offer the call. */
  async fire() {
    $("emergency-cancel").hidden = true;
    $("emergency-count").hidden = true;
    $("emergency-text").textContent = "Getting your location…";

    let fix = null;
    try {
      fix = await this.getFix();
    } catch {
      /* no location: still call */
    }

    const contactPhone = cleanPhone(this.settings.contactPhone);
    const contactName = this.settings.contactName || "your contact";
    const callNumber = this.settings.call911 || !contactPhone ? "911" : contactPhone;
    const call = $("emergency-call");
    call.href = `tel:${callNumber}`;
    call.textContent = callNumber === "911" ? "Call 911" : `Call ${contactName}`;

    const sms = $("emergency-sms");
    if (contactPhone && fix) {
      sms.href = smsHref(contactPhone, locationMessage(fix));
      sms.textContent = `Text my location to ${contactName}`;
      sms.hidden = false;
      $("emergency-where").textContent = `Your location: ${fix.lat.toFixed(5)}, ${fix.lng.toFixed(5)}`;
    } else {
      sms.hidden = true;
      $("emergency-where").textContent = fix
        ? `Your location: ${fix.lat.toFixed(5)}, ${fix.lng.toFixed(5)}. Tell the dispatcher.`
        : "Location unavailable. Tell the dispatcher where you are.";
    }
    $("emergency-text").textContent = contactPhone
      ? `Send the text to ${contactName}, then tap Call.`
      : "Tap Call now.";
    $("emergency-actions").hidden = false;
    call.focus();

    // Open the prefilled text right away: your location goes out even if nobody answers the call.
    if (contactPhone && fix) window.location.href = sms.href;
  }
}
