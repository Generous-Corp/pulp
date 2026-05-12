#!/usr/bin/env bash
# semantic_probes.sh — semantic-probe vector for the Spectr import-validation
# harness.
#
# Pixel diff alone has known blind spots: a Spectr render that "looks right"
# can still have silent runtime-import failures (soft error written to
# __pulpRuntimeImportErr__) or never actually finish (no mounted/settled
# phase reached). This script complements the pixel diff with three semantic
# assertions over artifacts produced by an earlier Spectr launch.
#
# This is a codex-required addition to
# planning/spectr-validated-runtime-import-product-spec.md — pixel diff is
# necessary but not sufficient. The probes below catch failure modes the
# diff cannot:
#
#   Probe 1 (runtime-import soft error)
#       __pulpRuntimeImportErr__ is set when the C++ bridge's
#       install_runtime_import_handlers() path traps a bundle-eval failure
#       (Babel transform, payload eval, missing-host-primitive). The JS
#       shim is supposed to surface it to opts.onError, but if the host
#       app never wires up onError the render keeps going with a partially
#       initialized React tree — pixel diff might still score above
#       threshold because some chrome painted. Probe 1 fails on any
#       non-empty value.
#
#   Probe 2 (lifecycle reached the end)
#       The runtime-import path should emit `phase=mounted` (React commit
#       completed) and `phase=settled` (rAF / useEffect queues drained
#       after __pulpRuntimeSettle__). If either marker is missing the
#       render is suspect — a stale paint from a previous mount, or a
#       commit that bailed before settle. Pixel diff has no notion of
#       "did the lifecycle finish."
#
#   Probe 3 (canvas actually painted)
#       The central canvas region (Spectr's spectrum gradient + band
#       columns) must have non-trivial pixel content above the near-black
#       threshold. A blank canvas with intact chrome can pass an overall
#       histogram diff because the chrome dominates the histogram. Probe 3
#       calls diff_against_reference_regions.py (PR #1871) and inspects
#       central_canvas.blank_candidate — fails if the canvas region is
#       blank.
#
# Usage:
#   semantic_probes.sh --log <runtime.log> --screenshot <candidate.png>
#                      [--reference <reference.png>]
#                      [--regions <regions.json>]
#                      [--require-trace]
#                      [--json]
#
# Inputs:
#   --log         Runtime log from Spectr (default
#                 /tmp/spectr-rt-runtime.log, the path spectr-roundtrip.sh
#                 writes). Probes 1 + 2 read this. If the file is missing,
#                 probes 1 + 2 are reported as SKIP and probe 3 still runs.
#   --screenshot  Captured PNG of the live Spectr window (required for
#                 probe 3).
#   --reference   Ground-truth PNG to diff against. Defaults to
#                 planning/screenshots/REFERENCE-spectr-editor-html.png.
#   --regions     Optional regions JSON (defaults to the built-in Spectr
#                 layout in diff_against_reference_regions.py).
#   --require-trace
#                 Treat missing __pulpRuntimeTrace__ lines as a hard FAIL
#                 even when the log file exists. Default is to WARN — the
#                 trace globals are still being wired into Spectr's bridge
#                 (see spectr #TBD) so we don't want to block harness
#                 adoption on instrumentation that doesn't ship yet.
#   --json        Emit a single JSON object summarizing all probes
#                 (overall_passed + per-probe verdicts). Default is
#                 human-readable.
#
# Exit codes:
#   0  every probe that ran PASSED (skipped probes do not fail).
#   1  at least one probe FAILED.
#   2  invalid arguments / artifacts unusable (e.g. no screenshot at all).
#
# Constraints:
#   - Plain bash + python3 only; no extra deps beyond what the existing
#     diff scripts already require (Pillow).
#   - Must work standalone: pass it artifacts from any earlier run, no
#     Spectr launch required.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PULP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

LOG_PATH="/tmp/spectr-rt-runtime.log"
SHOT_PATH=""
REF_PATH="$PULP_ROOT/planning/screenshots/REFERENCE-spectr-editor-html.png"
REGIONS_PATH=""
REQUIRE_TRACE=0
JSON_OUT=0

