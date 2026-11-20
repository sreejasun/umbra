# Umbra server image: a small Debian runtime with the C++ binary, web UI and Tempe data.
#
#   docker build -t umbra .
#   docker run --rm -p 8080:8080 -v umbra-data:/app/var umbra      # http://localhost:8080
#   docker run ... -e HERE_API_KEY=... -e XAI_API_KEY=... umbra    # optional live traffic / assistant
#
# Hosting platforms that set $PORT (Render, Fly.io, Cloud Run, ...) are supported.
# Put it behind HTTPS and set UMBRA_HTTPS=on: browsers only share location with
# secure pages, and iOS only installs web apps from them. See docs/DEPLOYMENT.md.

FROM debian:trixie AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build pkg-config libcurl4-openssl-dev libsqlite3-dev libsodium-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY include include
COPY src src
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUMBRA_BUILD_TESTS=OFF -DUMBRA_BUILD_TOOLS=OFF \
 && cmake --build build --target umbra_server

FROM debian:trixie-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libcurl4t64 libsqlite3-0 libsodium23 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home /app umbra
WORKDIR /app
COPY --from=build /src/build/umbra_server /app/umbra_server
COPY web web
COPY data data
ENV UMBRA_HOST=0.0.0.0 \
    UMBRA_WEB_DIR=/app/web \
    UMBRA_DATA=/app/data/tempe.osm.json \
    UMBRA_PLACES=/app/data/tempe.places.json \
    UMBRA_CAMPUS=/app/data/tempe.campus.json \
    UMBRA_WATER=/app/data/tempe.water.json \
    UMBRA_DB=/app/var/umbra.db \
    UMBRA_WALK=/app/data/tempe.walk.osm.json \
    UMBRA_SHADE=/app/data/tempe.shade.json
RUN mkdir -p /app/var && chown umbra /app/var
# Accounts live in /app/var: mount a volume so they survive restarts.
VOLUME /app/var
USER umbra
EXPOSE 8080
CMD ["/app/umbra_server"]
