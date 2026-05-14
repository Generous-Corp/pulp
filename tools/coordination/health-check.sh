#!/usr/bin/env bash
# health-check.sh — one-screen status of A/B/C autonomous coordination.
#
# Run this when you want to know "is this working?" — fast, idempotent,
# inspects only planning history + remote PR state, never modifies anything.
#
# Verdict at the end:
#   🟢 GREEN  — all 3 agents ticking within recent window, progress visible
#   🟡 YELLOW — partial: some active, others silent
#   🔴 RED    — no agents active or coordination not yet started
#
# Usage:
#   tools/coordination/health-check.sh
#
# Tunables (env):
#   COORD_WINDOW_MIN — minutes for "active" window per agent (default 20)
#   PULP_DIR         — override pulp root

set -uo pipefail

PULP="${PULP_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && git rev-parse --show-toplevel 2>/dev/null || pwd)}"
PLAN="${PULP}/planning"
WIN="${COORD_WINDOW_MIN:-20}"
NOW="$(date -u +%FT%TZ)"

# Color helpers — only when stdout is a TTY
if [ -t 1 ]; then
  G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; B=$'\033[1m'; D=$'\033[0m'
else
  G=""; Y=""; R=""; B=""; D=""
fi

echo "${B}=== Coordination health check — ${NOW} (window=${WIN} min) ===${D}"
echo

# Refresh planning from origin so we see other agents' commits
git -C "$PLAN" fetch origin main --quiet 2>/dev/null || echo "${Y}WARN: planning fetch failed${D}"
git -C "$PLAN" pull --rebase --quiet 2>/dev/null || true

# ── 1. Halt status ────────────────────────────────────────────────────────
echo "${B}## 1. Halt status${D}"
if head -5 "$PLAN/AGENT-STATUS.md" 2>/dev/null | grep -qE "^HALT ALL"; then
  echo "  ${R}🛑 HALT ALL detected at top of AGENT-STATUS — agents should be exiting${D}"
else
  echo "  ${G}✅ no HALT — agents should be active${D}"
fi
echo

# ── 2. Last tick per agent ────────────────────────────────────────────────
echo "${B}## 2. Last planning commit per agent${D}"

last_tick() {
  local agent="$1"
  git -C "$PLAN" log --since="6 hours ago" \
    --format="%cI|%h|%s" \
    --grep="agent-${agent}:" -1 2>/dev/null
}

minutes_since() {
  local iso="$1"
  python3 -c "
from datetime import datetime, timezone
try:
    t = datetime.fromisoformat('$iso'.replace('Z', '+00:00'))
    n = datetime.now(timezone.utc)
    print(int((n - t).total_seconds() / 60))
except Exception:
    print(99999)
"
}

A_TICK=$(last_tick a)
B_TICK=$(last_tick b)
C_TICK=$(last_tick c)

for entry in "A|$A_TICK" "B|$B_TICK" "C|$C_TICK"; do
  AGENT=$(echo "$entry" | cut -d'|' -f1)
  REST=$(echo "$entry" | cut -d'|' -f2-)
  if [ -z "$REST" ]; then
    echo "  Agent ${AGENT}: ${R}❌ no commit in last 6h${D}"
    continue
  fi
  TS=$(echo "$REST" | cut -d'|' -f1)
  SHA=$(echo "$REST" | cut -d'|' -f2)
  MSG=$(echo "$REST" | cut -d'|' -f3-)
  MIN_AGO=$(minutes_since "$TS")
  if [ "$MIN_AGO" -le "$WIN" ]; then
    echo "  Agent ${AGENT}: ${G}✅ ${MIN_AGO} min ago${D} — $SHA $MSG"
  else
    echo "  Agent ${AGENT}: ${Y}⚠️  ${MIN_AGO} min ago (>$WIN min)${D} — $SHA $MSG"
  fi
done
echo

# ── 3. Recent planning commits (last 30 min) ──────────────────────────────
echo "${B}## 3. Recent planning commits (last 30 min)${D}"
COMMITS=$(git -C "$PLAN" log --since="30 min ago" --format="  %cI %an: %s" 2>/dev/null | head -8)
if [ -z "$COMMITS" ]; then
  echo "  ${Y}(none in last 30 min)${D}"
else
  echo "$COMMITS"
fi
echo

# ── 4. Open PRs from this session ─────────────────────────────────────────
echo "${B}## 4. PR status (REST API — GraphQL skipped to avoid rate limits)${D}"
for PR in 1897 1898 1900; do
  STATE=$(gh api "repos/danielraffel/pulp/pulls/$PR" \
    --jq '"state=\(.state) mergeable_state=\(.mergeable_state) updated=\(.updated_at)"' \
    2>/dev/null || echo "?? rate-limited or missing")
  echo "  #$PR: $STATE"
