#!/usr/bin/env bash
# Install, and continuously verify, the Pulp CI files on the Proxmox host.
#
#   proxmox-host-health.sh                     # check; exit 1 on any finding
#   proxmox-host-health.sh --json              # the same check as one JSON object
#   proxmox-host-health.sh --json-out FILE     # text check; also write the JSON to FILE
#   proxmox-host-health.sh --install DIR       # install DIR/tools/ci/* per the table
#   proxmox-host-health.sh --install-staged    # install the verified stage of the ref
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
#
# Drift never installs anything by itself, because a merge to tools/ci must not
# become root code on this host until someone chooses to install it. Instead the
# check records when drift began (drift.state, cleared on the next drift-free
# check) and stages the table's files at the ref's exact commit, each verified
# against that commit's blob ids. --install-staged re-verifies the stage against
# the ref as it is now and refuses a stage the ref has moved past, so the one
# command a person runs installs exactly what the check compared against.
# --json reports the same state for fleet monitoring, including how long drift
# has persisted. The reaper unit runs the check with --json-out, so fleet
# monitoring reads the latest result from a world-readable file instead of
# running the check, which needs root's GitHub credential, itself.
set -uo pipefail

REPO="${PULP_PROXMOX_HEALTH_REPO:-Generous-Corp/pulp}"
REF="${PULP_PROXMOX_HEALTH_REF:-main}"
GH_CLI="${PULP_PROXMOX_HEALTH_GH:-/usr/local/bin/ghapp}"
ROOT_PREFIX="${PULP_PROXMOX_HEALTH_ROOT:-}"
SYSTEMCTL="${PULP_PROXMOX_HEALTH_SYSTEMCTL:-systemctl}"
GOVERNOR="${PULP_PROXMOX_HEALTH_GOVERNOR:-/usr/local/sbin/macpro-governor.sh}"
MAX_DATA_PERCENT="${MACPRO_GOVERNOR_MAX_DATA_PERCENT:-85}"
STATE_DIR="${ROOT_PREFIX}/var/lib/pulp-ci-host"
DRIFT_STATE="${STATE_DIR}/drift.state"
STAGE_ROOT="${ROOT_PREFIX}/root/pulp-deploy-staged"
# Warn this many points before the governor starts refusing clones.
DISK_WARN_MARGIN=10
JSON=0
JSON_OUT=""

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

# In --json mode stdout carries only the JSON object; the lines go to stderr.
log() { if [ "$JSON" = 1 ]; then printf '%s\n' "$*" >&2; else printf '%s\n' "$*"; fi; }

now_epoch() { printf '%s\n' "${PULP_PROXMOX_HEALTH_NOW:-$(date -u +%s)}"; }

gh_api() {
    env -u GH_TOKEN -u GITHUB_TOKEN HOME="${HOME:-/root}" "$GH_CLI" api "$@"
}

