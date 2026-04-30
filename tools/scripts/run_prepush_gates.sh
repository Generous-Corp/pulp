#!/usr/bin/env bash
# Pulp pre-push gate runner (standalone, agent-discoverable).
#
# Runs the same policy gates the .githooks/pre-push hook runs, with
# louder, more agent-readable output. Returns non-zero on any gate
# failure so it can be wired into agent loops, scripts, or shell
# pre-flight checks before invoking `git push`.
#
# Usage:
#   tools/scripts/run_prepush_gates.sh                       # run all gates
#   tools/scripts/run_prepush_gates.sh --base origin/develop # custom base
#   tools/scripts/run_prepush_gates.sh --skip-diff-cover     # skip diff-cover
#
# Environment variables (mirror the hook's contract — see #1144):
#   PULP_PREPUSH_BASE             override the diff base (default: origin/main)
#   PULP_DISABLE_PREPUSH_GATES=1  demote gate failures to advisory (still print)
#   PULP_DISABLE_PREPUSH_DIFF_COVER=1
#                                 demote only the diff-coverage gate to advisory
#   PULP_SKIP_DIFF_COVER=1        skip the diff-coverage check entirely
#   PULP_SKIP_PREPUSH=1           skip ALL gates (matches the hook's bypass)
#
# Exit codes:
#   0 — all gates clean (or all failures explicitly demoted via env var)
#   1 — at least one gate failed and the demote env var was not set
#   2 — environment / repo state is unusable (not a git repo, missing scripts)

set -u

usage() {
    sed -n 's/^# \{0,1\}//p' "$0" | sed -n '2,/^[^#]/p' | sed '$d'
}

# Argument parsing — light touch; env vars are the primary contract.
SKIP_DIFF_COVER_ARG=0
BASE_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --base)
            shift
            BASE_ARG="${1:-}"
            if [ -z "$BASE_ARG" ]; then
                echo "run_prepush_gates: --base requires an argument" >&2
                exit 2
            fi
            ;;
        --skip-diff-cover)
            SKIP_DIFF_COVER_ARG=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "run_prepush_gates: unknown argument: $1" >&2
            echo "Try: $0 --help" >&2
            exit 2
            ;;
    esac
    shift
done

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "run_prepush_gates: not in a git repo" >&2
    exit 2
}
cd "$ROOT" || exit 2

if [ "${PULP_SKIP_PREPUSH:-0}" = "1" ]; then
    echo "[run_prepush_gates] PULP_SKIP_PREPUSH=1 — skipping all gates." >&2
    exit 0
fi

PYTHON="${PYTHON:-python3}"
VBC="$ROOT/tools/scripts/version_bump_check.py"
SSC="$ROOT/tools/scripts/skill_sync_check.py"
CSC="$ROOT/tools/scripts/compat_sync_check.py"
CFG="$ROOT/tools/scripts/versioning.json"
COMPAT_MAP="$ROOT/tools/scripts/compat_path_map.json"
DEPS_AUDIT="$ROOT/tools/deps/audit.py"
DIFF_COVER_SH="$ROOT/tools/scripts/local_diff_cover.sh"

if [ ! -f "$VBC" ] || [ ! -f "$SSC" ] || [ ! -f "$CFG" ]; then
    echo "run_prepush_gates: missing one or more gate scripts (older checkout?)" >&2
    exit 2
fi

if [ -n "$BASE_ARG" ]; then
    BASE="$BASE_ARG"
else
    BASE="${PULP_PREPUSH_BASE:-origin/main}"
fi

echo "============================================================" >&2
echo "[run_prepush_gates] running pre-push gates against ${BASE}" >&2
echo "============================================================" >&2

fail=0
failed_gates=()

run_gate() {
    local name="$1"; shift
    local remediation="$1"; shift
    echo "" >&2
    echo "[gate] ${name}" >&2
    echo "------------------------------------------------------------" >&2
    if "$@"; then
        echo "[gate] ${name}: clean" >&2
        return 0
    fi
    echo "[gate] ${name}: FAILED" >&2
    echo "[gate] remediation: ${remediation}" >&2
    fail=1
    failed_gates+=("${name}")
    return 1
}

