#!/usr/bin/env bash
#
# Prove that a project built against an INSTALLED Pulp SDK gets its own trace
# spans into a captured .pftrace, and that an untraced SDK yields none.
#
# Builds test/fixtures/trace_consumer against the SDK prefix given, runs it
# (it starts and stops the session through pulp::runtime::Tracing), and then:
#
#   --expect on   the fixture must report a started and flushed session, the
#                 file must exist, and trace_processor must find the consumer's
#                 slice and instant (with its frame_index arg) in it. The control
#                 on the same query is the total slice count, which must be at
#                 least the probe count, so an unparsed file cannot pass.
#   --expect off  the fixture must report tracing unavailable and write no
#                 file, and the binary must carry no Perfetto symbols. The
#                 control on the same `nm` output is a count of pulp::runtime
#                 symbols, which must be non-zero, so an unreadable symbol table
#                 cannot pass as "no Perfetto".
#
# In both modes it prints the fixture binary's size (and a stripped copy's), so
# running it once per SDK flavor measures what tracing costs a consumer binary.
#
# Usage:
#   trace_consumer_e2e.sh --sdk <prefix> --expect on|off --work <dir>
#                         [--trace-processor <trace_processor_shell>]
#                         [--fetch-trace-processor]
#
# --fetch-trace-processor downloads the pinned trace_processor_shell named in
# tools/packages/tool-registry.json into the `pulp trace fetch` cache and checks
# its SHA-256, for a machine (a CI runner) that has no pulp CLI to fetch it.
#
# Exit: 0 pass, 1 the expectation failed, 2 usage or a missing instrument
# (no trace_processor for --expect on). A missing instrument is never a pass.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
fixture="$repo/test/fixtures/trace_consumer"

sdk="" expect="" work="" tp="" fetch_tp=0
while [ $# -gt 0 ]; do
  case "$1" in
    --sdk) sdk="$2"; shift 2 ;;
    --expect) expect="$2"; shift 2 ;;
    --work) work="$2"; shift 2 ;;
    --trace-processor) tp="$2"; shift 2 ;;
    --fetch-trace-processor) fetch_tp=1; shift ;;
    -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
    *) echo "trace_consumer_e2e: unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [ -z "$sdk" ] || [ -z "$work" ] || { [ "$expect" != on ] && [ "$expect" != off ]; }; then
  echo "usage: trace_consumer_e2e.sh --sdk <prefix> --expect on|off --work <dir>" >&2
  exit 2
fi
if [ ! -f "$sdk/lib/cmake/Pulp/PulpConfig.cmake" ]; then
  echo "trace_consumer_e2e: no PulpConfig.cmake under $sdk/lib/cmake/Pulp" >&2
  exit 2
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

# Locate trace_processor before building anything, so a missing instrument is
# reported as such rather than after a build.
if [ "$expect" = on ] && [ -z "$tp" ]; then
  home="${PULP_HOME:-$HOME/.pulp}"
  for candidate in "$home"/tools/trace-processor/*/*/trace_processor_shell \
                   "$home"/tools/trace-processor/*/*/trace_processor_shell.exe; do
    [ -x "$candidate" ] && { tp="$candidate"; break; }
  done
  if [ -z "$tp" ] && command -v trace_processor_shell >/dev/null 2>&1; then
    tp="$(command -v trace_processor_shell)"
  fi
fi
if [ "$expect" = on ] && [ -z "$tp" ] && [ "$fetch_tp" = 1 ]; then
  # Same pins and cache layout as `pulp trace fetch`, read from the tool
  # registry so this script carries no second copy of a SHA-256.
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) reg_key=macOS-arm64 cache_key=mac-arm64 ;;
    Darwin-x86_64) reg_key=macOS-x64 cache_key=mac-amd64 ;;
    Linux-x86_64) reg_key=Linux-x64 cache_key=linux-amd64 ;;
    Linux-aarch64|Linux-arm64) reg_key=Linux-arm64 cache_key=linux-arm64 ;;
    *) echo "trace_consumer_e2e: no pinned trace_processor for $(uname -s)-$(uname -m)" >&2; exit 2 ;;
  esac
  read -r tp_version tp_url tp_sha < <(python3 -I - "$repo/tools/packages/tool-registry.json" "$reg_key" <<'PY'
import json, sys
tool = json.load(open(sys.argv[1]))["tools"]["trace-processor"]
src = tool["binary_sources"][sys.argv[2]]
version = tool["pinned_version"]
print(version, src["url_template"].replace("${version}", version), src["sha256"])
PY
  )
  dest_dir="${PULP_HOME:-$HOME/.pulp}/tools/trace-processor/$tp_version/$cache_key"
  mkdir -p "$dest_dir"
  curl -fsSL --retry 3 -o "$dest_dir/trace_processor_shell.download" "$tp_url"
  got="$(python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' \
         "$dest_dir/trace_processor_shell.download")"
  if [ "$got" != "$tp_sha" ]; then
    rm -f "$dest_dir/trace_processor_shell.download"
    echo "trace_consumer_e2e: trace_processor SHA-256 mismatch ($got, pinned $tp_sha)" >&2
    exit 2
  fi
  chmod +x "$dest_dir/trace_processor_shell.download"
  mv "$dest_dir/trace_processor_shell.download" "$dest_dir/trace_processor_shell"
  tp="$dest_dir/trace_processor_shell"