usage() {
  sed -n '/^# /,/^$/p' "$0" | sed 's/^# //; s/^#$//'
  exit 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --log)        LOG_PATH="$2"; shift 2 ;;
    --screenshot) SHOT_PATH="$2"; shift 2 ;;
    --reference)  REF_PATH="$2"; shift 2 ;;
    --regions)    REGIONS_PATH="$2"; shift 2 ;;
    --require-trace) REQUIRE_TRACE=1; shift ;;
    --json)       JSON_OUT=1; shift ;;
    -h|--help)    usage ;;
    *) echo "semantic_probes.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac
done

# Style helpers — quiet down when emitting JSON so machine consumers don't
# have to strip ANSI.
if [[ $JSON_OUT -eq 1 ]]; then
  red() { :; }
  green() { :; }
  yel() { :; }
  say() { :; }
else
  red()   { printf '\033[31m%s\033[0m\n' "$*"; }
  green() { printf '\033[32m%s\033[0m\n' "$*"; }
  yel()   { printf '\033[33m%s\033[0m\n' "$*"; }
  say()   { printf '%s\n' "$*"; }
fi

# A single probe's result is captured in three parallel arrays so we can
# render either the human report or the JSON output without re-running.
PROBE_NAMES=()
PROBE_STATES=()    # pass | fail | skip
PROBE_DETAILS=()

record() {
  PROBE_NAMES+=("$1")
  PROBE_STATES+=("$2")
  PROBE_DETAILS+=("$3")
}

# ── Probe 1: __pulpRuntimeImportErr__ must be empty ───────────────────────
#
# The widget bridge writes any soft error as:
#   globalThis.__pulpRuntimeImportErr__ = "<message>";
# The Spectr-side __spectrLog hook + the bridge's set_err path both write
# diagnostic lines into the runtime log. We accept any line that mentions
# the global; non-empty quoted content fails the probe. Empty string ('' or
# "") is the explicit "all clear" value.
run_probe_1() {
  if [[ ! -f "$LOG_PATH" ]]; then
    record "runtime_import_err" "skip" "log file not found: $LOG_PATH"
    return
  fi

  # Lines that set the global to a non-empty value. The C++ side writes
  # `globalThis.__pulpRuntimeImportErr__ = "..."` (or = ''); the JS shim
  # may write `__pulpRuntimeImportErr__: <msg>` style logs via
  # __spectrLog. We accept both shapes.
  local hits
  hits=$(grep -E '__pulpRuntimeImportErr__' "$LOG_PATH" 2>/dev/null || true)

  if [[ -z "$hits" ]]; then
    # No mention at all is informational — the bridge may have run cleanly
    # without ever logging the global, or the log doesn't capture engine
    # eval prints. We pass since the explicit error path was not triggered.
    record "runtime_import_err" "pass" "no __pulpRuntimeImportErr__ entries in log (clean)"
    return
  fi

  # Filter out the clean assignments: `= ''`, `= ""`, `=''`, `=""`, and
  # the empty-string-coerced shape we use in tests. Anything left is a
  # real error payload.
  local bad
  bad=$(printf '%s\n' "$hits" \
    | grep -vE "__pulpRuntimeImportErr__[[:space:]]*=[[:space:]]*['\"]['\"]" \
    | grep -vE "__pulpRuntimeImportErr__[[:space:]]*:[[:space:]]*''" \
    | grep -vE "__pulpRuntimeImportErr__[[:space:]]*:[[:space:]]*\"\"" \
    | grep -vE "__pulpRuntimeImportErr__[[:space:]]*=[[:space:]]*''" \
    || true)

  if [[ -z "$bad" ]]; then
    record "runtime_import_err" "pass" "__pulpRuntimeImportErr__ set to empty (clean)"
  else
    # First offending line, trimmed, is the most useful artifact.
    local first
    first=$(printf '%s\n' "$bad" | head -1)
    record "runtime_import_err" "fail" "non-empty soft error: ${first:0:200}"
  fi
}

