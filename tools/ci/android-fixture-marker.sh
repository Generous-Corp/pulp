#!/usr/bin/env bash
# Marker check shared by android-fixture-lane.sh and its selftest.

# True when the captured runner output contains the required pass marker.
#
# This is a shell substring test, not `printf | grep -q`. grep -q exits at the
# first match, so on large output printf hits a broken pipe, and under
# pipefail that turned a passing run into "omitted pass marker".
output_has_marker() {
    local output=$1
    local marker=$2
    [[ "$output" == *"$marker"* ]]
}
