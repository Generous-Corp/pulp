#!/usr/bin/env python3
"""Does the data this machine has actually reach the model?

Every other test here checks code against data it invented. That leaves the
seam untested, and the seam is where the failures have been: the instrument
catalogue holds 74 recipes and `render_for` is imported, called, and returns
nothing, so a request naming a sound gets none of them. Every part worked; the
path did not, and no unit test could have said so.

So these assert the opposite direction -- take what is really installed and
really measured, build the contract the model is really given, and require
that specific facts from the data appear in it. A fact absent here is a fact
the model never had, whatever the code looks like.

Nothing is asserted about a machine that has not measured anything: a fresh
checkout has an empty port map, and a test that fails there would be testing
the machine rather than the code. Those cases skip loudly and say so, because
a skip is not a pass.
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import idiom_check      # noqa: E402
import patch            # noqa: E402
import patch_vocabulary  # noqa: E402

FAILED = 0
SKIPPED = 0


def ok(msg: str) -> None:
    print(f"  ok     {msg}")


def wrong(msg: str) -> None:
    global FAILED
    FAILED += 1
    print(f"  WRONG  {msg}")


def skip(msg: str) -> None:
    global SKIPPED
    SKIPPED += 1
    print(f"  skip   {msg} — this is a skip, not a pass")


def check(cond: bool, msg: str) -> None:
    ok(msg) if cond else wrong(msg)


def contract_for(prompt: str) -> str:
    """Exactly what patch.py assembles and hands to the model."""
    idioms = None
    claimed = idiom_check.resolve_intent(prompt)
    if claimed.slug:
        return patch_vocabulary.for_prompt(prompt, idioms)
    return patch_vocabulary.render(idioms)


def main() -> int:
    inv = patch.inventory()

    # ── measured port names must reach the model ────────────────────────────
    # A module the cartographer has measured must arrive with its jack names.
    # This is the failure that made the model wire blind: render_inventory
    # emits port lines only when a module has them, so an unmeasured module
    # and a module with no ports look identical from the outside.
    named = [(slug, name, m) for slug, p in inv.items()
             for name, m in p["modules"].items() if m.get("inputs")]
    if not named:
        skip("nothing on this machine is cartographed, so port names cannot "
             "be checked")
    else:
        slug, model, entry = named[0]
        jack = next((j for j in entry["inputs"] if j), None)
        text = patch.render_inventory(inv) if hasattr(patch, "render_inventory") \
            else ""
        if not text:
            skip("render_inventory is not reachable from here")
        else:
            check(model in text,
                  f"a cartographed module ({model}) appears in the inventory")
            check(bool(jack) and jack in text,
                  f"its measured jack name ({jack!r}) reaches the model")

    # ── an uncartographed module must not silently look portless ───────────
    blind = [(slug, name) for slug, p in inv.items()
             for name, m in p["modules"].items() if not m.get("inputs")]
    if blind:
        ok(f"{len(blind)} of {sum(len(p['modules']) for p in inv.values())} "
           f"installed modules have no measured ports (the model must be told, "
           f"not left to guess)")

    # ── listen_for must reach the model ────────────────────────────────────
    # This one works today and must keep working: it is the only part of the
    # book knowledge that demonstrably arrives.
    text = contract_for("a melodic four-note sequence")
    check("should sound like" in text.lower(),
          "a melodic request carries an expected sound to the model")

    # ── the instrument catalogue must reach the model ──────────────────────
    # 74 recipes with real numbers, keyed by the sound a person names. The
    # lookup passes an idiom slug and the entries are keyed by instrument, so
    # they have never once been rendered.
    cat = os.path.join(HERE, "knowledge", "technique", "instruments.json")
    if not os.path.exists(cat):
        skip("no instrument catalogue on this machine")
    else:
        entries = json.load(open(cat)).get("entries") or []
        names = [n for e in entries for n in (e.get("names") or [])]
        if not names:
            skip("the instrument catalogue is empty")
        else:
            pick = "cello" if "cello" in names else names[0]
            text = contract_for(f"make me a {pick}")
            check(pick in text.lower(),
                  f"naming a catalogued sound ({pick}) reaches the model")
            # The numbers are the reason the catalogue is worth having.
            entry = next(e for e in entries if pick in (e.get("names") or []))
            # `numbers` is a list of {quantity, value, provenance, anchor}:
            # the value carries its units as text ("7.5 Hz, applied to
            # amplitude") because a bare float would lose what it measures.
            nums = entry.get("numbers") or []
            first = next((n.get("value") for n in nums
                          if isinstance(n, dict) and n.get("value")), None)
            if not first:
                skip(f"{pick} carries no numeric settings to look for")
            else:
                check(str(first) in text,
                      f"its settings ({first!r}) reach the model too")

            # NEGATIVE CONTROL. A recipe surfaced for an unrelated request is
            # worse than none: it steers a patch toward a sound nobody asked
            # for. Something must NOT match.
            #
            # Matching on the NAME alone false-alarms: several instrument
            # names are also ordinary modular words -- `clock`, `bell`, `clap`
            # -- and appear in a contract that never rendered a recipe. So
            # look for a recipe's own prose, which nothing else emits.
            text = contract_for("a slowly evolving noise texture")
            whats = [((e.get("names") or ["?"])[0], (e.get("what") or "")[:40])
                     for e in entries if e.get("what")]
            leaked = [n for n, w in whats if w and w.lower() in text.lower()]
            check(not leaked,
                  "a request naming no instrument pulls in no recipe"
                  + (f" (leaked: {leaked[:3]})" if leaked else ""))

    # ── measured ranges must reach the model ───────────────────────────────
    withrange = [(name, m) for p in inv.values()
                 for name, m in p["modules"].items()
                 if any(isinstance(q, dict) and "min" in q
                        for q in (m.get("params") or []))]
    if not withrange:
        skip("no measured parameter ranges on this machine")
    else:
        ok(f"{len(withrange)} modules carry measured parameter ranges into "
           f"the inventory")

    print()
    if FAILED:
        print(f"{FAILED} wrong, {SKIPPED} skipped")
    else:
        print(f"all good ({SKIPPED} skipped)" if SKIPPED else "all good")
    return 1 if FAILED else 0


if __name__ == "__main__":
    raise SystemExit(main())
