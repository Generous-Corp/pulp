#!/usr/bin/env bash
# record_build_metric.sh — record one local build in Shipyard's metrics store.
#
# The single implementation of the local-build metrics contract, shared by
# every build path that runs on a developer or agent machine: the governed
# wrapper (tools/ci/governed-build.sh, used by Shipyard's local lane) and the
# `pulp build` / `pulp dev` CLI. One contract means `shipyard metrics watch
# --project pulp` sees agents' focused builds and the lane's full builds as the
# same series, keyed by target `local-build/<scope>` and the host.
#
# Best-effort and silent by design. It returns immediately: the record runs in
# the background with every stream detached, so it can neither fail nor delay
# the build, nor hold a caller's command-substitution pipe open. No shipyard on
# PATH (a build VM, a contributor checkout) or PULP_BUILD_METRICS=0 records
# nothing.
#
# Usage:
#   record_build_metric.sh --provider <governed-build|pulp-cli> \
#     --start <epoch-s> --end <epoch-s> --exit-code <rc> \
#     --jobs <n> --grant <lease|floor|tier0|host-profile|user|inherited> \
#     [--duration-ms <ms>] [--target <name>]... [--total-targets <n>]
#
# `--target` names each target the build was restricted to; none means `all`.
# `--duration-ms` overrides the whole-second difference of start and end when
# the caller measured more precisely.
set -u

[ "${PULP_BUILD_METRICS:-1}" = "0" ] && exit 0
sy="$(command -v shipyard 2>/dev/null)" || exit 0

provider="" start="" end="" rc="" jobs="" grant="" duration_ms="" total="" targets="" count=0
while [ $# -gt 0 ]; do
  case "$1" in
    --provider) provider="${2:-}"; shift 2 ;;
    --start) start="${2:-}"; shift 2 ;;
    --end) end="${2:-}"; shift 2 ;;
    --exit-code) rc="${2:-}"; shift 2 ;;
    --jobs) jobs="${2:-}"; shift 2 ;;
    --grant) grant="${2:-}"; shift 2 ;;
    --duration-ms) duration_ms="${2:-}"; shift 2 ;;
    --total-targets) total="${2:-}"; shift 2 ;;
    --target) targets="${targets:+$targets,}${2:-}"; count=$((count + 1)); shift 2 ;;
    *) exit 0 ;;  # an unknown field is a caller newer than this script: record nothing
  esac
done

is_uint() { case "$1" in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac; }
is_uint "$start" && is_uint "$end" && is_uint "$rc" || exit 0
[ "$start" -gt 0 ] && [ "$end" -ge "$start" ] || exit 0
is_uint "$duration_ms" || duration_ms=$(( (end - start) * 1000 ))

if [ "$count" -gt 0 ]; then
  scope="focused"
  workflow="targets:${count}/${total:-?}:${targets}"
else
  scope="all"
  workflow="targets:all"
fi

iso() { date -u -r "$1" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u -d "@$1" +%Y-%m-%dT%H:%M:%SZ; }
host="$(hostname -s 2>/dev/null || hostname)"

(
  "$sy" metrics record --project pulp --job governed-build \
    --target "local-build/$scope" --platform "$(uname -s | tr '[:upper:]' '[:lower:]')" \
    --backend local --provider "${provider:-unknown}" --host "$host" --runner "$host" \
    --step build --duration-ms "$duration_ms" \
    --status "$([ "$rc" -eq 0 ] && echo success || echo failure)" --exit-code "$rc" \
    --profile "j${jobs:-?}" --routing-decision "${grant:-unknown}" --workflow "${workflow:0:240}" \
    --branch "$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)" \
    --sha "$(git rev-parse HEAD 2>/dev/null || echo unknown)" \
    --started-at "$(iso "$start")" --completed-at "$(iso "$end")" \
    --external-id "local:$host:${provider:-unknown}:$$:$start"
) >/dev/null 2>&1 </dev/null &
exit 0
