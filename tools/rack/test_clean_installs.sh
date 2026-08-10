#!/usr/bin/env bash
# What clean_installs.sh must and must not delete.
#
# It runs against a fake HOME, so the real one is never touched by the test --
# and the assertions that matter are the NEGATIVE ones: a cleaner that removed
# everything would pass any check that only looks for the litter being gone.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$HERE/clean_installs.sh"
FAKE="$(mktemp -d)"
trap 'rm -rf "$FAKE"' EXIT

fail=0
# Two helpers rather than one that takes an operator. The single-helper version
# was called as `check "desc" ! -e "$path"`, which lands in the script as
# `[ "!" "-e" "$path" ]` -- a test of the literal string "!", always false. It
# reported five deletions as FAILED that had in fact happened.
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }
survives() { [ -d "$2" ] && ok "$1" || bad "$1"; }
gone()     { [ -e "$2" ] && bad "$1" || ok "$1"; }

mk() { mkdir -p "$1"; echo x > "$1/marker"; }

mk "$FAKE/Applications/Forge Modular.app"
mk "$FAKE/Applications/Forge Modular.app.prev"
mk "$FAKE/Applications/Forge Modular.app.backup-20260731-1219"
mk "$FAKE/Library/Audio/Plug-Ins/VST3/Forge Modular.vst3"
mk "$FAKE/Library/Audio/Plug-Ins/VST3/Forge Modular.vst3.prev"
mk "$FAKE/Library/Audio/Plug-Ins/CLAP/Forge Modular.clap"
mk "$FAKE/Library/Audio/Plug-Ins/CLAP/Forge Modular.clap.signed-backup"
mk "$FAKE/Library/Audio/Plug-Ins/Components/Forge Modular.component"
mk "$FAKE/Library/Audio/Plug-Ins/Components/Forge Modular.component.backup-20260731-1054"
# Things it has no business touching.
mk "$FAKE/Library/Audio/Plug-Ins/VST3/Some Other Plugin.vst3"
mk "$FAKE/Library/Application Support/Forge Modular/examples"
mk "$FAKE/Library/Application Support/Forge Modular/projects"

echo "1. dry run changes nothing"
out=$(HOME="$FAKE" bash "$SCRIPT")
survives "the dry run kept the stale copy" "$FAKE/Applications/Forge Modular.app.prev"
case "$out" in
    *"dry run"*) printf '  ok    it says it is a dry run\n' ;;
    *)           printf '  FAIL  a dry run that does not say so\n'; fail=1 ;;
esac
case "$out" in
    *"shadows"*) printf '  ok    it explains WHY ~/Applications goes\n' ;;
    *)           printf '  FAIL  ~/Applications removed without a reason given\n'; fail=1 ;;
esac

echo "2. --yes removes exactly the litter"
HOME="$FAKE" bash "$SCRIPT" --yes > /dev/null
gone "the .prev copy is gone" "$FAKE/Applications/Forge Modular.app.prev"
gone "the .backup copy is gone" "$FAKE/Applications/Forge Modular.app.backup-20260731-1219"
gone "the .signed-backup is gone" "$FAKE/Library/Audio/Plug-Ins/CLAP/Forge Modular.clap.signed-backup"
gone "the VST3 .prev is gone" "$FAKE/Library/Audio/Plug-Ins/VST3/Forge Modular.vst3.prev"
gone "the shadowing app copy is gone" "$FAKE/Applications/Forge Modular.app"

echo "3. and nothing else — the assertions that actually matter"
survives "the installed VST3 survives" "$FAKE/Library/Audio/Plug-Ins/VST3/Forge Modular.vst3"
survives "the installed CLAP survives" "$FAKE/Library/Audio/Plug-Ins/CLAP/Forge Modular.clap"
survives "the installed AU survives" "$FAKE/Library/Audio/Plug-Ins/Components/Forge Modular.component"
survives "another vendor's plugin survives" "$FAKE/Library/Audio/Plug-Ins/VST3/Some Other Plugin.vst3"
survives "the module pack survives" "$FAKE/Library/Application Support/Forge Modular/examples"
survives "the user's projects survive" "$FAKE/Library/Application Support/Forge Modular/projects"

echo "4. a clean machine says so rather than reporting work"
out=$(HOME="$FAKE" bash "$SCRIPT")
case "$out" in
    *"nothing to clean"*) printf '  ok    second run is a no-op\n' ;;
    *) printf '  FAIL  it did not report an already-clean machine\n'; fail=1 ;;
esac

[ "$fail" -eq 0 ] && echo "PASS" || echo "FAILED"
exit "$fail"
