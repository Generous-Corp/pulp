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

    print("\n" + ("all good" if bad == 0 else "FAILED"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
