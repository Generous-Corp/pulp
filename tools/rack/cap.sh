#!/usr/bin/env bash
# `cap SECONDS command...` — run something with a time limit, anywhere.
#
# macOS ships no `timeout`. It is GNU coreutils, so a machine that has never
# had it installed dies with "command not found" on every capped step, which
# reads as the thing being measured failing rather than the harness missing a
# tool. Sourced by prove_surfaces.sh and prove_idioms.sh so there is one shim
# rather than two that can drift.
#
# Usage:  . "$(dirname "$0")/cap.sh"   then   out="$(cap 60 some-command)"

if command -v timeout >/dev/null 2>&1; then
    cap() { timeout "$@"; }
elif command -v gtimeout >/dev/null 2>&1; then
    cap() { gtimeout "$@"; }
else
    cap() {
        local secs="$1"; shift
        "$@" &
        local job=$!
        # The killer's output goes to /dev/null, and that redirection is the
        # whole point of this line. `out="$(cap 1500 …)"` reads until EVERY
        # holder of the pipe closes it -- and a killer that inherited stdout is
        # one, so the substitution waits the full 1500 s however fast the
        # command was. On a machine with coreutils this shim never runs and the
        # bug is invisible; on one without, every capped step silently costs
        # its entire cap. That is what made a three-minute CLI proof on the M5
        # look like a hang, twice, and get abandoned both times.
        ( sleep "$secs"; kill -TERM "$job" 2>/dev/null ) >/dev/null 2>&1 &
        local killer=$!
        wait "$job" 2>/dev/null
        local rc=$?
        # `wait` after the kill, both quiet: without it the shell announces
        # "Terminated: 15" on stderr when it reaps the killer, and that line
        # lands in the middle of the evidence these scripts exist to produce.
        { kill "$killer" && wait "$killer"; } 2>/dev/null
        return "$rc"
    }
fi

# Run it as well as define it.
#
# Sourcing is the intended use, but `bash cap.sh 90 some-command` reads as the
# obvious one -- and it used to define the function, run NOTHING, and exit 0.
# A harness that measures nothing and reports success is worse than one that
# fails: a Rack patch-loading probe invoked this way came back clean with an
# empty log, which looks exactly like Rack opening the patch without complaint.
# So an executed-with-arguments invocation does the capped run it plainly means.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    if [ "$#" -lt 2 ]; then
        echo "usage: cap.sh SECONDS command...   (or: . cap.sh, then cap 60 cmd)" >&2
        exit 2
    fi
    cap "$@"
fi
