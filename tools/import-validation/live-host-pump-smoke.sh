#!/usr/bin/env bash
# live-host-pump-smoke.sh — assert the live host actually drives JS timers.
#
# pulp-internal #71: examples/design-tool/main.cpp was missing
# bridge->service_frame_callbacks() in its idle pump, so every imported
# app's setInterval/setTimeout queued forever. The C++ machinery was
# correct; the foot-gun was the host integration code. host_pump_lint.py
# guards against the source-level regression at PR time; this smoke
# guards against runtime breakage in any host where the pairing exists
# but the pump is broken for some other reason (run loop misconfigured,
# CVDisplayLink stalled, JS shim not installed, etc.).
#
# What it does:
#   1. Generates a tiny test script that schedules `setInterval(fn, 50)`
#      and prints "[smoke] tick N" on each fire.
#   2. Launches the live host in the background against that script.
#   3. Waits N seconds (default 2), then kills the host.
#   4. Counts "[smoke] tick" lines in the captured stdout.
#   5. PASS if count >= MIN_TICKS (default 5 — well below the 40 expected
#      from 2s / 50ms = 40, leaves slack for slow CI / cold start).
#
# Generic — every Pulp live host gains coverage by adding it to HOSTS=(...)
# below. Any future imported app (Figma/Stitch/v0/Pencil) using
# polling-state-update gets implicit protection.
#
# Usage:
#   tools/import-validation/live-host-pump-smoke.sh
#   MIN_TICKS=10 tools/import-validation/live-host-pump-smoke.sh
#   SMOKE_DURATION_MS=4000 tools/import-validation/live-host-pump-smoke.sh
#   PULP_DESIGN_TOOL=/path/to/pulp-design-tool tools/import-validation/live-host-pump-smoke.sh
#
# Exit codes:
#   0  PASS — every host drove setInterval at the expected cadence
#   1  FAIL — at least one host failed to drive setInterval
#   2  ERROR — pipeline broke (host binary missing, can't write tmp, etc.)

set -euo pipefail

PULP="${PULP_DIR:-/Users/danielraffel/Code/pulp}"
DESIGN_TOOL="${PULP_DESIGN_TOOL:-$PULP/build-release/examples/design-tool/pulp-design-tool}"
SKIA_DIR="${SKIA_DIR:-$PULP/external/skia-builder}"
SMOKE_DURATION_MS="${SMOKE_DURATION_MS:-2000}"
MIN_TICKS="${MIN_TICKS:-5}"
INTERVAL_MS="${INTERVAL_MS:-50}"
TMPDIR_SMOKE="${TMPDIR_SMOKE:-/tmp/pulp-host-pump-smoke}"

mkdir -p "$TMPDIR_SMOKE" || { echo "ERROR: cannot create $TMPDIR_SMOKE" >&2; exit 2; }

# Hosts under test. Add new live-host binaries here as they come online.
HOSTS=(
  "design-tool|$DESIGN_TOOL|--script"
)

write_smoke_script() {
  local out="$1"
  cat > "$out" <<EOF
// live-host-pump-smoke probe — see tools/import-validation/live-host-pump-smoke.sh.
// Schedules a setInterval; each fire prints a tick line that the wrapping
// shell script counts. If the host's idle pump is broken (e.g. missing
// service_frame_callbacks), the timer is scheduled but never fires, and
// the smoke fails.
(function () {
  var __n = 0;
  if (typeof setInterval !== 'function') {
    console.log('[smoke] FAIL: setInterval is not a function (typeof=' + typeof setInterval + ')');
    return;
  }
  var id = setInterval(function () {
    __n++;
    console.log('[smoke] tick ' + __n);
  }, ${INTERVAL_MS});
  console.log('[smoke] scheduled id=' + id);
})();
EOF
}

run_one_host() {
  local label="$1"
  local bin="$2"
  local script_flag="$3"

  if [ ! -x "$bin" ]; then
    echo "ERROR [$label]: host binary not found or not executable at $bin" >&2
    return 2
  fi

  local script_path="$TMPDIR_SMOKE/$label-probe.js"
  local log_path="$TMPDIR_SMOKE/$label-stdout.log"
  write_smoke_script "$script_path"

  # --no-show-window keeps the live host off-screen (no Dock icon, no GUI
  # flash on local runs / CI); --exit-after-ms drives a clean
  # request_close() so we don't rely on SIGTERM mid-frame. Together the
  # smoke runs the full per-vsync pump path with no user-visible UI.
  echo "==> $label: launching $bin $script_flag $script_path --no-show-window --exit-after-ms ${SMOKE_DURATION_MS}"
  SKIA_DIR="$SKIA_DIR" "$bin" "$script_flag" "$script_path" \
      --no-show-window --exit-after-ms "$SMOKE_DURATION_MS" \
      > "$log_path" 2>&1 &
  local pid=$!

  # Allow a short grace beyond exit-after-ms for the close path to drain.
  # If the host hangs past that, escalate to TERM/KILL so the smoke can't
  # block CI.
  local grace_ms=$((SMOKE_DURATION_MS + 1500))
  local sleep_s=$(awk "BEGIN { print ${grace_ms} / 1000 }")
  sleep "$sleep_s"
  kill "$pid" 2>/dev/null || true
  sleep 0.3
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true

  local ticks
  ticks=$(grep -c '\[smoke\] tick' "$log_path" || true)
  echo "    ticks observed: $ticks (min required: $MIN_TICKS)"

  if [ "$ticks" -ge "$MIN_TICKS" ]; then
    echo "    [$label] PASS"
    return 0
  fi

  echo "    [$label] FAIL — setInterval did not fire enough times" >&2
  echo "    --- captured stdout (head 40) ---" >&2
  head -40 "$log_path" >&2 || true
  echo "    ---" >&2
  return 1
}

overall=0
for entry in "${HOSTS[@]}"; do
  IFS='|' read -r label bin flag <<< "$entry"
  if ! run_one_host "$label" "$bin" "$flag"; then
    overall=1
  fi
done

if [ "$overall" -eq 0 ]; then
  echo "==> live-host-pump-smoke: PASS"
else
  echo "==> live-host-pump-smoke: FAIL — at least one host failed to drive timers" >&2
  echo "==> Likely cause: missing bridge->service_frame_callbacks() in the host's idle pump." >&2
  echo "==> See pulp-internal #71 / scripted_ui.cpp:67-81 / host_pump_lint.py." >&2
fi
exit "$overall"
