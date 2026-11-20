# Contributing

## Setup

- macOS: Xcode command-line tools, `brew install cmake ninja libsodium`
- Debian/Ubuntu: `apt install g++ cmake ninja-build pkg-config libcurl4-openssl-dev libsqlite3-dev libsodium-dev`

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug    # ASan + UBSan
npm test                                                                               # web modules (Node >= 20)
```

## Conventions

- C++20, formatted with the repository's `.clang-format`; the build must stay free of warnings
  (`-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast …`).
- The core library (`include/umbra`, `src/core`, `src/io`, `src/service`) does no network I/O:
  HTTP clients are injected so everything is testable offline.
- Every feature comes with tests (GoogleTest, fixtures in `tests/data`) and updates to `README.md`
  and `docs/`. Numbers quoted in the docs must be measured, not estimated.
- Map text is untrusted: in the UI use `textContent`, never `innerHTML`.
- Never commit secrets, `.env`, databases (`var/`) or screenshots showing a real person's location.

## Pull requests

Describe what changed and how you tested it (see the template). CI builds and tests on Linux and
macOS, with and without sanitizers, builds the Docker image and runs CodeQL.