# Prints "<commit sha>" then the tools/ci listing (name<TAB>blob) at that
# commit, so every later comparison and the stage use one exact commit.
read_ref() {
    local sha listing
    sha="$(gh_api "repos/${REPO}/commits/${REF}" --jq .sha 2>/dev/null)"
    [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || return 1
    listing="$(gh_api "repos/${REPO}/contents/tools/ci?ref=${sha}" \
        --jq '.[] | [.name, .sha] | @tsv' 2>/dev/null)"
    [ -n "$listing" ] || return 1
    printf '%s\n%s\n' "$sha" "$listing"
}

listing_blob() { awk -F '\t' -v n="$2" '$1 == n { print $2 }' <<< "$1"; }

# Every table file in DIR/tools/ci must carry the listing's blob id.
stage_matches() {
    local dir="$1" listing="$2" repo_file host_path mode expected
    while read -r repo_file host_path mode; do
        [ -n "$repo_file" ] || continue
        expected="$(listing_blob "$listing" "$repo_file")"
        [ -n "$expected" ] && [ -f "${dir}/tools/ci/${repo_file}" ] || return 1
        [ "$(git_blob_id "${dir}/tools/ci/${repo_file}")" = "$expected" ] || return 1
    done <<< "$MANIFEST"
}

# Stage the table at SHA under STAGE_ROOT/SHA, verified file by file, and keep
# only that one stage. Prints the stage directory when it is ready; its
# messages go to stderr because the caller captures stdout.
stage_ref() {
    local sha="$1" listing="$2" dir tmp repo_file host_path mode old
    dir="${STAGE_ROOT}/${sha}"
    if ! { [ -f "${dir}/.verified" ] && stage_matches "$dir" "$listing"; }; then
        tmp="${STAGE_ROOT}/.incoming.$$"
        rm -rf "$tmp" && mkdir -p "${tmp}/tools/ci" || return 1
        while read -r repo_file host_path mode; do
            [ -n "$repo_file" ] || continue
            gh_api -H "Accept: application/vnd.github.raw" \
                "repos/${REPO}/contents/tools/ci/${repo_file}?ref=${sha}" \
                > "${tmp}/tools/ci/${repo_file}" 2>/dev/null \
                || { rm -rf "$tmp"; log "STAGE cannot fetch tools/ci/${repo_file} at ${sha:0:12}" >&2; return 1; }
        done <<< "$MANIFEST"
        if ! stage_matches "$tmp" "$listing"; then
            rm -rf "$tmp"
            log "STAGE fetched files do not match ${sha:0:12}'s blob ids; nothing staged" >&2
            return 1
        fi
        printf '%s\n' "$sha" > "${tmp}/.verified"
        rm -rf "$dir" && mv "$tmp" "$dir" || return 1
    fi
    for old in "$STAGE_ROOT"/* "$STAGE_ROOT"/.incoming.*; do
        [ -e "$old" ] && [ "$old" != "$dir" ] && rm -rf "$old"
    done
    printf '%s\n' "$dir"
}

# drift.state keeps the first time drift was seen; a later ref only updates the
# sha, so the age measures how long the host has gone without a reinstall.
record_drift() {
    local sha="$1" first=""
    [ -f "$DRIFT_STATE" ] && first="$(sed -n 's/^first_drift_epoch=//p' "$DRIFT_STATE")"
    [[ "$first" =~ ^[0-9]+$ ]] || first="$(now_epoch)"
    mkdir -p "$STATE_DIR" || return 1
    printf 'first_drift_epoch=%s\nref_sha=%s\n' "$first" "$sha" > "${DRIFT_STATE}.new" \
        && mv -f "${DRIFT_STATE}.new" "$DRIFT_STATE"
    printf '%s\n' "$first"
}

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
    local ref_data sha="" listing repo_file host_path mode target expected actual
    local findings=0 unit data_pct="" disk_finding=0 first="" stage="" reinstall
    local drift_rows="" failed_rows=""
    if ! ref_data="$(read_ref)"; then
        log "UNVERIFIED cannot read ${REPO} tools/ci at ${REF}; drift is unknown"
        report unverified "" "" "" "" "" "" "" ""
        exit 2
    fi
    sha="$(head -n 1 <<< "$ref_data")"
    listing="$(tail -n +2 <<< "$ref_data")"
    while read -r repo_file host_path mode; do
        [ -n "$repo_file" ] || continue
        target="${ROOT_PREFIX}${host_path}"
        expected="$(listing_blob "$listing" "$repo_file")"
        if [ -z "$expected" ]; then
            log "DRIFT $host_path — tools/ci/${repo_file} is not on ${REF}"
            drift_rows+="${host_path}"$'\t'"${repo_file}"$'\t'$'\t'$'\t'"not_on_ref"$'\n'
        elif [ ! -f "$target" ]; then
            log "DRIFT $host_path — not installed (expected tools/ci/${repo_file})"
            drift_rows+="${host_path}"$'\t'"${repo_file}"$'\t'$'\t'"${expected}"$'\t'"not_installed"$'\n'
        else
            actual="$(git_blob_id "$target")"
            if [ "$actual" != "$expected" ]; then
                log "DRIFT $host_path — installed ${actual:0:12}, ${REF} has ${expected:0:12} (tools/ci/${repo_file})"
                drift_rows+="${host_path}"$'\t'"${repo_file}"$'\t'"${actual}"$'\t'"${expected}"$'\t'"modified"$'\n'
            fi
        fi
    done <<< "$MANIFEST"
    if [ -n "$drift_rows" ]; then
        findings=$((findings + $(printf '%s' "$drift_rows" | grep -c .)))
        first="$(record_drift "$sha")" || log "STATE cannot write $DRIFT_STATE"
        stage="$(stage_ref "$sha" "$listing")" || stage=""
    else
        rm -f "$DRIFT_STATE"
    fi

    # A slot that tripped StartLimitBurst stays down until someone resets it.
    while read -r unit _; do
        [ -n "$unit" ] || continue
        log "FAILED $unit — restart ceiling or fatal error; see journalctl -u $unit"
        failed_rows+="${unit}"$'\n'
        findings=$((findings + 1))
    done < <("$SYSTEMCTL" list-units --failed --plain --no-legend \
        'pulp-ephemeral-pool@*' 'pulp-trusted-ephemeral-pool@*' \
        'pulp-pr-safe-ephemeral-pool@*' 'proxmox-ephemeral-pool@*' 2>/dev/null)

    if [ -x "${ROOT_PREFIX}${GOVERNOR}" ]; then
        data_pct="$("${ROOT_PREFIX}${GOVERNOR}" status 2>/dev/null \
            | sed -n 's/^disk: .* data \([0-9]*\)%.*/\1/p')"
        if [ -z "$data_pct" ]; then
            log "DISK thin-pool fill is unreadable"
            disk_finding=1
        elif [ "$data_pct" -ge $((MAX_DATA_PERCENT - DISK_WARN_MARGIN)) ]; then
            log "DISK thin pool at ${data_pct}%; clones are refused at ${MAX_DATA_PERCENT}%"
            disk_finding=1
        fi
        findings=$((findings + disk_finding))
    fi

    if [ -n "$stage" ]; then
        reinstall="$0 --install-staged"
    else
        reinstall="$0 --install <pulp checkout at ${REF}>"
    fi
    if [ "$findings" -gt 0 ]; then
        if [ -n "$drift_rows" ]; then
            log "STAGE ${REF} at ${sha:0:12}: ${stage:-nothing staged}${stage:+ is staged and verified}; drift began $(( $(now_epoch) - ${first:-$(now_epoch)} ))s ago"
        fi
        log "UNHEALTHY ${findings} finding(s); reinstall with: ${reinstall}"
        report unhealthy "$sha" "$first" "$drift_rows" "$failed_rows" \
            "$data_pct" "$disk_finding" "$stage" "$reinstall"
        exit 1
    fi
    log "HEALTHY every installed file matches ${REPO}@${REF}; no failed pool slot"
    report healthy "$sha" "" "" "" "$data_pct" 0 "" ""
    exit 0
}

# Print the JSON (--json) and/or replace JSON_OUT with it (--json-out). The
# file is renamed into place so a reader never sees a partial object, and every
# outcome, including unverified, rewrites it so checked_at shows the check ran.
report() {
    local dir
    [ "$JSON" = 1 ] && emit_json "$@"
    if [ -n "$JSON_OUT" ]; then
        dir="$(dirname "$JSON_OUT")"
        # Create the directory with its mode only when absent: an existing one
        # (e.g. /tmp, 1777) belongs to someone else and keeps its mode.
        { [ -d "$dir" ] || install -d -m 0755 "$dir"; } \
            && emit_json "$@" > "${JSON_OUT}.new.$$" \
            && chmod 0644 "${JSON_OUT}.new.$$" \
            && mv -f "${JSON_OUT}.new.$$" "$JSON_OUT" \
            || { rm -f "${JSON_OUT}.new.$$"; log "STATUS cannot write $JSON_OUT"; }
    fi
    return 0
}

# One JSON object for fleet monitoring. Schema 1 fields are append-only.
emit_json() {
    STATE="$1" SHA="$2" FIRST="$3" DRIFT_ROWS="$4" FAILED_ROWS="$5" DATA_PCT="$6" \
    DISK_FINDING="$7" STAGE="$8" REINSTALL="$9" NOW="$(now_epoch)" REPO="$REPO" REF="$REF" \
    MAX_DATA_PERCENT="$MAX_DATA_PERCENT" HOSTNAME_="$(hostname 2>/dev/null)" \
    python3 - <<'PY'
import datetime, json, os

def iso(epoch):
    return datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

env = os.environ
now = int(env["NOW"])
first = env["FIRST"]
files = []
for row in env["DRIFT_ROWS"].splitlines():
    host_path, repo_file, installed, expected, reason = row.split("\t")
    files.append({
        "host_path": host_path,
        "repo_path": f"tools/ci/{repo_file}",
        "installed_blob": installed or None,
        "expected_blob": expected or None,
        "reason": reason,
    })
stage = env["STAGE"]
print(json.dumps({
    "schema": 1,
    "host": env["HOSTNAME_"] or None,
    "checked_at": iso(now),
    "state": env["STATE"],
    "repo": env["REPO"],
    "ref": env["REF"],
    "ref_sha": env["SHA"] or None,
    "drift": {
        "first_seen": iso(first) if first else None,
        "age_seconds": now - int(first) if first else None,
        "files": files,
    },
    "failed_units": env["FAILED_ROWS"].split(),
    "disk": {
        "data_percent": int(env["DATA_PCT"]) if env["DATA_PCT"] else None,
        "refuse_at_percent": int(env["MAX_DATA_PERCENT"]),
        "finding": env["DISK_FINDING"] == "1",
    },
    "stage": {"path": stage or None, "ready": bool(stage)},
    "reinstall_command": env["REINSTALL"] or None,
}, sort_keys=True))
PY
}

do_install_staged() {
    local ref_data sha listing dir
    ref_data="$(read_ref)" || { log "REFUSED cannot read ${REPO} at ${REF}; the stage cannot be verified"; exit 2; }
    sha="$(head -n 1 <<< "$ref_data")"
    listing="$(tail -n +2 <<< "$ref_data")"
    dir="${STAGE_ROOT}/${sha}"
    if [ ! -f "${dir}/.verified" ]; then
        log "REFUSED no stage for ${REF} at ${sha:0:12}; run the check to stage it"
        exit 1
    fi
    if ! stage_matches "$dir" "$listing"; then
        log "REFUSED stage ${dir} no longer matches ${REF} at ${sha:0:12}"
        exit 1
    fi
    log "installing ${REPO}@${sha:0:12} from ${dir}"
    do_install "$dir"
}

case "${1:-}" in
    "") do_check ;;
    --json) JSON=1; do_check ;;
    --json-out) [ -n "${2:-}" ] || { log "usage: $0 --json-out <file>"; exit 2; }; JSON_OUT="$2"; do_check ;;
    --install) [ -n "${2:-}" ] || { log "usage: $0 --install <pulp checkout>"; exit 2; }; do_install "$2" ;;
    --install-staged) do_install_staged ;;
    --manifest) printf '%s\n' "$MANIFEST" ;;
    *) log "usage: $0 [--json | --json-out <file> | --install <pulp checkout> | --install-staged | --manifest]"; exit 2 ;;
esac
