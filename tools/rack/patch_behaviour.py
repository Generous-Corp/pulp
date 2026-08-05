#!/usr/bin/env python3
"""Read the gate's behaviour block, and say which requested behaviours it shows.

This is the seam between the measurement and the request. The gate measures and
reports; the idiom library says which behaviours a request implies; this turns
one into a verdict on the other. It is deliberately small and deliberately dumb:
the predicates are data in `patch_behaviour_thresholds.json`, not code here.

THE CONTRACT WITH THE CALLER IS "REPORT, NEVER SILENTLY REJECT". Every verdict
carries the number that produced it and a sentence naming what to do about it,
because a retry told "not melodic enough" has nothing to change, and a person
told the same has nothing to disagree with. A flag whose measurement was not
meaningful comes back UNMEASURED, which is neither a pass nor a failure -- a
patch must never be rejected on a number that was never really measured.

Every cable into the audio interface is checked, and a flag passes when ANY of
them shows it. A lead line over a bass drone is not reliably the louder of the
two, so "the loudest cable" is the wrong question to ask of a patch.
"""

from __future__ import annotations

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
THRESHOLDS = os.path.join(HERE, "patch_behaviour_thresholds.json")

#: What the gate puts in front of its machine-readable line. One line, one JSON
#: object, so nothing here has to count brackets in a report that also carries
#: compiler warnings and per-module traces.
MARKER = "BEHAVIOUR_JSON "

#: The schema this reader understands. A block from a newer gate is reported as
#: unreadable rather than parsed on the assumption that nothing moved.
SCHEMA = 1

PASS, FAIL, UNMEASURED = "pass", "fail", "unmeasured"


def load_thresholds(path: str = THRESHOLDS) -> dict:
    with open(path) as f:
        return json.load(f)


def parse(report: str) -> dict | None:
    """The behaviour block out of a gate report, or None if it has none.

    The last one wins: a report that somehow carries two blocks is describing
    two runs, and the newer is the one being talked about.
    """
    found = None
    for line in report.splitlines():
        if line.startswith(MARKER):
            try:
                found = json.loads(line[len(MARKER):])
            except ValueError:
                continue
    if found is None:
        return None
    if found.get("schema", 0) > SCHEMA:
        return {"unreadable": f"the gate emitted schema {found.get('schema')} "
                              f"and this reader understands {SCHEMA}"}
    return found


def _value(cable: dict, field: str):
    """A dotted path into one cable's numbers, or None where it is absent."""
    node = cable
    for part in field.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node if isinstance(node, (int, float)) else None


_OPS = {
    ">=": lambda a, b: a >= b,
    "<=": lambda a, b: a <= b,
    ">": lambda a, b: a > b,
    "<": lambda a, b: a < b,
    "==": lambda a, b: a == b,
}


def _says(cond: dict, actual) -> str:
    """The condition's own sentence, with the measured number in it."""
    text = cond.get("says") or f"{cond['field']} was {actual}"
    try:
        return text.format(actual=actual)
    except (KeyError, ValueError, IndexError):
        return f"{text} ({cond['field']} = {actual})"


def _check(cable: dict, conds: list, need_all: bool) -> tuple[bool, list]:
    """(satisfied, the sentences for the conditions that were not)."""
    met, unmet = 0, []
    for cond in conds:
        actual = _value(cable, cond["field"])
        if actual is None:
            unmet.append(f"the gate reported no {cond['field']}")
            continue
        op = _OPS.get(cond["op"])
        if op is None:
            unmet.append(f"no comparison called '{cond['op']}'")
            continue
        if op(actual, cond["value"]):
            met += 1
        else:
            unmet.append(_says(cond, actual))
    if need_all:
        return not unmet, unmet
    return met > 0, unmet


def evaluate_cable(cable: dict, flag: str, spec: dict) -> dict:
    """One flag against one cable."""
    ok, why = _check(cable, spec.get("needs", []), True)
    if not ok:
        return {"flag": flag, "verdict": UNMEASURED, "source": cable.get("source", "?"),
                "why": why}
    conds = spec.get("all_of")
    ok, why = _check(cable, conds, True) if conds is not None else \
        _check(cable, spec.get("any_of", []), False)
    return {"flag": flag, "verdict": PASS if ok else FAIL,
            "source": cable.get("source", "?"), "why": [] if ok else why}


def evaluate(block: dict, flags, thresholds: dict | None = None) -> list:
    """Every requested flag against the whole patch. -> a verdict per flag.

    `flags` is whatever the idiom's `behaviour` field carried: a dict of
    name -> bool, or any iterable of names. Only flags set truthy are asked
    about, and a name with no entry in the thresholds file comes back
    UNMEASURED naming itself -- an idiom inventing a behaviour nobody measures
    should be visible, not silently satisfied.
    """
    table = (thresholds or load_thresholds()).get("flags", {})
    if isinstance(flags, dict):
        wanted = [name for name, on in flags.items() if on]
    else:
        wanted = list(flags or [])

    if block.get("unreadable"):
        return [{"flag": f, "verdict": UNMEASURED, "source": "?",
                 "why": [block["unreadable"]]} for f in wanted]

    cables = block.get("cables") or []
    out = []
    for flag in wanted:
        spec = table.get(flag)
        if spec is None:
            out.append({"flag": flag, "verdict": UNMEASURED, "source": "?",
                        "why": [f"nothing measures '{flag}' yet; add it to "
                                f"{os.path.basename(THRESHOLDS)} or drop it "
                                f"from the idiom"]})
            continue
        if not cables:
            out.append({"flag": flag, "verdict": UNMEASURED, "source": "?",
                        "why": ["the gate measured no cable into the audio "
                                "interface"]})
            continue
        per = [evaluate_cable(c, flag, spec) for c in cables]
        best = next((v for v in per if v["verdict"] == PASS), None)
        if best is None:
            # Prefer a real failure over "not measurable": a failure names
            # something to fix, and reporting the cable that was merely too
            # quiet to judge hides the one that was judged and came up short.
            best = next((v for v in per if v["verdict"] == FAIL), per[0])
        best = dict(best)
        best["means"] = spec.get("means", "")
        out.append(best)
    return out


def failures(verdicts: list) -> list:
    """Only the flags the patch was measured against and did not show."""
    return [v for v in verdicts if v["verdict"] == FAIL]


def explain(verdicts: list) -> str:
    """The verdicts as something to hand a model or print for a person.

    Empty when there is nothing to say, so a caller can test it as a string.
    """
    lines = []
    for v in verdicts:
        if v["verdict"] == PASS:
            continue
        head = "not measurable" if v["verdict"] == UNMEASURED else "not shown"
        means = f" ({v['means']})" if v.get("means") else ""
        lines.append(f"  {v['flag']}{means}: {head} on {v['source']}")
        for why in v["why"]:
            lines.append(f"      {why}")
    return "\n".join(lines)


if __name__ == "__main__":
    import sys
    text = sys.stdin.read() if len(sys.argv) < 2 else open(sys.argv[1]).read()
    parsed = parse(text)
    if parsed is None:
        print("no behaviour block in that report")
        raise SystemExit(2)
    asked = sys.argv[2:] or list(load_thresholds()["flags"])
    got = evaluate(parsed, asked)
    for v in got:
        print(f"{v['verdict']:11s} {v['flag']}")
    told = explain(got)
    if told:
        print(told)
