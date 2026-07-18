# shellcheck shell=sh
# tart-home.sh — one shared resolution of TART_HOME (the Tart VM store) for
# every piece of Pulp's VM tooling. Source it; call pulp_require_tart_home
# immediately before the first `tart` invocation.
#
# The store path is a per-host FACT, not a repo constant: a host that keeps its
# VMs on an external build SSD and a host that keeps them on internal storage
# hold them at different paths, by design. A repo-side default is therefore
# wrong on some host — and wrong QUIETLY, because `tart list` against an empty
# store is not an error. A reaper pointed at the wrong store inspects nothing,
# deletes nothing, and exits 0 reporting success: a silent no-op forever.
#
# So the repo holds the rule and the host holds the value. Resolution order:
#
#   1. TART_HOME in the environment (shell profile, launchd plist, one-off).
#   2. The host's own declaration via `tartci host-profile --json` (.vm_home).
#   3. A hard error naming exactly what to set and where. Never a guess.
#
# POSIX sh — no bashisms, so it is sourceable from any of the tooling shells.

# Echo the host's declared VM store, or return 1 when the host declares none.
# Any tartci/parse failure is "not declared", never a fabricated path.
pulp_tart_home_from_profile() {
    command -v tartci >/dev/null 2>&1 || return 1
    _pth_json=$(tartci host-profile --json 2>/dev/null) || return 1
    [ -n "$_pth_json" ] || return 1

    _pth_val=""
    if command -v python3 >/dev/null 2>&1; then
        _pth_val=$(printf '%s' "$_pth_json" | python3 -c '
import json, sys
try:
    profile = json.load(sys.stdin)
except Exception:
    sys.exit(1)
value = profile.get("vm_home") if isinstance(profile, dict) else None
if isinstance(value, str) and value.strip():
    sys.stdout.write(value.strip())
' 2>/dev/null) || _pth_val=""
    elif command -v jq >/dev/null 2>&1; then
        _pth_val=$(printf '%s' "$_pth_json" | jq -r '.vm_home // empty' 2>/dev/null) || _pth_val=""
    fi

    [ -n "$_pth_val" ] || return 1
    printf '%s' "$_pth_val"
}

# Resolve + export TART_HOME, or exit non-zero with an actionable message.
pulp_require_tart_home() {
    if [ -n "${TART_HOME:-}" ]; then
        export TART_HOME
        return 0
    fi
    if TART_HOME=$(pulp_tart_home_from_profile); then
        export TART_HOME
        return 0
    fi

    printf '\033[31m✗ TART_HOME is not set, and this host does not declare a VM store.\033[0m\n' >&2
    cat >&2 <<'EOF'

  The VM store path is a per-host value; the repo will not guess it. Guessing
  is worse than failing: `tart` treats a wrong-but-existing path as an empty
  store, so VM tooling would appear to succeed while operating on nothing.

  Declare it on THIS host, in one of these ways:

    1. Persist it for every shell and launchd agent (preferred):
         echo 'export TART_HOME=/path/to/VMs' >> ~/.zprofile
       and set the same path in the <key>TART_HOME</key> entry of each
       ~/Library/LaunchAgents/com.danielraffel.pulp.*.plist that runs VM
       tooling (launchd does not read your shell profile).

    2. Declare it to tartci, so `tartci host-profile --json` reports it as
       "vm_home" and every tool here inherits it.

    3. For this invocation only:
         TART_HOME=/path/to/VMs <command>

  To find an existing store on this host:
    ls -d ~/VMs /Volumes/*/VMs 2>/dev/null
EOF
    exit 1
}
