#!/usr/bin/env bash
# pulp-ephemeral-reap — destroy leaked ephemeral clones, and ONLY those.
#
# A clone should destroy itself when its job ends. One that outlives its job is
# a leak (an interrupted run, or the period when the cleanup guard was never
# armed). Reaping it by hand is easy to get wrong in one specific way: printing
# a safety check and then deleting anyway. Every refusal here returns before the
# destroy, so the check cannot be decorative.
#
#   pulp-ephemeral-reap.sh          # report only
#   pulp-ephemeral-reap.sh --yes    # destroy what it reports
set -uo pipefail

BASE=200; MAX=219       # retain recovery coverage for legacy allocations
REPO="Generous-Corp/pulp"
PAT_FILE=/root/.config/pulp/secrets/gh-runner-pat
DO_IT=0
[ "${1:-}" = "--yes" ] && DO_IT=1

registered() {   # 0 registered, 1 absent, 2 lookup failed or incomplete
    local runner_name="$1" runners_json
    [ -r "$PAT_FILE" ] || return 2
    runners_json="$(curl -fSs -H "Authorization: Bearer $(cat "$PAT_FILE")" \
         -H "Accept: application/vnd.github+json" \
         "https://api.github.com/repos/${REPO}/actions/runners?per_page=100" 2>/dev/null)" \
        || return 2
    RUNNER_NAME="$runner_name" python3 -c '
import json, os, sys
try: data = json.load(sys.stdin)
except Exception: sys.exit(2)
if any(r.get("name") == os.environ["RUNNER_NAME"] for r in data.get("runners", [])):
    sys.exit(0)
sys.exit(2 if data.get("total_count", 0) > 100 else 1)
' <<<"$runners_json"
}

registered_for_vmid() {   # fallback when the guest identity file is unreadable
    local vmid="$1" runners_json
    [ -r "$PAT_FILE" ] || return 2
    runners_json="$(curl -fSs -H "Authorization: Bearer $(cat "$PAT_FILE")" \
         -H "Accept: application/vnd.github+json" \
         "https://api.github.com/repos/${REPO}/actions/runners?per_page=100" 2>/dev/null)" \
        || return 2
    VMID="$vmid" python3 -c '
import json, os, sys
try: data = json.load(sys.stdin)
except Exception: sys.exit(2)
base = "pulp-ci-ephemeral-" + os.environ["VMID"]
if any(r.get("name") == base or r.get("name", "").startswith(base + "-")
       for r in data.get("runners", [])):
    sys.exit(0)
sys.exit(2 if data.get("total_count", 0) > 100 else 1)
' <<<"$runners_json"
}

for id in $(seq "$BASE" "$MAX"); do
    qm status "$id" >/dev/null 2>&1 || continue
    ip="$(qm guest cmd "$id" network-get-interfaces 2>/dev/null \
          | grep -oE '192\.168\.[0-9]+\.[0-9]+' | head -1)"
    if [ -z "$ip" ]; then
        echo "SKIP $id — guest has no reachable IPv4 address"
        continue
    fi

    # Unreachable is NOT permission to delete: a guest that cannot answer
    # cannot testify that it is idle.
    if ! ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
             "ci@$ip" true 2>/dev/null; then
        echo "SKIP $id — guest unreachable, cannot prove it is idle"
        continue
    fi
    if ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
           "ci@$ip" 'pgrep -x Runner.Listener >/dev/null || pgrep -x Runner.Worker >/dev/null || pgrep -f "[c]onfig.sh" >/dev/null' 2>/dev/null; then
        echo "SKIP $id — an Actions runner process is active"
        continue
    fi

    # Correlate GitHub state to this VM's registration. Historical UUID-suffixed
    # ghosts for the same VMID must not make the current idle clone unreapable.
    if runner_name="$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
        "ci@$ip" "python3 -c 'import json; print(json.load(open(\"/home/ci/actions-runner/.runner\", encoding=\"utf-8-sig\"))[\"agentName\"])'" \
        2>/dev/null)"; then
        registered "$runner_name"
        lookup_status=$?
        if [ "$lookup_status" -eq 0 ]; then
            echo "SKIP $id — current runner $runner_name is still registered"
            continue
        elif [ "$lookup_status" -eq 2 ]; then
            echo "SKIP $id — cannot prove current runner is unregistered"
            continue
        fi
    else
        registered_for_vmid "$id"
        lookup_status=$?
        if [ "$lookup_status" -eq 0 ]; then
            echo "SKIP $id — runner identity unreadable and a matching registration exists"
            continue
        elif [ "$lookup_status" -eq 2 ]; then
            echo "SKIP $id — runner identity unreadable and registration state is unknown"
            continue
        fi
    fi

    if [ "$DO_IT" = 0 ]; then
        echo "WOULD REAP $id — unregistered and idle"
        continue
    fi
    echo "REAP $id"
    qm stop "$id" >/dev/null 2>&1
    for _ in $(seq 1 20); do
        [ "$(qm status "$id" 2>/dev/null)" = "status: stopped" ] && break; sleep 3
    done
    qm destroy "$id" --purge >/dev/null 2>&1 || echo "  WARN: destroy of $id failed"
    [ -n "$ip" ] && ssh-keygen -f /root/.ssh/known_hosts -R "$ip" >/dev/null 2>&1
done
