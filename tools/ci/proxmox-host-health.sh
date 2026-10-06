#!/usr/bin/env bash
# Install, and continuously verify, the Pulp CI files on the Proxmox host.
#
#   proxmox-host-health.sh                     # check; exit 1 on any finding
#   proxmox-host-health.sh --install DIR       # install DIR/tools/ci/* per the table
#   proxmox-host-health.sh --manifest          # print the install table
#
# One table below says which repository file is installed at which host path.
# --install copies from a checkout using that table, and the check compares
# every installed file's git blob id with the same repository path on the
# default branch. The two cannot disagree about where a file belongs, and a host
# copy edited in place, or left behind by a merge that changed the repository,
# is reported instead of silently running.
#
# The check also reports pool slots that hit their restart ceiling and thin-pool
# fill near the governor's refusal threshold. It runs after every reaper pass
# (ExecStartPost in pulp-ephemeral-reap.service), so a finding leaves that unit
# failed and visible in `systemctl --failed` within 15 minutes.
#
# A check that cannot reach GitHub exits 2, never 0: an unverified host is not a
# healthy one.
set -uo pipefail

REPO="${PULP_PROXMOX_HEALTH_REPO:-Generous-Corp/pulp}"
REF="${PULP_PROXMOX_HEALTH_REF:-main}"
GH_CLI="${PULP_PROXMOX_HEALTH_GH:-/usr/local/bin/ghapp}"
ROOT_PREFIX="${PULP_PROXMOX_HEALTH_ROOT:-}"
SYSTEMCTL="${PULP_PROXMOX_HEALTH_SYSTEMCTL:-systemctl}"
GOVERNOR="${PULP_PROXMOX_HEALTH_GOVERNOR:-/usr/local/sbin/macpro-governor.sh}"
MAX_DATA_PERCENT="${MACPRO_GOVERNOR_MAX_DATA_PERCENT:-85}"
# Warn this many points before the governor starts refusing clones.
DISK_WARN_MARGIN=10

# repository file (under tools/ci/)          host path                                       mode
MANIFEST="$(cat <<'EOF'
proxmox-ephemeral-runner-linux.sh            /usr/local/sbin/pulp-ephemeral-runner.sh                    0755
proxmox-trusted-ephemeral-runner-linux.sh    /usr/local/sbin/proxmox-trusted-ephemeral-runner-linux.sh   0755
proxmox-pr-safe-ephemeral-runner-linux.sh    /usr/local/sbin/proxmox-pr-safe-ephemeral-runner-linux.sh   0755
proxmox-ephemeral-reap-linux.sh              /usr/local/sbin/pulp-ephemeral-reap.sh                      0755
configure-proxmox-ci-network.sh              /usr/local/sbin/configure-proxmox-ci-network                0755
macpro-governor.sh                           /usr/local/sbin/macpro-governor.sh                          0755
proxmox-host-health.sh                       /usr/local/sbin/pulp-proxmox-host-health.sh                 0755
verify_linux_runner_group.py                 /usr/local/lib/pulp/verify_linux_runner_group.py            0755
pulp-ephemeral-pool@.service                 /etc/systemd/system/pulp-ephemeral-pool@.service            0644
pulp-trusted-ephemeral-pool@.service         /etc/systemd/system/pulp-trusted-ephemeral-pool@.service    0644
pulp-pr-safe-ephemeral-pool@.service         /etc/systemd/system/pulp-pr-safe-ephemeral-pool@.service    0644
proxmox-ephemeral-pool@.service              /etc/systemd/system/proxmox-ephemeral-pool@.service         0644
pulp-ephemeral-reap.service                  /etc/systemd/system/pulp-ephemeral-reap.service             0644
pulp-ephemeral-reap.timer                    /etc/systemd/system/pulp-ephemeral-reap.timer               0644
EOF
)"

log() { printf '%s\n' "$*"; }

git_blob_id() {
    python3 - "$1" <<'PY'
import hashlib, sys
data = open(sys.argv[1], "rb").read()
print(hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest())
PY
}

