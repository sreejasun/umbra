#!/usr/bin/env bash
# Shared helper: run an Overpass QL query with retries, exponential backoff and
# mirror fallback, writing the JSON response to a file.
#
#   overpass_fetch "<query>" <output.json>
#
# Environment: OVERPASS_URL (force one endpoint), MAX_ATTEMPTS (default 4).

overpass_fetch() {
  local query="$1"
  local out="$2"
  local max_attempts="${MAX_ATTEMPTS:-4}"
  local endpoints

  if [[ -n "${OVERPASS_URL:-}" ]]; then
    endpoints=("$OVERPASS_URL")
  else
    endpoints=(
      "https://overpass-api.de/api/interpreter"
      "https://overpass.kumi.systems/api/interpreter"
      "https://overpass.private.coffee/api/interpreter"
    )
  fi

  mkdir -p "$(dirname "$out")"
  local tmp
  tmp="$(mktemp)"

  local delay=5 attempt endpoint status
  for ((attempt = 1; attempt <= max_attempts; attempt++)); do
    for endpoint in "${endpoints[@]}"; do
      echo "[attempt ${attempt}/${max_attempts}] ${endpoint} ..."
      status=$(curl --silent --show-error --location \
        --user-agent "umbra/1.0 (open-source routing demo)" \
        --max-time 300 \
        --data-urlencode "data=${query}" \
        --output "$tmp" \
        --write-out "%{http_code}" \
        "$endpoint" 2>/dev/null) || status="000"

      # A 200 can still carry an Overpass runtime error, so also require elements.
      if [[ "$status" == "200" ]] && grep -q '"type": "\(node\|way\|relation\)"' "$tmp"; then
        mv "$tmp" "$out" && chmod 644 "$out"
        # Strip whitespace (~25% smaller) when Python is available; harmless otherwise.
        if command -v python3 >/dev/null 2>&1; then
          python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); json.dump(d, open(p,"w"), separators=(",",":"), ensure_ascii=False)' "$out" || true
        fi
        echo "Saved $(du -h "$out" | cut -f1) to ${out}"
        return 0
      fi
      echo "  -> failed (HTTP ${status})" >&2
    done
    if ((attempt < max_attempts)); then
      echo "Retrying in ${delay}s ..." >&2
      sleep "$delay"
      delay=$((delay * 2))
    fi
  done

  rm -f "$tmp"
  echo "All Overpass endpoints failed. Try again later or set OVERPASS_URL." >&2
  return 1
}
