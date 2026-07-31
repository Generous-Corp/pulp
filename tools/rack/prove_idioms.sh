#!/usr/bin/env bash
# Build a patch for each of a dozen prompts and report what its idiom check said.
#
# The library's self-test proves the idioms can fail. This proves the whole
# path: a prompt resolves to an idiom, the model is told what that idiom
# requires, and the patch that comes back satisfies it. Those are different
# claims, and only this one is about the product.
#
# Four of the prompts IMPLY an idiom without naming it. That half goes untested
# whenever the examples are written by someone who already knows the vocabulary,
# which is everyone who writes the examples.
#
#   tools/rack/prove_idioms.sh              # all of them, sequentially
#   tools/rack/prove_idioms.sh 3            # just the third
#
# Each build costs a model call and a few minutes; the whole run is long by
# design and prints as it goes, so a failure is visible before the end.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="${FORGE_MODULAR_TOOLS:-$HOME/Library/Application Support/Forge Modular/tools/rack}"
OUT="${PROVE_OUT:-/tmp/forge-idiom-proof.txt}"

# family, prompt, how the idiom is reached
PROMPTS=(
  "generative|a krell patch where each note chooses the next one's length|named"
  "generative|an evolving ambient drone that never settles|named"
  "generative|a melody that repeats then slowly mutates|implied"
  "rhythm|a bouncing ball rhythm that slows down as it settles|named"
  "rhythm|a kick drum with a fast pitch sweep|named"
  "rhythm|four different speeds coming from one clock|implied"
  "voice|a classic subtractive voice with a filter envelope|named"
  "voice|a west coast voice through a low pass gate|named"
  "voice|a bass sound that stays in the key of C|implied"
  "modulation|slow vibrato on a sustained note|named"
  "texture|wind and rain that keeps shifting|implied"
  "texture|two detuned oscillators beating against each other|named"
)

only="${1:-}"
: > "$OUT"
pass=0; fail=0; n=0

for entry in "${PROMPTS[@]}"; do
    n=$((n + 1))
    [ -n "$only" ] && [ "$only" != "$n" ] && continue
    IFS='|' read -r family prompt how <<< "$entry"

    slug="$(cd "$HERE" && python3 -c "
import sys; sys.path.insert(0,'.')
from idiom_check import resolve
print(resolve('''$prompt''') or '')
")"
    if [ -z "$slug" ]; then
        printf '%2d. %-9s %-52s NO IDIOM RESOLVED\n' "$n" "$family" "${prompt:0:52}" | tee -a "$OUT"
        fail=$((fail + 1))
        continue
    fi

    printf '%2d. %-9s %-52s -> %s (%s)\n' "$n" "$family" "${prompt:0:52}" "$slug" "$how" | tee -a "$OUT"
    log="$(cd "$TOOLS" && timeout 1500 python3 patch.py build "$prompt" 2>&1)"

    # The generator prints the verdict; the artifact is checked again here
    # rather than trusted, because "built" and "holds" are separate claims and
    # this project has had them disagree.
    path="$(printf '%s' "$log" | sed -n 's/.*cables → \(.*\)$/\1/p' | tail -1)"
    if [ -z "$path" ] || [ ! -f "$path" ]; then
        printf '    FAILED to build: %s\n' "$(printf '%s' "$log" | tail -2 | head -1)" | tee -a "$OUT"
        fail=$((fail + 1))
        continue
    fi
    verdict="$(cd "$HERE" && python3 idiom_check.py "$path" "$slug" 2>&1)"
    if printf '%s' "$verdict" | grep -q "holds"; then
        printf '    HOLDS  %s\n' "$(basename "$path")" | tee -a "$OUT"
        pass=$((pass + 1))
    else
        printf '%s\n' "$verdict" | sed 's/^/    /' | tee -a "$OUT"
        fail=$((fail + 1))
    fi
done

printf '\n%d held, %d did not\n' "$pass" "$fail" | tee -a "$OUT"
[ "$fail" -eq 0 ]
