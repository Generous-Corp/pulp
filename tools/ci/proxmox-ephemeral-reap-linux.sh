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

BASE=200; MAX=219
REPO="Generous-Corp/pulp"
PAT_FILE=/root/.config/pulp/secrets/gh-runner-pat
DO_IT=0
[ "${1:-}" = "--yes" ] && DO_IT=1

registered() {   # is a runner by this name known to GitHub (any status)?
    [ -r "$PAT_FILE" ] || return 0   # cannot prove it is unused -> treat as in use
    curl -s -H "Authorization: Bearer $(cat "$PAT_FILE")" \
         -H "Accept: application/vnd.github+json" \
         "https://api.github.com/repos/${REPO}/actions/runners?per_page=100" 2>/dev/null \
      | grep -q "\"name\": *\"pulp-ci-ephemeral-$1\""
}

for id in $(seq "$BASE" "$MAX"); do
    qm status "$id" >/dev/null 2>&1 || continue
    name="pulp-ci-ephemeral-$id"

    if registered "$id"; then
        echo "SKIP $id — still registered with GitHub"
        continue
    fi

    ip="$(qm guest cmd "$id" network-get-interfaces 2>/dev/null \
          | grep -oE '192\.168\.[0-9]+\.[0-9]+' | head -1)"
    if [ -n "$ip" ]; then
        # Unreachable is NOT permission to delete: a guest that cannot answer
        # cannot testify that it is idle.
        if ! ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
                 "ci@$ip" true 2>/dev/null; then
            echo "SKIP $id — guest unreachable, cannot prove it is idle"
            continue
        fi
        if ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 \
               "ci@$ip" 'pgrep -f Runner.Worker >/dev/null' 2>/dev/null; then
            echo "SKIP $id — a Runner.Worker is active"
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
