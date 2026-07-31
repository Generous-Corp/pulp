#!/usr/bin/env bash
# Which copy of the generator a proof is about, and whether that is the copy
# being reviewed.
#
# There are two trees. The repo is what a person edits and what a reviewer
# reads. `~/Library/Application Support/Forge Modular` is what the app and the
# CLI actually run, and what `install_toolchain.sh` copies the repo into.
#
# They diverge in both directions. Editing the repo without reinstalling means
# a proof exercises last week's code. And the module pack is deliberately NOT
# deleted on reinstall -- generated modules land there and never come back --
# so the installed tree can know about modules the repo has never heard of.
#
# That second one is not hypothetical. A melody patch was built against an
# inventory holding SLEWRF, held its idiom, and was then re-checked against the
# repo's inventory, which had no SLEWRF manifest, so the slew stopped counting
# as a slew, the signal no longer reached the oscillator through it, and a
# correct patch was reported as not a turing machine. The patch never changed.
# The checker was asked twice and given two different racks.
#
# So: check the artifact where it was made, and say so when the two trees
# disagree, rather than letting a proof be quietly about the wrong code.
#
# Usage:  . "$(dirname "$0")/toolchain.sh"   then   toolchain_report

# Report any drift between the repo's generator and the installed one.
# Never fails the run: a proof about the installed toolchain is still a valid
# proof, as long as nobody mistakes it for a proof about the repo.
toolchain_report() { # <repo tools/rack dir> <installed tools/rack dir>
    local here="$1" tools="$2"
    if [ ! -d "$tools" ]; then
        printf 'no installed toolchain at %s\n' "$tools"
        return 0
    fi
    if [ "$(cd "$here" && pwd -P)" = "$(cd "$tools" && pwd -P)" ]; then
        return 0
    fi
    local drift=""
    local f
    for f in patch.py idiom_check.py patch_vocabulary.py generate.py; do
        [ -f "$here/$f" ] || continue
        [ -f "$tools/$f" ] || { drift="$drift $f(absent)"; continue; }
        diff -q "$here/$f" "$tools/$f" >/dev/null 2>&1 || drift="$drift $f"
    done
    local mine theirs
    mine=$(ls "$here/../../examples/forge-modular/modules"/*.json 2>/dev/null | wc -l | tr -d ' ')
    theirs=$(ls "$tools/../../examples/forge-modular/modules"/*.json 2>/dev/null | wc -l | tr -d ' ')

    printf 'generator: %s\n' "$tools"
    if [ -n "$drift" ]; then
        printf '  NOTE the installed generator differs from this checkout:%s\n' "$drift"
        printf '       this proves the INSTALLED code. Run install_toolchain.sh\n'
        printf '       first if you meant to prove the checkout.\n'
    fi
    if [ "$mine" != "$theirs" ]; then
        printf '  NOTE the module packs differ: %s manifest(s) here, %s installed.\n' \
               "$mine" "$theirs"
        printf '       Modules generated on this machine live only in the\n'
        printf '       installed pack until they are copied back.\n'
    fi
}
