#!/bin/bash
# Uninstall everything a Pulp combined installer placed on this machine.
#
# Ships INSIDE a standalone app (Contents/Resources/) next to the manifest the
# installer generated for that release, so a user who has the app has the
# uninstaller — no second artifact to find, download, or keep in sync.
#
# It removes what the MANIFEST says and nothing else. The alternative — globbing
# for likely-looking names under /Library/Audio/Plug-Ins — deletes another
# vendor's plugin the day two products share a word, and that is not a mistake
# you get to apologise for. Every path is additionally checked against an
# allowlist of roots before removal, so a corrupted or hand-edited manifest
# still cannot reach outside the places an installer is allowed to write.
#
# Usage:
#   pulp_uninstall.sh [--manifest PATH] [--yes] [--dry-run]
#
# Removing from /Library needs root, so the script re-executes itself under
# sudo once it knows there is something to do — asking for a password before
# showing the user what will happen is backwards.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MANIFEST="$HERE/uninstall-manifest.txt"
ASSUME_YES=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest) MANIFEST="$2"; shift 2;;
    --yes|-y) ASSUME_YES=1; shift;;
    --dry-run) DRY_RUN=1; shift;;
    -h|--help) sed -n '2,20p' "$0"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

[[ -f "$MANIFEST" ]] || { echo "no uninstall manifest at: $MANIFEST" >&2; exit 2; }

PRODUCT="$(sed -n 's/^product: //p' "$MANIFEST" | head -1)"
VERSION="$(sed -n 's/^version: //p' "$MANIFEST" | head -1)"
[[ -n "$PRODUCT" ]] || PRODUCT="this Pulp product"

# Roots an installer is allowed to write, and therefore the only roots an
# uninstaller may remove from.
allowed_root() {
  case "$1" in
    /Library/Audio/Plug-Ins/*) return 0;;
    /Applications/*) return 0;;
    "$HOME"/Library/Application\ Support/*) return 0;;
    *) return 1;;
  esac
}

PATHS=()
RECEIPTS=()
while IFS= read -r line; do
  case "$line" in
    path:*)
      p="${line#path: }"
      if allowed_root "$p"; then PATHS+=("$p")
      else echo "refusing path outside an installable root: $p" >&2; fi;;
    receipt:*) RECEIPTS+=("${line#receipt: }");;
  esac
done < "$MANIFEST"

# Only mention what is actually still there, so a second run reads "nothing to
# remove" rather than listing files it is about to fail to delete.
PRESENT=()
for p in "${PATHS[@]:-}"; do [[ -n "$p" && -e "$p" ]] && PRESENT+=("$p"); done

echo "Uninstall $PRODUCT${VERSION:+ $VERSION}"
echo
if [[ "${#PRESENT[@]}" -eq 0 ]]; then
  echo "Nothing left to remove — the files are already gone."
else
  echo "This will remove:"
  for p in "${PRESENT[@]}"; do echo "  $p"; done
fi
echo

if [[ "$DRY_RUN" == 1 ]]; then
  echo "(dry run — nothing was removed)"
  exit 0
fi
if [[ "${#PRESENT[@]}" -eq 0 && "${#RECEIPTS[@]}" -eq 0 ]]; then exit 0; fi

if [[ "$ASSUME_YES" != 1 ]]; then
  read -r -p "Remove these? [y/N] " reply
  case "$reply" in [yY]*) ;; *) echo "Cancelled."; exit 0;; esac
fi

# Re-exec under sudo now that the user has seen the list and agreed. Passing
# --yes forward so they are not asked twice for the same decision.
if [[ "$(id -u)" != 0 ]]; then
  echo "Administrator access is needed to remove files from /Library."
  exec sudo "$0" --manifest "$MANIFEST" --yes
fi

failed=0
for p in "${PRESENT[@]:-}"; do
  [[ -n "$p" ]] || continue
  if rm -rf "$p"; then echo "removed  $p"; else echo "FAILED   $p" >&2; failed=1; fi
done

# Forget the receipts last: while they exist, macOS still believes the product
# is installed, and a reinstall over a half-removed state behaves oddly.
for r in "${RECEIPTS[@]:-}"; do
  [[ -n "$r" ]] || continue
  pkgutil --forget "$r" >/dev/null 2>&1 && echo "forgot   $r"
done

echo
if [[ "$failed" == 0 ]]; then echo "$PRODUCT has been removed."
else echo "Some files could not be removed — see the errors above." >&2; fi
exit "$failed"
