#!/usr/bin/env python3
"""Announce how full the A2T scope-history window is, before its guard fails.

`test_scope_touching_history_keeps_headroom_under_its_limit` (ctest
`gpu-trace-overhead-acceptance-selftest`, on the required `macos` gate) fails
once the scope-touching revision count passes 75% of
`A2T_SCOPE_HISTORY_LIMIT`, and from then on every gate run is red until
`A2T_SCOPE_HISTORY_BASE` is re-pinned (decisions contract row 22). The count
climbs with ordinary traffic, so that failure is a date, not a surprise, but
only if someone sees the climb.

When the test runs with `PULP_A2T_HEADROOM_OUT` set it writes its measurement
there. This reads it and prints one annotation on the gate job:

- a `::warning` from `EARLY_WARNING_RATIO` of the limit, naming the re-pin;
- a `::notice` with the numbers below it, so a missing annotation is itself
  visible as "not measured";
- a `::warning` when the file is missing or unreadable after the suite ran,
  because a silent instrument is the failure this exists to prevent.

It never fails the step: the test is the gate, this is the advance notice.

    a2t_headroom_annotation.py <measurement.json>
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

TITLE = "a2t-scope-history-headroom"
EARLY_WARNING_RATIO = 0.60
REPIN = ("re-pin A2T_SCOPE_HISTORY_BASE on protected main, retaining about 25% of the "
         "window; never raise A2T_SCOPE_HISTORY_LIMIT (decisions contract row 22)")


def classify(count: int, limit: int, budget: int) -> str:
    """'over' past the guard's budget, 'early' from the early-warning share, else 'ok'."""
    if count > budget:
        return "over"
    if count >= EARLY_WARNING_RATIO * limit:
        return "early"
    return "ok"


def annotation(measurement: dict) -> str:
    count, limit, budget = (int(measurement[k]) for k in ("count", "limit", "budget"))
    level = classify(count, limit, budget)
    summary = (f"{count} scope-touching revisions of a {limit} limit "
               f"({count / limit:.0%}); the gate fails past {budget}")
    if level == "over":
        return f"::warning title={TITLE}::{summary}. The required gate is failing now: {REPIN}."
    if level == "early":
        return (f"::warning title={TITLE}::{summary}, {budget - count} revisions from now. "
                f"Schedule the re-pin: {REPIN}.")
    return f"::notice title={TITLE}::{summary}."


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 0
    path = Path(argv[1])
    try:
        measurement = json.loads(path.read_text(encoding="utf-8"))
        line = annotation(measurement)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"::warning title={TITLE}::A2T scope-history headroom was not measured "
              f"({type(error).__name__}: {error}); the window can fill without notice.")
        return 0
    print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