fi
if [ "$expect" = on ] && { [ -z "$tp" ] || [ ! -x "$tp" ]; }; then
  echo "trace_consumer_e2e: no trace_processor_shell found; run 'pulp trace fetch', pass --trace-processor, or pass --fetch-trace-processor" >&2
  exit 2
fi

mkdir -p "$work"
work="$(cd "$work" && pwd)"
build="$work/build"
trace="$work/consumer.pftrace"
rm -rf "$build" "$trace"

generator=()
command -v ninja >/dev/null 2>&1 && generator=(-G Ninja)
expect_flag=OFF; [ "$expect" = on ] && expect_flag=ON
cmake -S "$fixture" -B "$build" "${generator[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPulp_DIR="$sdk/lib/cmake/Pulp" \
  -DEXPECT_TRACING="$expect_flag" > "$work/configure.log" 2>&1 \
  || { tail -30 "$work/configure.log" >&2; fail "configure against $sdk (EXPECT_TRACING=$expect_flag)"; }
cmake --build "$build" > "$work/build.log" 2>&1 \
  || { tail -30 "$work/build.log" >&2; fail "fixture build against $sdk"; }

bin="$build/trace-consumer"
[ -x "$bin" ] || bin="$build/trace-consumer.exe"
[ -x "$bin" ] || fail "fixture binary was not produced"

set +e
status_line="$("$bin" "$trace")"
rc=$?
set -e
echo "$status_line"
case "$status_line" in
  "trace-consumer: "*) ;;
  *) fail "fixture printed no status line (exit $rc); the run cannot be judged" ;;
esac
[ "$rc" -eq 0 ] || fail "fixture exited $rc"

field() { sed -n "s/.* $1=\([^ ]*\).*/\1/p" <<<"$status_line"; }

# Size: the binary as linked, and a stripped copy.
bytes() { wc -c < "$1" | tr -d ' '; }
stripped="$work/trace-consumer.stripped"
cp "$bin" "$stripped"
if [ "$(uname -s)" = Darwin ]; then strip -x "$stripped"; else strip "$stripped"; fi
echo "size: expect=$expect binary_bytes=$(bytes "$bin") stripped_bytes=$(bytes "$stripped")"

perfetto_syms="$(nm -C "$bin" 2>/dev/null | grep -c perfetto || true)"
pulp_syms="$(nm -C "$bin" 2>/dev/null | grep -c 'pulp::runtime' || true)"
echo "symbols: perfetto=$perfetto_syms pulp_runtime=$pulp_syms"
[ "$pulp_syms" -gt 0 ] || fail "nm found no pulp::runtime symbols; the symbol table could not be read"

if [ "$expect" = off ]; then
  [ "$(field compiled_in)" = 0 ] || fail "an untraced SDK compiled tracing into the consumer"
  [ "$(field start)" = unavailable ] || fail "an untraced SDK started a session ($(field start))"
  [ ! -e "$trace" ] || fail "an untraced SDK wrote $trace"
  [ "$perfetto_syms" -eq 0 ] || fail "an untraced consumer binary carries $perfetto_syms Perfetto symbols"
  echo "PASS: untraced SDK: consumer emits nothing and carries no Perfetto"
  exit 0
fi

[ "$(field compiled_in)" = 1 ] || fail "the traced SDK did not compile tracing into the consumer"
[ "$(field start)" = started ] || fail "session did not start ($(field start))"
[ "$(field stopped)" = 1 ] || fail "session did not flush"
[ -s "$trace" ] || fail "no trace written at $trace"
[ "$perfetto_syms" -gt 0 ] || fail "a traced consumer binary carries no Perfetto symbols"

query="$work/query.sql"
cat > "$query" <<'SQL'
SELECT
  (SELECT count(*) FROM slice) AS total,
  (SELECT count(*) FROM slice
     WHERE name = 'consumer_probe' AND category = 'lifecycle' AND dur > 0) AS probe,
  (SELECT count(*) FROM slice
     WHERE name = 'consumer_probe_instant' AND category = 'lifecycle'
       AND EXTRACT_ARG(arg_set_id, 'debug.frame_index') = 7) AS instant;
SQL
"$tp" -q "$query" "$trace" > "$work/query.out" 2> "$work/query.err" \
  || { tail -20 "$work/query.err" >&2; echo "trace_consumer_e2e: trace_processor failed" >&2; exit 2; }
row="$(grep -E '^[0-9]+,[0-9]+,[0-9]+$' "$work/query.out" | tail -1 || true)"
[ -n "$row" ] || { cat "$work/query.out" >&2; echo "trace_consumer_e2e: no result row from trace_processor" >&2; exit 2; }
IFS=, read -r total probe instant <<<"$row"
echo "query: total=$total probe=$probe instant=$instant"
[ "$probe" -ge 1 ] || fail "the consumer's consumer_probe slice is not in the trace"
[ "$instant" -ge 1 ] || fail "the consumer's consumer_probe_instant (frame_index=7) is not in the trace"
[ "$total" -ge "$((probe + instant))" ] || fail "total slice count $total is below the probe counts; the query is inconsistent"
echo "PASS: traced SDK: the consumer's span and instant are in $trace"