# ── Probe 2: phase=mounted AND phase=settled in trace ────────────────────
#
# Both markers MUST appear (anywhere in the log). The trace global
# (__pulpRuntimeTrace__) is the canonical channel; Spectr's bridge is
# expected to log each trace entry via __spectrLog with a `phase=<name>`
# token. The harness can also accept structured JSON entries with a
# "phase":"mounted" field.
#
# Default behavior is WARN if missing (trace wiring is still landing — see
# spectr issue tracker for status). Pass --require-trace once the trace
# globals are uniformly present.
run_probe_2() {
  if [[ ! -f "$LOG_PATH" ]]; then
    record "runtime_import_trace" "skip" "log file not found: $LOG_PATH"
    return
  fi

  local saw_mounted=0
  local saw_settled=0
  if grep -qE 'phase=mounted|"phase":[[:space:]]*"mounted"' "$LOG_PATH" 2>/dev/null; then
    saw_mounted=1
  fi
  if grep -qE 'phase=settled|"phase":[[:space:]]*"settled"' "$LOG_PATH" 2>/dev/null; then
    saw_settled=1
  fi

  if [[ $saw_mounted -eq 1 && $saw_settled -eq 1 ]]; then
    record "runtime_import_trace" "pass" "phase=mounted and phase=settled present"
    return
  fi

  local missing=""
  [[ $saw_mounted -eq 0 ]] && missing="mounted"
  [[ $saw_settled -eq 0 ]] && missing="${missing:+$missing,}settled"

  if [[ $REQUIRE_TRACE -eq 1 ]]; then
    record "runtime_import_trace" "fail" "missing trace phase(s): $missing"
  else
    # Treat as SKIP rather than FAIL while trace wiring lands. Note the
    # missing markers so the operator can see whether to start enforcing.
    record "runtime_import_trace" \
      "skip" "no $missing trace markers (advisory; pass --require-trace to enforce)"
  fi
}

# ── Probe 3: central canvas region is non-blank ──────────────────────────
#
# Uses diff_against_reference_regions.py (PR #1871). Reads its --json
# output and asserts central_canvas.blank_candidate == false. We do not
# require the region to *pass* the per-region threshold here — that's the
# pixel-diff harness's job. We only require that *something* painted into
# the canvas region, which is the failure mode a global histogram diff
# routinely launders away.
run_probe_3() {
  if [[ -z "$SHOT_PATH" ]]; then
    record "canvas_non_blank" "fail" "no --screenshot provided"
    return
  fi
  if [[ ! -f "$SHOT_PATH" ]]; then
    record "canvas_non_blank" "fail" "screenshot file not found: $SHOT_PATH"
    return
  fi
  if [[ ! -f "$REF_PATH" ]]; then
    record "canvas_non_blank" "fail" "reference file not found: $REF_PATH"
    return
  fi

  local regions_script="$SCRIPT_DIR/diff_against_reference_regions.py"
  if [[ ! -f "$regions_script" ]]; then
    # Fallback: the legacy diff_against_reference.py has its own
    # is_blank() over the whole frame. That's coarser (a blank canvas
    # plus full chrome won't trigger it) but still catches the worst
    # case — a fully empty window. Better than reporting "skip" on a
    # probe the spec calls out as a hard gate.
    local fallback="$SCRIPT_DIR/diff_against_reference.py"
    if [[ ! -f "$fallback" ]]; then
      record "canvas_non_blank" "fail" "neither diff_against_reference_regions.py nor diff_against_reference.py present"
      return
    fi
    local json
    if ! json=$(python3 "$fallback" "$REF_PATH" "$SHOT_PATH" --json 2>&1); then
      # Legacy script exits 1 on diff fail but still emits JSON, so
      # capture either way.
      :
    fi
    local blank
    blank=$(printf '%s' "$json" \
      | python3 -c 'import json,sys
try:
    d=json.loads(sys.stdin.read())
    print("true" if d.get("blank_candidate") else "false")
except Exception:
    print("error")' 2>/dev/null || echo "error")
    case "$blank" in
      false)
        record "canvas_non_blank" "pass" "fallback (whole-frame) blank check: not blank"
        ;;
      true)
        record "canvas_non_blank" "fail" "fallback (whole-frame) blank check: candidate is blank"
        ;;
      *)
        record "canvas_non_blank" "fail" "fallback diff did not emit parseable JSON"
        ;;
    esac
    return
  fi

  # Per-region path — preferred. The script may exit 1 when other regions
  # fail their threshold, but still emits valid JSON; we only inspect
  # central_canvas so we explicitly ignore the exit code.
  local args=()
  args+=("$REF_PATH" "$SHOT_PATH" --json)
  if [[ -n "$REGIONS_PATH" ]]; then
    args+=(--regions "$REGIONS_PATH")
  fi

  local out
  out=$(python3 "$regions_script" "${args[@]}" 2>&1 || true)

  local probe_state probe_detail
  read -r probe_state probe_detail < <(
    PROBE_OUT="$out" python3 <<'PY'
import json
import os
import sys

raw = os.environ.get("PROBE_OUT", "")
try:
    data = json.loads(raw)
except Exception as e:
    print(f"fail diff_against_reference_regions.py-did-not-emit-json:{type(e).__name__}")
    sys.exit(0)

regions = data.get("regions") or {}
canvas = regions.get("central_canvas")
if not canvas:
    print("fail regions-json-missing-central_canvas")
    sys.exit(0)

blank = bool(canvas.get("blank_candidate"))
score = canvas.get("score")
try:
    score = float(score)
except Exception:
    score = -1.0

if blank:
    print(f"fail central_canvas-blank_candidate=true(score={score:.3f})")
else:
    print(f"pass central_canvas-non-blank(score={score:.3f})")
PY
  )
  record "canvas_non_blank" "$probe_state" "$probe_detail"
}

