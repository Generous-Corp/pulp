#!/usr/bin/env bash
# Can this session actually show a window?
#
# Usage:  . "$(dirname "$0")/session.sh"   then   if screen_is_locked; then ...
#
# A logged-in session and a session that can DISPLAY something are different
# things, and only the first is easy to test for. `pgrep loginwindow` is true
# of a locked screen too, so a driver that checks only that will happily try to
# click a window loginwindow owns, fail to focus it, and report a failure of
# the app. Behind the lock Skia cannot create a GPU context either, so the app
# floods its log with dropped draws -- which reads exactly like a rendering
# regression and is not one.

# Is the screen locked (screen saver / lock screen showing)?
#
# Deliberately NOT `ioreg … | grep -q …`. Under `set -o pipefail` -- which the
# proof scripts set -- that pipeline reports FAILURE when the pattern is FOUND:
# `grep -q` exits at the first match and closes the pipe, `ioreg` dies of
# SIGPIPE, and pipefail takes the non-zero. The match succeeds and the branch
# is skipped anyway. That cost a locked machine a FAIL where it should have
# had a skip, with the source line sitting there looking correct.
screen_is_locked() {
    local state
    state="$(ioreg -n Root -d1 2>/dev/null)" || return 1
    case "$state" in
        *'"CGSSessionScreenIsLocked"=Yes'*) return 0 ;;
        *) return 1 ;;
    esac
}
