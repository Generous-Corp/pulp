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
# Evidence outlives the run, so it does not live in a directory the OS clears.
#
# The last run's per-prompt transcripts went to a scratchpad under /tmp. The
# summary that survived cites them by path, and they are gone -- so the one
# record of what twelve model calls actually did is a file pointing at nothing.
# Each run costs a dozen model calls and the better part of an hour; that is
# not evidence to leave somewhere temporary.
RUNS="$HERE/patch_idioms/regressions"
OUT="${PROVE_OUT:-$RUNS/idiom-proof.txt}"

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

# `cap SECONDS command...` -- a time limit that works on a machine
# with no coreutils. One shim, shared, because two copies drift.
. "$HERE/cap.sh"

# Why a generation stopped, shared with prove_surfaces.sh.
. "$HERE/reason.sh"

# Which generator this proof is about, and whether it is the one being read.
. "$HERE/toolchain.sh"
toolchain_report "$HERE" "$TOOLS"

only="${1:-}"
LOGS="${PROVE_LOGS:-${OUT%.txt}-logs}"
mkdir -p "$LOGS"
: > "$OUT"
pass=0; fail=0; n=0
silent_total=0; lint_total=0; idiom_total=0

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
    # Each failed attempt's patch and full activity report land here, so a
    # silent run can be diagnosed from what it produced rather than by paying
    # for another one.
    log="$(cd "$TOOLS" && FORGE_ATTEMPT_DIR="$LOGS/$(printf '%02d' "$n")-attempts" \
           cap 1500 python3 patch.py build "$prompt" 2>&1)"

    # The generator prints the verdict; the artifact is checked again here
    # rather than trusted, because "built" and "holds" are separate claims and
    # this project has had them disagree.
    # The whole log, kept. Only the last two lines used to survive, which is
    # how the previous run's summary came to say four prompts died at the audio
    # gate when its own transcript showed two -- the reason printed is the
    # LAST attempt's, and the earlier attempts had already scrolled away.
    printf '%s\n' "$log" > "$LOGS/$(printf '%02d' "$n").log"

    path="$(printf '%s' "$log" | sed -n 's/.*cables → \(.*\)$/\1/p' | tail -1)"
    if [ -z "$path" ] || [ ! -f "$path" ]; then
        # Which gate stopped it, counted from the transcript rather than
        # described from memory. A silent patch and a wrongly wired one are
        # different bugs with different fixes.
        audio=$(printf '%s' "$log" | grep -c "makes no sound")
        wiring=$(printf '%s' "$log" | grep -c "^  rejected (attempt")
        wrong=$(printf '%s' "$log" | grep -c "not a .* patch yet")
        printf '    FAILED to build: %s\n' "$(generator_reason "$log")" | tee -a "$OUT"
        printf '        attempts stopped by: %s silent, %s rejected by the lint, %s wrong idiom\n' \
               "$audio" "$wiring" "$wrong" | tee -a "$OUT"
        silent_total=$((silent_total + audio))
        lint_total=$((lint_total + wiring))
        idiom_total=$((idiom_total + wrong))
        fail=$((fail + 1))
        continue
    fi
    verdict="$(cd "$TOOLS" && python3 idiom_check.py "$path" "$slug" 2>&1)"
    if printf '%s' "$verdict" | grep -q "holds"; then
        printf '    HOLDS  %s\n' "$(basename "$path")" | tee -a "$OUT"
        pass=$((pass + 1))
    else
        printf '%s\n' "$verdict" | sed 's/^/    /' | tee -a "$OUT"
        fail=$((fail + 1))
    fi
done

printf '\n%d held, %d did not\n' "$pass" "$fail" | tee -a "$OUT"
printf 'across every attempt of the runs that failed: %d silent, %d rejected by the lint, %d wrong idiom\n' \
       "$silent_total" "$lint_total" "$idiom_total" | tee -a "$OUT"
printf 'full transcripts: %s\n' "$LOGS" | tee -a "$OUT"
[ "$fail" -eq 0 ]