done
echo

# ── 5. Agent C status doc ─────────────────────────────────────────────────
echo "${B}## 5. Agent C autonomous status (planning/AGENT-C-STATUS.md)${D}"
if [ -f "$PLAN/AGENT-C-STATUS.md" ]; then
  C_DOC_AGE_MIN=$(python3 -c "
import os, time
try:
    mtime = os.path.getmtime('$PLAN/AGENT-C-STATUS.md')
    print(int((time.time() - mtime) / 60))
except Exception:
    print(99999)
")
  if [ "$C_DOC_AGE_MIN" -le "$WIN" ]; then
    echo "  ${G}✅ updated ${C_DOC_AGE_MIN} min ago${D}"
  else
    echo "  ${Y}⚠️  last updated ${C_DOC_AGE_MIN} min ago (>$WIN min — C may be idle)${D}"
  fi
  echo "  Last 4 entries:"
  tail -8 "$PLAN/AGENT-C-STATUS.md" | sed 's/^/    /'
else
  echo "  ${Y}⚠️  doc not yet created (C hasn't started or has never ticked)${D}"
  C_DOC_AGE_MIN=99999
fi
echo

# ── 6. STALLED tags + escalations ─────────────────────────────────────────
echo "${B}## 6. STALLED / escalation indicators${D}"
STALLS=$(grep -nE "STALLED ON|Round 3 missed|escalat" "$PLAN/AGENT-STATUS.md" 2>/dev/null | tail -5)
if [ -z "$STALLS" ]; then
  echo "  ${G}✅ no STALLED tags or escalation notes${D}"
else
  echo "$STALLS" | sed 's/^/  /'
fi
echo

# ── 7. Latest visual diff score (from screenshot filenames) ───────────────
echo "${B}## 7. Visual diff score trajectory${D}"
LATEST_PNG=$(ls -t "$PLAN/screenshots/"*.png 2>/dev/null | head -3)
if [ -n "$LATEST_PNG" ]; then
  echo "  3 most recent screenshots in planning/screenshots/:"
  echo "$LATEST_PNG" | while IFS= read -r f; do
    NAME=$(basename "$f")
    AGE_MIN=$(python3 -c "
import os, time
print(int((time.time() - os.path.getmtime('$f')) / 60))
")
    echo "    ${AGE_MIN}m ago — $NAME"
  done
else
  echo "  ${Y}(no screenshots yet)${D}"
fi
REF="$PLAN/screenshots/REFERENCE-spectr-editor-html.png"
LATEST=$(ls -t "$PLAN/screenshots/spectr-"*.png 2>/dev/null | head -1)
if [ -f "$REF" ] && [ -f "$LATEST" ] && [ -x "$(command -v python3)" ]; then
  echo "  Current score (vs locked REFERENCE):"
  python3 "$PULP/tools/import-validation/diff_against_reference.py" \
    "$REF" "$LATEST" --threshold 0.85 2>/dev/null | head -3 | sed 's/^/    /'
fi
echo

# ── 8. Verdict ────────────────────────────────────────────────────────────
echo "${B}## 8. Verdict${D}"

# An agent is "active" if it ticked within COORD_WINDOW_MIN
agent_active() {
  local tick_line="$1"
  [ -z "$tick_line" ] && return 1
  local ts; ts=$(echo "$tick_line" | cut -d'|' -f1)
  local ago; ago=$(minutes_since "$ts")
  [ "$ago" -le "$WIN" ]
}

A_OK=0; B_OK=0; C_OK=0
agent_active "$A_TICK" && A_OK=1
agent_active "$B_TICK" && B_OK=1
[ "$C_DOC_AGE_MIN" -le "$WIN" ] && C_OK=1

TOTAL=$((A_OK + B_OK + C_OK))

if [ "$TOTAL" -eq 3 ]; then
  echo "  ${G}🟢 GREEN — ALL THREE agents ticking within last ${WIN} min, coordination flowing${D}"
elif [ "$TOTAL" -ge 1 ]; then
  echo "  ${Y}🟡 YELLOW — partial:${D}"
  [ "$A_OK" -eq 0 ] && echo "    ${R}❌ Agent A silent (no tick in last ${WIN} min)${D}"
  [ "$B_OK" -eq 0 ] && echo "    ${R}❌ Agent B silent (no tick in last ${WIN} min)${D}"
  [ "$C_OK" -eq 0 ] && echo "    ${R}❌ Agent C silent (AGENT-C-STATUS.md not updated in last ${WIN} min)${D}"
else
  echo "  ${R}🔴 RED — no agents active. Either coordination not yet started, all sessions died, or HALT was issued.${D}"
fi

echo
echo "${B}=== Done ===${D}"
