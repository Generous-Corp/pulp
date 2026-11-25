#!/usr/bin/env bash
# Install (or remove) the daily lane reaper LaunchAgent.
#
#   install-lane-reaper-launchagent.sh --root <lanes dir> [--act]   install
#   install-lane-reaper-launchagent.sh --print --root <lanes dir>    render only
#   install-lane-reaper-launchagent.sh --uninstall                   remove
#
# Without --act the installed job is a dry run: it logs what it would reclaim
# to ~/Library/Logs/pulp-lane-reaper.log and deletes nothing. With --act it
# passes --yes to tools/scripts/clean_lane_builds.py. The job runs the reaper
# from the checkout that holds this script, so install from a checkout that
# tracks main.
set -euo pipefail

LABEL="com.danielraffel.pulp.lane-reaper"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECKOUT="$(cd "$HERE/../.." && pwd)"
TEMPLATE="$HERE/$LABEL.plist.template"
DEST="${PULP_LANE_REAPER_PLIST_DIR:-$HOME/Library/LaunchAgents}/$LABEL.plist"

root="" act=0 print_only=0 uninstall=0
while [ $# -gt 0 ]; do
  case "$1" in
    --root) root="${2:?--root needs a directory}"; shift 2 ;;
    --act) act=1; shift ;;
    --print) print_only=1; shift ;;
    --uninstall) uninstall=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ "$uninstall" = 1 ]; then
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  rm -f "$DEST"
  echo "removed $LABEL"
  exit 0
fi

[ -n "$root" ] || { echo "--root is required" >&2; exit 2; }
[ -d "$root" ] || { echo "root $root is not a directory" >&2; exit 2; }
root="$(cd "$root" && pwd)"
case "$root:$CHECKOUT:$HOME" in *'|'*) echo "paths must not contain '|'" >&2; exit 2 ;; esac

act_arg='<string>--yes</string>'
render() {
  if [ "$act" = 1 ]; then
    sed -e "s|\$ACT_ARG|$act_arg|g"
  else
    sed -e '/\$ACT_ARG/d'
  fi | sed -e "s|\\\$HOME|$HOME|g" -e "s|\\\$PULP_CHECKOUT|$CHECKOUT|g" \
           -e "s|\\\$LANES_ROOT|$root|g"
}

if [ "$print_only" = 1 ]; then
  render < "$TEMPLATE"
  exit 0
fi

mkdir -p "$(dirname "$DEST")" "$HOME/Library/Logs"
render < "$TEMPLATE" > "$DEST"
plutil -lint "$DEST" >/dev/null
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$DEST"
mode="dry run (logs only)"; [ "$act" = 1 ] && mode="ACTING (--yes)"
echo "installed $LABEL for $root, daily 04:30, $mode"
