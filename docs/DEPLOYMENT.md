# Deploying and testing on a phone

Umbra is one C++ binary that serves both the JSON API and the web app. Any machine that can run a
container (or build C++) can host it.

## 1. Run it locally

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
cp .env.example .env            # optional: add API keys here
./build/release/umbra_server    # http://127.0.0.1:8080
```

Location works on `http://localhost` because browsers treat it as secure. Your phone is a different
device, so it needs HTTPS.

## 2. Try it on your phone (no deployment)

Start an HTTPS tunnel to your laptop and open the printed `https://…` URL on the phone:

```bash
./build/release/umbra_server --host 127.0.0.1 --port 8080
cloudflared tunnel --url http://localhost:8080      # or: ngrok http 8080
```

- **iPhone:** open the URL in Safari → Share → **Add to Home Screen**. Umbra then opens full-screen
  with its own icon. To debug, connect the phone by cable and use Safari on the Mac →
  Develop → *your iPhone* (no Xcode needed).
- **Android:** open the URL in Chrome → menu → **Install app**. To debug, use `chrome://inspect`
  on the laptop. With *Port forwarding* (8080 → localhost:8080) the phone can even use
  `http://localhost:8080` without a tunnel.

No Android Studio or Xcode is needed: Umbra is a web app.

### Developing the UI

The web app is served straight from `web/`: edit a file, save, refresh (C++ changes need
`cmake --build --preset release` and a server restart).

- **Phone layout in Chrome:** DevTools (Cmd+Option+I) → device toolbar (Cmd+Shift+M). For an
  iPhone 16, add a custom device of 393 × 852 at pixel ratio 3 with a mobile Safari user agent (or use
  *iPhone 14 Pro*, the same size). Fake a location in ⋮ → More tools → Sensors. While developing,
  tick *Update on reload* (Application → Service workers) and *Disable cache* (Network).
- **Android emulator** (Android Studio → Device Manager): start the server, run
  `adb reverse tcp:8080 tcp:8080`, open `http://localhost:8080` in the emulator's Chrome, set a location
  under Extended controls → Location, and debug it from `chrome://inspect`.
- **Safari:** Develop → Enter Responsive Design Mode.

Microphone, voice, texting, calling and the home-screen install need a real phone.

## 3. Host it (always on)

```bash
docker build -t umbra .
docker run -d -p 8080:8080 -v umbra-data:/app/var \
  -e UMBRA_HTTPS=on -e UMBRA_TRUST_PROXY=on \
  -e HERE_API_KEY=... -e XAI_API_KEY=... \
  umbra
```

Platforms that build from a `Dockerfile` and give you HTTPS (Render, Fly.io, Google Cloud Run,
Railway) work as is; they set `$PORT`, which Umbra honours. Put API keys in the platform's
**secret** settings, never in the repository. Mount a persistent volume at `/app/var` if you
enable accounts, or users and their saved places are lost on every restart.

| Setting | When |
|---|---|
| `UMBRA_HTTPS=on` | Always behind HTTPS: sends HSTS and marks the session cookie `Secure` |
| `UMBRA_TRUST_PROXY=on` | Behind the platform's load balancer, so rate limits see real client IPs |
| `UMBRA_RATE_LIMIT` | Requests/s per IP (default 20). Raise it if many users share one IP (campus Wi-Fi) |
| `UMBRA_DB=off` | Run without accounts (no database needed) |

## 4. Protecting it

The server already:

- limits each IP to 20 API requests/s (burst 80), sign-in attempts to 5 then 1 per 12 s, and
  assistant questions to 20/hour per account;
- sends a strict Content-Security-Policy (no inline or third-party scripts beyond Leaflet),
  `X-Frame-Options: DENY`, `nosniff`, a referrer policy, a permissions policy and HSTS;
- caps request bodies at 64 KB and drops slow clients after 10 s;
- requires `X-Umbra-Request: 1` on state-changing requests and uses `SameSite=Strict`,
  `HttpOnly` cookies (no CSRF);
- hashes passwords with Argon2id and stores only hashes of session tokens;
- runs as a non-root user in the container and logs API paths without query strings (no positions).

For a public deployment, also put it behind a CDN/WAF such as **Cloudflare** (free plan): DDoS
protection, bot filtering and TLS certificates in front of the app. Keep the host's firewall
closed except for 80/443, and enable the CodeQL and Dependabot workflows in this repository.

## 5. Capacity

Measured on an 8-core laptop (Apple clang 21, `-O3`, rate limit off, ApacheBench, 32 concurrent
clients): about **2,150 drive routes/s**, **320 shade-aware walks/s** and **13,000 searches/s**
from one process using ~40 MB of RAM for the map. An active user plans a route every 30 s or so,
so one small instance serves hundreds of simultaneous users. Live weather and traffic are fetched
once per refresh interval for everyone, so upstream usage does not grow with users. The assistant
is the expensive part: each question costs model tokens, hence its per-account limit.

To scale out, run more instances behind a load balancer: all map data is read-only. Accounts then
need a shared database; the `UserStore` interface is where PostgreSQL would replace SQLite.