do_install() {
    local src="$1/tools/ci" stamp repo_file host_path mode target backup_dir
    [ -d "$src" ] || { log "ERROR: $src is not a Pulp checkout's tools/ci"; exit 2; }
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    backup_dir="${ROOT_PREFIX}/root/pulp-ci-host-backup-${stamp}"
    mkdir -p "$backup_dir"
    while read -r repo_file host_path mode; do
        [ -n "$repo_file" ] || continue
        [ -f "${src}/${repo_file}" ] || { log "ERROR: missing ${src}/${repo_file}"; exit 2; }
        target="${ROOT_PREFIX}${host_path}"
        mkdir -p "$(dirname "$target")"
        [ -e "$target" ] && cp -a "$target" "${backup_dir}/"
        # Write beside the target and rename over it: a running supervisor keeps
        # its open inode, and a reader never sees a half-written file.
        install -m "$mode" "${src}/${repo_file}" "${target}.new.$$" \
            && mv -f "${target}.new.$$" "$target" \
            || { log "ERROR: could not install $target"; exit 1; }
        log "installed $host_path"
    done <<< "$MANIFEST"
    log "previous copies saved in $backup_dir"
    [ -n "$ROOT_PREFIX" ] || "$SYSTEMCTL" daemon-reload
}

do_check() {
    local listing repo_file host_path mode target expected actual findings=0 unit data_pct
    listing="$(env -u GH_TOKEN -u GITHUB_TOKEN HOME="${HOME:-/root}" \
        "$GH_CLI" api "repos/${REPO}/contents/tools/ci?ref=${REF}" \
        --jq '.[] | [.name, .sha] | @tsv' 2>/dev/null)"
    if [ -z "$listing" ]; then
        log "UNVERIFIED cannot read ${REPO} tools/ci at ${REF}; drift is unknown"
        exit 2
    fi
    while read -r repo_file host_path mode; do
        [ -n "$repo_file" ] || continue
        target="${ROOT_PREFIX}${host_path}"
        expected="$(awk -F '\t' -v n="$repo_file" '$1 == n { print $2 }' <<< "$listing")"
        if [ -z "$expected" ]; then
            log "DRIFT $host_path — tools/ci/${repo_file} is not on ${REF}"
            findings=$((findings + 1))
        elif [ ! -f "$target" ]; then
            log "DRIFT $host_path — not installed (expected tools/ci/${repo_file})"
            findings=$((findings + 1))
        else
            actual="$(git_blob_id "$target")"
            if [ "$actual" != "$expected" ]; then
                log "DRIFT $host_path — installed ${actual:0:12}, ${REF} has ${expected:0:12} (tools/ci/${repo_file})"
                findings=$((findings + 1))
            fi
        fi
    done <<< "$MANIFEST"

    # A slot that tripped StartLimitBurst stays down until someone resets it.
    while read -r unit _; do
        [ -n "$unit" ] || continue
        log "FAILED $unit — restart ceiling or fatal error; see journalctl -u $unit"
        findings=$((findings + 1))
    done < <("$SYSTEMCTL" list-units --failed --plain --no-legend \
        'pulp-ephemeral-pool@*' 'pulp-trusted-ephemeral-pool@*' \
        'pulp-pr-safe-ephemeral-pool@*' 'proxmox-ephemeral-pool@*' 2>/dev/null)

    if [ -x "${ROOT_PREFIX}${GOVERNOR}" ]; then
        data_pct="$("${ROOT_PREFIX}${GOVERNOR}" status 2>/dev/null \
            | sed -n 's/^disk: .* data \([0-9]*\)%.*/\1/p')"
        if [ -z "$data_pct" ]; then
            log "DISK thin-pool fill is unreadable"
            findings=$((findings + 1))
        elif [ "$data_pct" -ge $((MAX_DATA_PERCENT - DISK_WARN_MARGIN)) ]; then
            log "DISK thin pool at ${data_pct}%; clones are refused at ${MAX_DATA_PERCENT}%"
            findings=$((findings + 1))
        fi
    fi

    if [ "$findings" -gt 0 ]; then
        log "UNHEALTHY ${findings} finding(s); reinstall with: $0 --install <pulp checkout at ${REF}>"
        exit 1
    fi
    log "HEALTHY every installed file matches ${REPO}@${REF}; no failed pool slot"
}

case "${1:-}" in
    "") do_check ;;
    --install) [ -n "${2:-}" ] || { log "usage: $0 --install <pulp checkout>"; exit 2; }; do_install "$2" ;;
    --manifest) printf '%s\n' "$MANIFEST" ;;
    *) log "usage: $0 [--install <pulp checkout> | --manifest]"; exit 2 ;;
esac
