#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <fixture-runner> <playback-runner> <corpus-dir>" >&2
    exit 2
fi

fixture_runner=$1
playback_runner=$2
corpus_dir=$3
remote_root=/data/local/tmp/pulp-android-fixtures

for path in "$fixture_runner" "$playback_runner" "$corpus_dir/corpus.index"; do
    [[ -e "$path" ]] || { echo "missing input: $path" >&2; exit 2; }
done

adb shell "rm -rf '$remote_root' && mkdir -p '$remote_root'"
adb push "$fixture_runner" "$remote_root/pulp-fixture-runner" >/dev/null
adb push "$playback_runner" "$remote_root/pulp-android-fixture-runner" >/dev/null
adb push "$corpus_dir" "$remote_root/corpus" >/dev/null
adb shell "chmod 755 '$remote_root/pulp-fixture-runner' '$remote_root/pulp-android-fixture-runner'"

run_remote() {
    local binary=$1
    local required_marker=$2
    local output marker status
    marker="__PULP_REMOTE_EXIT_${RANDOM}_${RANDOM}__"
    output=$(adb shell "'$remote_root/$binary' --corpus '$remote_root/corpus'; status=\$?; printf '\n${marker}%s\n' \"\$status\"" 2>&1) || {
        printf '%s\n' "$output" >&2
        echo "adb transport failed for $binary" >&2
        return 1
    }
    printf '%s\n' "$output"
    status=$(printf '%s\n' "$output" | sed -n "s/^${marker}\([0-9][0-9]*\)\r\{0,1\}$/\1/p" | tail -1)
    [[ -n "$status" ]] || { echo "missing remote exit marker for $binary" >&2; return 1; }
    [[ "$status" == 0 ]] || { echo "$binary returned $status" >&2; return "$status"; }
    printf '%s\n' "$output" | grep -Fq "$required_marker" || {
        echo "$binary omitted pass marker: $required_marker" >&2
        return 1
    }
}

run_remote pulp-fixture-runner "pulp-fixture-runner:"
echo "PASS android-timeline-round-trip"
run_remote pulp-android-fixture-runner "PASS android-playback-render"

# Causal control: mutate only the device copy of the golden. Structural corpus
# validation stays green while the playback oracle must reject the bad stream.
adb shell "printf '00\n' > '$remote_root/corpus/v1/replay-render.golden'"
run_remote pulp-fixture-runner "pulp-fixture-runner:"
set +e
control_output=$(run_remote pulp-android-fixture-runner "PASS android-playback-render" 2>&1)
control_status=$?
set -e
printf '%s\n' "$control_output"
if [[ "$control_status" -ne 1 ]] ||
   ! grep -Fq "FAIL android-playback-fixtures: audio and MIDI stream differs from replay-render golden" \
       <<<"$control_output"; then
    echo "negative control did not fail through the playback golden oracle" >&2
    exit 1
fi
echo "PASS android-playback-negative-control"

adb push "$corpus_dir/v1/replay-render.golden" \
    "$remote_root/corpus/v1/replay-render.golden" >/dev/null
run_remote pulp-android-fixture-runner "PASS android-playback-render"
echo "PASS android-fixture-lane"
