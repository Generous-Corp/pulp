#!/usr/bin/env bash
# The app surface's two preconditions: where the app is, and whether the
# screen can show it.
#
# Both were wrong in ways that produced a confident, misleading answer.
# The location check looked only in /Applications while the app installs to
# ~/Applications, so on a machine that HAD the app this step reported "not
# installed here" and skipped. The session check was `pgrep loginwindow`,
# which is true of a LOCKED screen too -- so the step ran, could not focus a
# window loginwindow owns, and reported FAIL. Behind the lock Skia also cannot
# make a GPU context, so the app floods the log with dropped draws, which
# reads as a rendering regression and is not one.
#
# A skip misreported as a failure sends someone to debug working code. These
# assert the script's decisions without launching anything.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
S="$HERE/prove_surfaces.sh"
bad=0
ok()    { printf '  ok     %s\n' "$*"; }
wrong() { printf '  WRONG  %s\n' "$*"; bad=$((bad + 1)); }

grep -q 'HOME/Applications/Forge Modular.app' "$S" \
    && ok "the app is looked for in ~/Applications, where it installs" \
    || wrong "only /Applications is searched — the app step will skip itself
         on a machine that has the app"

# Behaviour, not source text. Asserting the LINE was present passed happily
# while the check was broken: under `set -o pipefail` the original
# `ioreg | grep -q` reported failure precisely WHEN it matched, because grep -q
# closes the pipe and ioreg dies of SIGPIPE. A test that reads the script
# cannot see that. So the predicate is executed, under the same shell options
# the proof scripts use, against a fake ioreg that says what we choose.
. "$HERE/session.sh"
FAKE="$(mktemp -d)"
trap 'rm -rf "$FAKE"' EXIT
make_ioreg() {
    # The quotes around the key are part of ioreg's output and part of what is
    # matched, so they have to survive into the stub. Passing the string
    # through printf's format unquoted let the stub's own shell strip them,
    # and the "locked" fixture then produced UNLOCKED-looking output -- a
    # fixture that disagrees with the real thing tests nothing.
    {
        echo '#!/bin/sh'
        echo "cat <<'IOREG_EOF'"
        echo "$1"
        # Then a LOT more, because the size is load-bearing. The bug being
        # guarded against is `ioreg | grep -q`: grep exits at the first match
        # and closes the pipe, ioreg dies of SIGPIPE, and pipefail takes the
        # non-zero -- so the pipeline fails precisely when the pattern is
        # found. A one-line stub never triggers it, because the writer finishes
        # before the reader leaves. With a short fixture this whole test passed
        # against the broken implementation.
        echo 'IOREG_EOF'
        echo "yes 'padding so the writer is still writing when grep leaves' \\"
        echo "  | head -200000"
    } > "$FAKE/ioreg"
    chmod +x "$FAKE/ioreg"
}

# The fixture has to look like ioreg really does, or the predicate is being
# tested against a string this code will never see.
make_ioreg '  "IOConsoleUsers" = ({"kCGSSessionOnConsoleKey"=Yes,"CGSSessionScreenIsLocked"=Yes,"kCGSSessionUserIDKey"=501})'
[ "$(PATH="$FAKE:$PATH" ioreg -n Root -d1)" = "$(PATH="$FAKE:$PATH" ioreg)" ] \
    && ok "the fake ioreg ignores its arguments, as a stand-in must" \
    || wrong "the fake ioreg is argument-sensitive — it is not standing in"

( set -uo pipefail
  PATH="$FAKE:$PATH"
  make_ioreg '  "IOConsoleUsers" = ({"kCGSSessionOnConsoleKey"=Yes,"CGSSessionScreenIsLocked"=Yes})'
  screen_is_locked ) \
    && ok "a locked screen is detected, under pipefail" \
    || wrong "screen_is_locked said NO on a locked screen — the app step will
         run, fail to focus, and report a FAIL for a machine that is
         merely locked"

( set -uo pipefail
  PATH="$FAKE:$PATH"
  make_ioreg '  "IOConsoleUsers" = ({"kCGSSessionOnConsoleKey"=Yes,"CGSSessionScreenIsLocked"=No})'
  screen_is_locked ) \
    && wrong "screen_is_locked said YES on an UNLOCKED screen — every app run
         would skip itself and never prove anything" \
    || ok "an unlocked screen is not mistaken for a locked one"

grep -q 'screen_is_locked' "$S" \
    && ok "the app step consults it" \
    || wrong "nothing in the proof checks whether the screen is locked"

# The lock branch must SKIP, not fail. `none` is the skip reporter; `bad` is
# the failure reporter. Reporting a locked screen as a failure is the bug.
lock_branch="$(awk '/screen_is_locked/{f=1} f&&/^ *(none|bad) /{print $1; exit}' "$S")"
[ "$lock_branch" = "none" ] \
    && ok "a locked screen is reported as a skip, not a failure" \
    || wrong "the locked-screen branch reports '$lock_branch', not 'none' —
         a skip reported as a failure sends someone to debug working code"

# And the launch must use whichever path was found, not a hardcoded one.
grep -q 'open -a "\$APP"' "$S" \
    && ok "the app that was found is the app that is launched" \
    || wrong "the launch ignores the located app and opens a fixed path"

# The reason a generation stopped, in the generator's own words.
#
# Every harness reported `tail -2 | head -1`, and each produced something
# useless at least once: a mid-sentence fragment for a login failure, and a
# traceback's bare caret line for a crash. Both sent somebody to open the log
# by hand, which is the one thing a verdict is supposed to save.
. "$HERE/reason.sh"

crash_log='  audio out: Speakers
Traceback (most recent call last):
  File "patch.py", line 1437, in generate
    keep_attempt(patch, report, attempt + 1, "rejected")
                        ^^^^^^
UnboundLocalError: cannot access local variable'
crash_reason="$(generator_reason "$crash_log")"
case "$crash_reason" in
    *Traceback*patch.py*) ok "a crash is reported as the traceback, not its caret" ;;
    *) wrong "a crash reported as '$crash_reason' — the caret line names
         nothing, and this is what it did" ;;
esac

login_log='model call failed: the model CLI is not logged in for this session.
  It said: Not logged in
  window on that machine, or unlock the keychain first:'
login_reason="$(generator_reason "$login_log")"
case "$login_reason" in
    *"not logged in for this session"*) ok "a login failure names the cause" ;;
    *) wrong "a login failure reported as '$login_reason'" ;;
esac

# And nothing recognisable still says something, rather than nothing.
plain_reason="$(generator_reason 'thinking
something unremarkable
the last line')"
[ -n "$plain_reason" ] \
    && ok "an unrecognised ending still reports the tail" \
    || wrong "an unrecognised ending reported nothing at all"

printf '\n%s\n' "$([ "$bad" -eq 0 ] && echo 'all good' || echo FAILED)"
[ "$bad" -eq 0 ]
