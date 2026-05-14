#!/usr/bin/env bash
# check_label_coverage.sh — structural assertion: does the importer's IR
# capture the same UI text strings the canonical browser render shows?
#
# Usage:
#   check_label_coverage.sh <generated-ui.js> <reference-labels.txt> [--min-coverage 0.70]
#
# The reference-labels.txt is one expected label per line. Lines starting
# with `#` are ignored (comments). For Spectr's editor.html, the canonical
# reference list was captured from the Chrome accessibility tree at
# 1320x860 viewport — see planning/spectr-reimport-validation-plan.md.
#
# Exit codes:
#   0  coverage ≥ threshold (PASS)
#   1  coverage below threshold (FAIL — importer regression or shim gap)
#   2  inputs missing or malformed

set -euo pipefail

UIJS="${1:-}"
REF="${2:-}"
THRESHOLD=0.70
if [[ "${3:-}" == "--min-coverage" && -n "${4:-}" ]]; then
  THRESHOLD="$4"
fi

if [[ -z "$UIJS" || -z "$REF" ]]; then
  echo "usage: $0 <generated-ui.js> <reference-labels.txt> [--min-coverage 0.70]" >&2
  exit 2
fi
[[ -f "$UIJS" ]] || { echo "ERROR: missing $UIJS" >&2; exit 2; }
[[ -f "$REF" ]]  || { echo "ERROR: missing $REF" >&2; exit 2; }

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yel()   { printf '\033[33m%s\033[0m\n' "$*"; }

# Extract labels the importer wrote (createLabel('id', 'text', ...) syntax)
captured="$(mktemp)"
trap "rm -f $captured" EXIT
grep -oE "createLabel\('[^']+', '[^']*'" "$UIJS" \
  | sed -E "s/createLabel\('[^']+', '//; s/'$//" \
  > "$captured"

ref_total=0
hit=0
fuzzy=0
miss_list=()
while IFS= read -r expected; do
  # Skip empty lines and comments
  [[ -z "$expected" || "$expected" == \#* ]] && continue
  ref_total=$((ref_total + 1))
  if grep -Fxq "$expected" "$captured"; then
    hit=$((hit + 1))
  elif [[ ${#expected} -ge 4 ]] && grep -Fq "$expected" "$captured"; then
    # Codex P1 follow-up on PR #1847: limit the substring fallback to
    # labels of >= 4 characters. Short labels like "A" / "B" / "?" /
    # "⋯" would otherwise match as substrings inside unrelated longer
    # labels ("▸ A", "BANDS", etc), inflating coverage and letting
    # missing controls slip past the 70% gate. Exact-match path
    # (grep -Fxq above) still works for every length — short labels
    # are detected, just not via substring promotion.
    fuzzy=$((fuzzy + 1))
    hit=$((hit + 1))
  else
    miss_list+=("$expected")
  fi
done < "$REF"

if [[ $ref_total -eq 0 ]]; then
  red "ERROR: no labels in $REF"
  exit 2
fi

coverage=$(awk -v h="$hit" -v t="$ref_total" 'BEGIN{printf "%.3f", h/t}')

echo "Reference labels: $ref_total"
echo "Captured exact:   $((hit - fuzzy))"
echo "Captured fuzzy:   $fuzzy"
echo "Coverage:         $coverage  (threshold $THRESHOLD)"

if (( $(awk -v c="$coverage" -v t="$THRESHOLD" 'BEGIN{print (c<t)}') )); then
  red "✗ FAIL — coverage $coverage < threshold $THRESHOLD"
  echo
  echo "Missing labels:"
  for m in "${miss_list[@]}"; do
    echo "  - $m"
  done
  exit 1
else
  green "✓ PASS — coverage $coverage ≥ threshold $THRESHOLD"
  if [[ ${#miss_list[@]} -gt 0 ]]; then
    yel "  (still missing: ${#miss_list[@]} labels, listed below)"
    for m in "${miss_list[@]}"; do
      echo "    - $m"
    done
  fi
  exit 0
fi
