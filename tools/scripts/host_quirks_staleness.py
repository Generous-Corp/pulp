#!/usr/bin/env python3
"""host_quirks_staleness.py — flag host-quirk catalog entries due for re-review.

Reads ``core/format/host-quirks.json`` and reports two kinds of staleness
(host-quirks enforcement plan, P4):

  * ``Speculative`` / ``LessonOnly`` entries whose ``last_verified`` date is
    older than N months — they were never bench-confirmed and may have
    rotted (a host shipped a fix, or the symptom changed).
  * ``Validated`` entries carrying an ``affected_versions`` expression — a
    prompt to re-verify them against the host's *current* major, since a
    DAW that fixed the underlying bug makes the accommodation dead weight.

This is a **preview/report tool** by default: it prints findings and exits 0
(advisory). The CI workflow that wraps it decides whether to open tracking
issues. Detection is a pure function (``stale_entries``) so it unit-tests
without touching the clock or the network.

Usage:
    python3 tools/scripts/host_quirks_staleness.py [--catalog PATH]
        [--now YYYY-MM-DD] [--months N] [--json]
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
DEFAULT_CATALOG = REPO_ROOT / "core" / "format" / "host-quirks.json"
DEFAULT_STALE_MONTHS = 6

# Tiers that go stale on age (never bench-confirmed → may have rotted).
AGE_STALE_TIERS = {"Speculative", "LessonOnly"}


def _parse_date(s: str) -> _dt.date | None:
    """Parse a YYYY-MM-DD date; return None if absent/unparseable."""
    if not s:
        return None
    try:
        return _dt.date.fromisoformat(s.strip()[:10])
    except ValueError:
        return None


def _months_between(earlier: _dt.date, later: _dt.date) -> int:
    """Whole months from `earlier` to `later` (calendar-based, floored)."""
    months = (later.year - earlier.year) * 12 + (later.month - earlier.month)
    if later.day < earlier.day:
        months -= 1
    return max(0, months)


def stale_entries(quirks: list[dict], now: _dt.date, months: int) -> list[dict]:
    """Return the catalog entries due for re-review, with a reason.

    Pure function — no I/O, no clock. Each result is the original entry plus
    ``_staleness`` = {"kind", "reason", "age_months"}.
    """
    out: list[dict] = []
    for q in quirks:
        tier = q.get("tier", "")
        last = _parse_date(q.get("last_verified", ""))

        if tier in AGE_STALE_TIERS:
            if last is None:
                out.append({**q, "_staleness": {
                    "kind": "age",
                    "reason": f"{tier} with no parseable last_verified",
                    "age_months": None,
                }})
                continue
            age = _months_between(last, now)
            if age >= months:
                out.append({**q, "_staleness": {
                    "kind": "age",
                    "reason": (f"{tier} not re-verified in {age} months "
                               f"(>= {months})"),
                    "age_months": age,
                }})
        elif tier == "Validated" and q.get("affected_versions"):
            # Not age-gated — a standing prompt to confirm the accommodation
            # is still needed on the host's current major.
            out.append({**q, "_staleness": {
                "kind": "version-recheck",
                "reason": (f"Validated; re-verify still needed on current "
                           f"major (affected_versions="
                           f"{q['affected_versions']})"),
                "age_months": _months_between(last, now) if last else None,
            }})
    return out


def render_report(entries: list[dict], months: int) -> str:
    if not entries:
        return f"host-quirks staleness: no entries past the {months}-month window. ✓"
    lines = [f"host-quirks staleness report ({len(entries)} flagged):", ""]
    for e in entries:
        s = e["_staleness"]
        lines.append(f"  • {e['flag']} [{e.get('tier')}] — {s['reason']}")
        lines.append(f"      host={e.get('host')} last_verified={e.get('last_verified', '?')}")
    lines.append("")
    lines.append("Advisory: review these rows; promote/demote tiers or refresh "
                 "last_verified after re-checking against the current host.")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--catalog", type=pathlib.Path, default=DEFAULT_CATALOG)
    ap.add_argument("--now", default=None,
                    help="Reference date YYYY-MM-DD (default: today).")
    ap.add_argument("--months", type=int, default=DEFAULT_STALE_MONTHS)
    ap.add_argument("--json", action="store_true",
                    help="Emit the flagged entries as a JSON array.")
    args = ap.parse_args(argv)

    now = _parse_date(args.now) if args.now else _dt.date.today()
    if now is None:
        print(f"error: --now '{args.now}' is not YYYY-MM-DD", file=sys.stderr)
        return 2

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    entries = stale_entries(catalog.get("quirks", []), now, args.months)

    if args.json:
        print(json.dumps(entries, indent=2))
    else:
        print(render_report(entries, args.months))
    # Advisory tool: always exit 0 so the scheduled job decides what to do.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