run_probe_1
run_probe_2
run_probe_3

# ── Aggregate verdict ─────────────────────────────────────────────────────
overall="pass"
fail_count=0
skip_count=0
pass_count=0
for state in "${PROBE_STATES[@]}"; do
  case "$state" in
    pass) ((pass_count++)) || true ;;
    fail) ((fail_count++)) || true ;;
    skip) ((skip_count++)) || true ;;
  esac
done
if [[ $fail_count -gt 0 ]]; then
  overall="fail"
fi

if [[ $JSON_OUT -eq 1 ]]; then
  # Pass arrays to Python via base64-encoded env vars — bash command
  # substitution strips NUL bytes, so any other delimiter would break on
  # detail strings containing the delimiter. base64 always survives.
  encode_array() {
    local out=""
    local item
    for item in "$@"; do
      out+="$(printf '%s' "$item" | base64)"$'\n'
    done
    printf '%s' "$out"
  }
  PROBE_NAMES_B64=$(encode_array "${PROBE_NAMES[@]}")
  PROBE_STATES_B64=$(encode_array "${PROBE_STATES[@]}")
  PROBE_DETAILS_B64=$(encode_array "${PROBE_DETAILS[@]}")
  export PROBE_NAMES_B64 PROBE_STATES_B64 PROBE_DETAILS_B64
  export PROBE_PASS_COUNT="$pass_count"
  export PROBE_FAIL_COUNT="$fail_count"
  export PROBE_SKIP_COUNT="$skip_count"
  python3 <<'PY'
import base64
import json
import os

def decode_array(name):
    raw = os.environ.get(name, "")
    return [
        base64.b64decode(line).decode("utf-8", errors="replace")
        for line in raw.splitlines() if line.strip()
    ]

names   = decode_array("PROBE_NAMES_B64")
states  = decode_array("PROBE_STATES_B64")
details = decode_array("PROBE_DETAILS_B64")
probes = [
    {"name": n, "state": s, "detail": d}
    for n, s, d in zip(names, states, details)
]
fail_count = int(os.environ.get("PROBE_FAIL_COUNT", "0"))
print(json.dumps({
    "overall_passed": fail_count == 0,
    "summary": {
        "pass": int(os.environ.get("PROBE_PASS_COUNT", "0")),
        "fail": fail_count,
        "skip": int(os.environ.get("PROBE_SKIP_COUNT", "0")),
    },
    "probes": probes,
}, indent=2))
PY
else
  for i in "${!PROBE_NAMES[@]}"; do
    name="${PROBE_NAMES[$i]}"
    state="${PROBE_STATES[$i]}"
    detail="${PROBE_DETAILS[$i]}"
    case "$state" in
      pass) green "  ✓ ${name}  ${detail}" ;;
      fail) red   "  ✗ ${name}  ${detail}" ;;
      skip) yel   "  ⊘ ${name}  ${detail}" ;;
    esac
  done
  say ""
  if [[ $overall == "pass" ]]; then
    green "semantic-probes overall: PASS  (${pass_count} pass, ${skip_count} skip)"
  else
    red   "semantic-probes overall: FAIL  (${fail_count} fail, ${pass_count} pass, ${skip_count} skip)"
  fi
fi

if [[ $fail_count -gt 0 ]]; then
  exit 1
fi
exit 0
