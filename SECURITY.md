# Security

## Reporting a vulnerability

Please report security issues privately through GitHub's
[**Report a vulnerability**](../../security/advisories/new) form, not in a public issue. Include
steps to reproduce and the impact. You can expect a reply within a week.

## What Umbra protects

- **Accounts:** passwords hashed with Argon2id (libsodium); session tokens are 256-bit random values
  stored only as BLAKE2b hashes; `HttpOnly`, `SameSite=Strict` cookies, `Secure` over HTTPS; logins
  take the same time whether or not the email exists.
- **Requests:** per-IP rate limits, a strict Content-Security-Policy, CSRF protection via a required
  custom header, 64 KB body limit, read/write timeouts, parameterised SQL only.
- **Privacy:** the safety feature's phrase and emergency contact never leave the device, and the
  microphone is only used when the user turns listening on; only the map's centre and bounding box are sent to weather and traffic providers;
  access logs omit query strings; users can export or delete all their data.
- **Secrets:** API keys are read from the environment or a git-ignored `.env`, never from the
  command line, never logged, never sent to the browser.

Details: [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md#4-protecting-it) and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#security).
