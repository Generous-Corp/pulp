# Why a generation stopped, in the generator's own words.
#
#   . "$HERE/reason.sh"
#   generator_reason "$log"
#
# Every harness here reported `tail -2 | head -1` — an arbitrary line — and
# each produced something useless at least once tonight:
#
#   FAIL  the CLI did not produce a patch:   window on that machine, or
#         unlock the keychain first:            <- a mid-sentence fragment
#   FAILED to build:                 ^^^^^^     <- a traceback's caret line
#
# Neither names the problem or the fix, and both sent somebody to open the log
# by hand. The generators end with a small set of known messages, so report the
# first one that appears (with the line after it, which usually carries the
# remedy) and fall back to the tail only when none is found.
#
# One shim, shared, because two copies drift — the same reason cap.sh exists.
# Kept in step with the generators by tools/rack/test_generator_endings.py.
GENERATOR_ENDINGS="gave up after
model call failed
not logged in for this session
could not fetch the library catalog
could not fetch the module index
contract is not sound
did not contain both a json
duplicate addModel
SDK not found
two manifests claim
already running against this module pack
Traceback (most recent call last)"

generator_reason() {
    local out="$1" line marker
    while IFS= read -r marker; do
        [ -n "$marker" ] || continue
        line="$(printf '%s' "$out" | grep -m 1 -A 1 -F "$marker" | tr '\n' ' ')"
        if [ -n "$line" ]; then
            printf '%s' "$(printf '%s' "$line" | sed 's/  */ /g; s/^ //; s/ $//')"
            return
        fi
    done <<EOF
$GENERATOR_ENDINGS
EOF
    printf '%s' "$(printf '%s' "$out" | tail -2 | head -1)"
}
