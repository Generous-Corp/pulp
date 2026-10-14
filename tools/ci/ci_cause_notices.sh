#!/usr/bin/env bash
# Name the cause of every failed job in this workflow run attempt as a
# `ci.cause/v1` record, printed as a `::notice title=ci-cause::` line.
#
# A job's own log cannot be downloaded until the job completes, so this runs
# in a job that needs the others and reads their finished logs through the
# Actions API. Each record comes from `shipyard cause extract`, run on the
# pinned Shipyard release below after its SHA-256 matches. The step is
# advisory: every failure here is reported and exits 0.
#
# Environment: GITHUB_REPOSITORY, GITHUB_RUN_ID, GITHUB_RUN_ATTEMPT,
# GITHUB_EVENT_NAME, GITHUB_EVENT_PATH, RUNNER_TEMP, GH_TOKEN (actions: read).
# Test seams: CI_CAUSE_GH (the gh command), CI_CAUSE_CURL (the download
# command), CI_CAUSE_ARCH (uname -m), CI_CAUSE_MAX_JOBS, and
# CI_CAUSE_TEST_SHA256, which replaces the pinned hash so a fake binary can be
# run; it is for the selftest beside this script only.
set -uo pipefail

SHIPYARD_VERSION="v0.326.0"
asset_sha256() {
    case "$1" in
        shipyard-linux-x64) echo "1c2368fd8f038f198343cd7884687dcf00cc049828cfe3edaebd682dd885b3da" ;;
        shipyard-linux-arm64) echo "d559667fd353b772f6156f877a250bbb3db9a08c75aecf6f670ef77f2cbb3a68" ;;
        *) return 1 ;;
    esac
}

gh_cmd="${CI_CAUSE_GH:-gh}"
curl_cmd="${CI_CAUSE_CURL:-curl}"
max_jobs="${CI_CAUSE_MAX_JOBS:-20}"
repo="${GITHUB_REPOSITORY:?}"
run_id="${GITHUB_RUN_ID:?}"
attempt="${GITHUB_RUN_ATTEMPT:-1}"
event="${GITHUB_EVENT_NAME:-}"
work="${RUNNER_TEMP:-/tmp}/ci-cause"
mkdir -p "$work"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

case "${CI_CAUSE_ARCH:-$(uname -m)}" in
    x86_64 | amd64) asset="shipyard-linux-x64" ;;
    aarch64 | arm64) asset="shipyard-linux-arm64" ;;
    *) asset="" ;;
esac
if [ "$(uname -s)" != "Linux" ] && [ -z "${CI_CAUSE_ARCH:-}" ]; then
    asset=""
fi
if [ -z "$asset" ]; then
    echo "ci-cause: no pinned Shipyard asset for $(uname -s) $(uname -m); no records"
    exit 0
fi
want="${CI_CAUSE_TEST_SHA256:-$(asset_sha256 "$asset")}"
bin="$work/shipyard"
url="https://github.com/danielraffel/Shipyard/releases/download/$SHIPYARD_VERSION/$asset"
if ! "$curl_cmd" -fsSL --retry 3 -o "$bin" "$url"; then
    echo "::warning title=ci-cause::could not download Shipyard $SHIPYARD_VERSION ($asset); no records"
    exit 0
fi
got=$(sha256_of "$bin")
if [ "$got" != "$want" ]; then
    echo "::warning title=ci-cause::Shipyard $SHIPYARD_VERSION $asset has SHA-256 $got, not the pinned $want; not run"
    rm -f "$bin"
    exit 0
fi
chmod +x "$bin"

pr=$(python3 -c 'import json, sys
try:
    print(json.load(open(sys.argv[1])).get("pull_request", {}).get("number", ""))
except Exception:
    print("")' "${GITHUB_EVENT_PATH:-/dev/null}")

listing="$work/jobs.json"
if ! "$gh_cmd" api --paginate \
    "repos/$repo/actions/runs/$run_id/attempts/$attempt/jobs?per_page=100" >"$listing"; then
    echo "::warning title=ci-cause::could not list the run's jobs; no records"
    exit 0
fi
# --paginate concatenates one JSON object per page.
ids=$(python3 -c 'import json, sys
text, at, decoder = open(sys.argv[1]).read(), 0, json.JSONDecoder()
while at < len(text):
    if text[at].isspace():
        at += 1
        continue
    page, at = decoder.raw_decode(text, at)
    for job in page.get("jobs", []):
        if job.get("conclusion") in ("failure", "timed_out"):
            print(job["id"])' "$listing")

# Job logs carry ANSI colour, and a gh that guards against terminal escape
# sequences refuses such a response unless asked; an older gh rejects the
# flag itself, and then the bare command is the one that works.
fetch_log() {
    local path=$1 out=$2 err=$3
    if "$gh_cmd" api --allow-escape-sequences "$path" >"$out" 2>"$err"; then
        return 0
    fi
    if grep -q "unknown flag" "$err"; then
        "$gh_cmd" api "$path" >"$out" 2>"$err"
        return
    fi
    return 1
}

# gh's own message, on one line, for a fetch that failed.
why() {
    tr '\n' ' ' <"$1" | cut -c1-300
}

failed=0
named=0
for id in $ids; do
    failed=$((failed + 1))
    if [ "$failed" -gt "$max_jobs" ]; then
        echo "ci-cause: job $id skipped: over the $max_jobs-job limit"
        continue
    fi
    payload="$work/job-$id.json"
    log="$work/job-$id.log"
    err="$work/job-$id.err"
    if ! "$gh_cmd" api "repos/$repo/actions/jobs/$id" >"$payload" 2>"$err"; then
        echo "ci-cause: job $id payload unavailable: $(why "$err")"
        continue
    fi
    if ! fetch_log "repos/$repo/actions/jobs/$id/logs" "$log" "$err"; then
        echo "ci-cause: job $id log unavailable: $(why "$err")"
        continue
    fi
    # The payload names the job; GITHUB_JOB here would name this job instead.
    name=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("name", ""))' "$payload")
    args=(cause extract --log "$log" --job-json "$payload" --job "$name" --repo "$repo")
    [ -n "$event" ] && args+=(--event "$event")
    [ -n "$pr" ] && args+=(--pr "$pr")
    if "$bin" "${args[@]}" --notice; then
        named=$((named + 1))
    else
        echo "ci-cause: job $id: shipyard cause extract failed"
    fi
done
echo "ci-cause: $failed failed job(s), $named record(s), Shipyard $SHIPYARD_VERSION"
exit 0