run_gate "skill-sync" \
    "update the matching .agents/skills/<name>/SKILL.md OR add 'Skill-Update: skip skill=<name> reason=\"...\"' to the tip commit" \
    "$PYTHON" "$SSC" --base "$BASE" --config "$CFG" --mode=report || true

run_gate "version-bump" \
    "bump the affected surface(s) (CMakeLists.txt / .claude-plugin/plugin.json) OR add 'Version-Bump: <surface>=skip reason=\"...\"' to the tip commit" \
    "$PYTHON" "$VBC" --base "$BASE" --config "$CFG" --mode=report || true

if [ -f "$CSC" ] && [ -f "$COMPAT_MAP" ]; then
    run_gate "compat-sync" \
        "update compat.json + the matching docs page OR add a 'Compat-Update: skip prefix=<prefix> reason=\"...\"' trailer" \
        "$PYTHON" "$CSC" --base "$BASE" --mode=report --enforce || true
fi

if [ -f "$DEPS_AUDIT" ]; then
    echo "" >&2
    echo "[gate] deps-audit" >&2
    echo "------------------------------------------------------------" >&2
    if "$PYTHON" "$DEPS_AUDIT" --strict; then
        echo "[gate] deps-audit: clean" >&2
    else
        echo "[gate] deps-audit: FAILED" >&2
        echo "[gate] remediation: run \`python3 tools/deps/audit.py --strict\` and reconcile DEPENDENCIES.md / NOTICE.md / docs/reference/licensing.md" >&2
        fail=1
        failed_gates+=("deps-audit")
    fi
fi

if [ -f "$DIFF_COVER_SH" ] \
   && [ "$SKIP_DIFF_COVER_ARG" != "1" ] \
   && [ "${PULP_SKIP_DIFF_COVER:-0}" != "1" ]; then
    if changed=$(git diff --name-only "$BASE...HEAD" 2>/dev/null); then
        if echo "$changed" | grep -qE '^(core/|tools/cli/|tools/scripts/)'; then
            echo "" >&2
            echo "[gate] diff-cover" >&2
            echo "------------------------------------------------------------" >&2
            if bash "$DIFF_COVER_SH"; then
                echo "[gate] diff-cover: clean" >&2
            else
                if [ "${PULP_DISABLE_PREPUSH_DIFF_COVER:-0}" = "1" ]; then
                    echo "[gate] diff-cover: FAILED but DEMOTED to advisory (PULP_DISABLE_PREPUSH_DIFF_COVER=1)" >&2
                else
                    echo "[gate] diff-cover: FAILED" >&2
                    echo "[gate] remediation: add tests for changed core/ tools/cli/ tools/scripts/ surface; threshold + filters live in tools/scripts/coverage_config.json" >&2
                    fail=1
                    failed_gates+=("diff-cover")
                fi
            fi
        fi
    fi
fi

echo "" >&2
echo "============================================================" >&2
if [ "$fail" = "0" ]; then
    echo "[run_prepush_gates] all gates clean — push is safe." >&2
    echo "============================================================" >&2
    exit 0
fi

echo "[run_prepush_gates] FAILED gates: ${failed_gates[*]:-<none>}" >&2
echo "============================================================" >&2
echo "" >&2

if [ "${PULP_DISABLE_PREPUSH_GATES:-0}" = "1" ]; then
    echo "[run_prepush_gates] PULP_DISABLE_PREPUSH_GATES=1 — failures DEMOTED to advisory." >&2
    echo "[run_prepush_gates] CI will still enforce these gates; fix locally to avoid a CI roundtrip." >&2
    exit 0
fi

echo "[run_prepush_gates] options:" >&2
echo "  1. Fix the failing gate(s) above (recommended; CI runs the same checks)." >&2
echo "  2. PULP_DISABLE_PREPUSH_GATES=1 tools/scripts/run_prepush_gates.sh   # demote to advisory" >&2
echo "  3. PULP_SKIP_PREPUSH=1 git push                                       # full bypass (emergencies)" >&2
exit 1
