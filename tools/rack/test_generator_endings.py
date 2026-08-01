#!/usr/bin/env python3
"""Can the app see every way a generation ENDS?

    tools/rack/test_generator_endings.py

The app watches a log. When a generation stops without producing anything, the
only thing telling the app so is the wording of the line the generator printed
on its way out — and the monitor matches those by substring.

A message it does not match reads as progress. The outcome stays `running`, the
stage never resolves, and there is nothing to open: the app waits forever on a
build that ended minutes ago. That is not hypothetical — "gave up after N
attempts" was missing, and so was "model call failed", which is what a machine
whose model CLI cannot reach its credential prints. Any SSH session, or a
locked keychain, ends there.

So: every `raise SystemExit(...)` in the two generators must be matched by some
rule in build_monitor.cpp. Both sides are read from source, so neither is a
copy that can drift.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GENERATORS = [os.path.join(HERE, "patch.py"), os.path.join(HERE, "generate.py")]
# Everything else that writes into the log the app watches. The monitor's rules
# match sentences from all of these, not only the two entry points.
LOG_WRITERS = GENERATORS + [
    os.path.join(HERE, "forge_modular.py"),      # panel + manifest emitter
    os.path.join(HERE, "patch_gate.cpp"),        # the audio gate
    os.path.join(HERE, "behaviour_gate.cpp"),    # the module gate
    os.path.join(HERE, "idiom_check.py"),        # the idiom verdicts
]
MONITOR = os.path.join(HERE, "..", "..", "forge-seam", "modular",
                       "build_monitor.cpp")

# Exits that never reach a watched log: argument handling and usage errors,
# raised before the app's engine is involved. Named so the list is a decision.
NOT_IN_A_BUILD_LOG = {
    "rack sdk not found at": "raised at import/setup, before a build starts — "
                             "but matched anyway, since it costs nothing",
}


def endings():
    """The literal prefix of every SystemExit message the generators raise."""
    out = {}
    for path in GENERATORS:
        src = open(path).read()
        for m in re.finditer(r'SystemExit\(\s*f?"([^"{]{8,})', src):
            text = m.group(1).strip()
            # Cut at the first interpolation or escape; the prefix is what a
            # substring rule can match.
            text = re.split(r'\\n|\{', text)[0].strip().rstrip(":").lower()
            if len(text) >= 8:
                out[text] = os.path.basename(path)
    return out


def rules():
    """Every substring build_monitor.cpp treats as an error or a refusal."""
    src = open(MONITOR).read()
    return {m.lower() for m in re.findall(r'contains\(lower,\s*"([^"]+)"\)', src)}


def main():
    bad = 0
    msgs, rs = endings(), rules()
    if not msgs:
        print("  WRONG  no SystemExit messages found — this check is blind")
        return 1
    if not rs:
        print("  WRONG  no contains() rules found in build_monitor.cpp")
        return 1

    unseen = []
    for msg, where in sorted(msgs.items()):
        if not any(rule in msg for rule in rs):
            unseen.append(f"{where}: {msg!r}")
    if unseen:
        print(f"  WRONG  {len(unseen)} generator ending(s) the app cannot see. "
              f"Each one is a build that watches forever:")
        for u in unseen:
            print(f"           {u}")
        bad += 1
    else:
        print(f"  ok     all {len(msgs)} generator endings are recognised")

    # And the reverse: a rule matching wording nobody prints any more is a
    # refusal or an error the app can no longer see. The rules are substrings
    # of the generators' own sentences, so a reworded message leaves the rule
    # pointing at nothing — and nothing about that is visible at runtime.
    # Decoded, because a rule is written in C++ with \xNN escapes for the
    # arrow while the Python prints the character itself — comparing the raw
    # spellings reports a match as missing.
    def decoded(rule):
        # A C++ literal spells the arrow \xe2\x86\x92 — the UTF-8 BYTES of
        # it — while the Python prints the character. Unescaping alone gives
        # mojibake and reports a rule that matches as missing, which is a
        # false alarm sending somebody hunting for nothing.
        try:
            raw = rule.encode("latin-1").decode("unicode_escape")
            return raw.encode("latin-1").decode("utf-8")
        except Exception:                                   # noqa: BLE001
            return rule
    generator_text = "\n".join(
        open(g, encoding="utf-8", errors="replace").read()
        for g in LOG_WRITERS if os.path.exists(g)).lower()
    # Rules that legitimately match text from elsewhere: a Python traceback, a
    # compiler, or the OS. Named so each is a decision.
    NOT_FROM_A_GENERATOR = {
        "fatal error":  "the compiler's, when a module fails to build",
        "no such file": "the shell's, when something is missing",
        "traceback":    "Python's own, from any raised exception",
        # `gate`-kind rules whose wording no current tool prints. They decide
        # how a line is DISPLAYED, not whether a build ended, so an orphan
        # here is cosmetic — but it is listed rather than ignored, so a new
        # one shows up instead of joining a silent pile.
        "uses pulp dsp":    "an older module gate's wording; nothing prints it",
        "uses no pulp dsp": "the same gate's other verdict",
        "rejected at":      "an older lint's wording",
    }
    orphaned = []
    for rule in sorted(rs):
        if rule in generator_text or decoded(rule) in generator_text:
            continue
        if any(key in rule for key in NOT_FROM_A_GENERATOR):
            continue
        orphaned.append(rule)
    if orphaned:
        print(f"  WRONG  {len(orphaned)} rule(s) match wording no generator "
              f"prints — the app can no longer see what they were for:")
        for o in orphaned:
            print(f"           {o!r}")
        bad += 1
    else:
        print(f"  ok     every rule matches something a generator still says")

    print("\n" + ("all good" if bad == 0 else "FAILED"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
