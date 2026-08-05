#!/usr/bin/env python3
"""What these books taught us, in our words, in a form something can ship.

    python3 knowledge.py                 # print the technique layer
    python3 knowledge.py --check         # lint it
    python3 knowledge.py --for <slug>    # what an idiom's realisation should know
    python3 knowledge.py --compare       # what it adds to a contract, measured

TWO LAYERS, AND THE SPLIT IS THE POINT.

  technique     instrument-agnostic. What a technique is, why it works, what it
                should sound like, and the numbers. Mentions no module, no
                jack, no cable and no VCV Rack. `knowledge/technique/*.json`.
  realisation   how to build it HERE, which is the idiom library next door:
                roles, ports, cables, negative controls.

An agent writing a C++ Processor for a drum synth wants the first and none of
the second. An agent wiring a rack wants both. Fused, this is Rack-only forever
and the question of using it anywhere else cannot even be asked; split, it
transfers for free. This is the decision that is expensive to reverse later, so
it is made now and it is ENFORCED -- `problems()` fails a technique entry whose
own prose names an idiom or reaches for Rack vocabulary. The link runs one way:
an idiom declares what it `realises`, and this layer never mentions an idiom.

SCOPE. Forge Modular only, deliberately. No registry, no cross-subsystem
plumbing, nothing Pulp-wide. One instance, right.

OPTIONAL, LIKE THE AFFORDANCES. Nothing here may make the generator worse or
slower when it is absent or ignored. Absent knowledge means less help, never a
failure.
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TECHNIQUE_DIR = os.path.join(HERE, "knowledge", "technique")

PROVENANCE = ("read", "canon", "inferred")

# TWO SHAPES OF ENTRY, and conflating them would overstate what is here.
#
#   technique  hand-written: what a technique is, WHY it works, what to listen
#              for. Six of these took a reading pass each and each one says
#              something no other entry says.
#   settings   generated from a catalogue: a named sound and the values that
#              make it recognisable. Seventy-odd of them, and their prose is
#              templated -- their whole content is the name and the numbers.
#
# Both are useful and they are not the same thing. Reporting 80 "technique
# entries" would be padding a count with records whose reasoning is a single
# sentence written once and repeated. A settings record is therefore exempt
# from the `why` requirement, because demanding one would only produce more of
# the same sentence, and it is counted separately everywhere.
KINDS = ("technique", "settings")

# Words that mean this entry has stopped being instrument-agnostic. Checked
# against OUR prose only -- an anchor quotes the source, and the source is
# entitled to say "VCA".
_REALISATION_WORDS = ("vcv", " rack ", "module", "cable", "jack", "patch cable",
                      "vco", "vca", "vcf", "attenuverter", "multiple's")


def load(path: str | None = None) -> dict:
    """Every technique entry, by id."""
    out: dict[str, dict] = {}
    root = path or TECHNIQUE_DIR
    if not os.path.isdir(root):
        return out
    for name in sorted(os.listdir(root)):
        if not name.endswith(".json") or name.startswith("_"):
            continue
        with open(os.path.join(root, name)) as f:
            doc = json.load(f)
        for entry in doc.get("entries", []):
            if entry.get("id"):
                out[entry["id"]] = entry
    return out


def _our_prose(entry: dict) -> str:
    """Everything in an entry that WE wrote, excluding quoted source text."""
    parts = [entry.get("what", ""), entry.get("why", "")]
    listen = entry.get("listen_for") or {}
    parts += [listen.get("sounds_like", ""), listen.get("confusable_with", "")]
    for n in entry.get("numbers") or []:
        parts += [n.get("quantity", ""), str(n.get("value", "")),
                  n.get("grounding", "")]
    return " ".join(parts).lower()


def problems(entries: dict | None = None, idioms: dict | None = None) -> list[str]:
    """Everything wrong with the technique layer.

    The layer-separation rules are the ones worth having: an entry that drifts
    into naming modules is one nobody outside this directory can use, and the
    drift is invisible because the entry still reads perfectly well.
    """
    entries = entries if entries is not None else load()
    bad: list[str] = []
    slugs = set(idioms or {})

    for eid, entry in sorted(entries.items()):
        prose = _our_prose(entry)

        for word in _REALISATION_WORDS:
            if word in prose:
                bad.append(f"{eid} says {word.strip()!r} in its own prose; the "
                           f"technique layer is instrument-agnostic and this "
                           f"belongs in the idiom that realises it")
        # A CROSS-REFERENCE, not a word. "vibrato" is a technique and also an
        # idiom slug, and banning the word would forbid the technique layer
        # from naming techniques -- the first version of this rule did exactly
        # that and flagged five honest entries. What is actually forbidden is
        # POINTING at the realisation: a backticked slug, or "the <slug>
        # idiom". Those are the references that stop making sense the moment
        # this file is read anywhere but next door.
        for slug in slugs:
            if f"`{slug}`" in prose or f"the {slug} idiom" in prose:
                bad.append(f"{eid} points at the idiom {slug}; the link runs "
                           f"the other way, so the technique layer stays "
                           f"portable")

        if entry.get("provenance") not in PROVENANCE:
            bad.append(f"{eid} has provenance {entry.get('provenance')!r}")
        elif entry["provenance"] == "read" and not (entry.get("anchor") or {}).get("quote"):
            bad.append(f"{eid} says it was read and carries no anchor")
        elif entry["provenance"] != "read" and entry.get("anchor"):
            bad.append(f"{eid} is {entry['provenance']} and carries an anchor")

        kind = entry.get("kind", "technique")
        if kind not in KINDS:
            bad.append(f"{eid} has kind {kind!r}, which is not one of {KINDS}")
        if len(str(entry.get("what") or "")) < 60:
            bad.append(f"{eid} does not say what it is")
        if kind == "technique" and len(str(entry.get("why") or "")) < 60:
            bad.append(f"{eid} does not say why it works, which is the half an "
                       f"agent cannot derive from a cable list")
        # A settings record earns its place by carrying values. One with no
        # measured number is a name and a template.
        if kind == "settings" and not (entry.get("numbers") or []):
            bad.append(f"{eid} is a settings record with no measured value in "
                       f"it, which is a name and a template")

        # A NUMBER CARRIES ITS OWN PROVENANCE. A technique can be common
        # knowledge while one of its numbers came out of a specific book, and
        # letting the entry's tier cover both is how a guess acquires a
        # citation. This is the rule the whole file exists to keep.
        for n in entry.get("numbers") or []:
            where = f"{eid}/{n.get('quantity')}"
            if n.get("provenance") not in PROVENANCE:
                bad.append(f"{where} has provenance {n.get('provenance')!r}")
            elif n["provenance"] == "read" and not (n.get("anchor") or {}).get("quote"):
                bad.append(f"{where} says it was read and carries no anchor")
            elif n["provenance"] != "read" and n.get("anchor"):
                bad.append(f"{where} is {n['provenance']} and carries an anchor")
            if len(str(n.get("grounding") or "")) < 40:
                bad.append(f"{where} gives a number with no account of where it "
                           f"came from — which is the difference between a "
                           f"reference and a plausible sentence")
    return bad


def for_idiom(slug: str, entries: dict | None = None,
              idioms: dict | None = None) -> list[dict]:
    """The technique an idiom realises. Derived from the idiom's own claim."""
    entries = entries if entries is not None else load()
    if idioms is None:
        sys.path.insert(0, HERE)
        import idiom_check                            # noqa: PLC0415
        idioms = idiom_check.load_idioms()
    idiom = idioms.get(slug) or {}
    return [entries[i] for i in idiom.get("realises", []) if i in entries]


def render(entry: dict) -> str:
    """One entry, as the model should read it."""
    out = [f"### {entry['id']}", f"    {entry['what']}",
           f"    why: {entry['why']}"]
    listen = entry.get("listen_for") or {}
    if listen.get("sounds_like"):
        out.append(f"    listen for: {listen['sounds_like']}")
    if listen.get("confusable_with"):
        out.append(f"    not: {listen['confusable_with']}")
    for n in entry.get("numbers") or []:
        mark = {"read": "measured", "canon": "common knowledge",
                "inferred": "derived"}[n["provenance"]]
        out.append(f"    {n['quantity']}: {n['value']}  [{mark}]")
    return "\n".join(out)


def render_for(slug: str, entries: dict | None = None,
               idioms: dict | None = None) -> str:
    got = for_idiom(slug, entries, idioms)
    if not got:
        return ""
    return ("What is known about the technique behind this patch, independently "
            "of how it is wired:\n\n" +
            "\n\n".join(render(e) for e in got) + "\n")


def compare(idioms: dict | None = None) -> int:
    """What the knowledge layer adds to a contract, counted rather than claimed.

    HONESTLY LABELLED, BECAUSE THIS IS NOT THE TEST THAT MATTERS. The question
    worth answering is whether a patch built WITH this is better than one built
    without, and that needs generated patches and a listener. This measures the
    thing in between: how much grounded, checkable guidance reaches the model at
    all. A knowledge base that delivers nothing cannot improve anything, so this
    is a necessary condition and not a sufficient one -- and reporting it as if
    it were would be exactly the laundering this layer is built against.
    """
    sys.path.insert(0, HERE)
    import idiom_check                                # noqa: PLC0415
    idioms = idioms if idioms is not None else idiom_check.load_idioms()
    entries = load()

    linked = {s: i.get("realises") or [] for s, i in idioms.items()
              if i.get("realises")}
    print(f"technique entries: {len(entries)}")
    print(f"idioms declaring a realisation: {len(linked)} of {len(idioms)}")

    numbers = [n for e in entries.values() for n in (e.get("numbers") or [])]
    by_tier: dict[str, int] = {}
    for n in numbers:
        by_tier[n["provenance"]] = by_tier.get(n["provenance"], 0) + 1
    print(f"numbers available: {len(numbers)} "
          + ", ".join(f"{v} {k}" for k, v in sorted(by_tier.items())))

    print("\nper request, what reaches the model:")
    print(f"  {'idiom':<26} {'without':>8} {'with':>8}  numbers delivered")
    total_without = total_with = 0
    for slug in sorted(linked):
        without = len(idiom_check.load_idioms()[slug].get("topology") or [])
        got = for_idiom(slug, entries, idioms)
        n = sum(len(e.get("numbers") or []) for e in got)
        total_without += 0
        total_with += n
        print(f"  {slug:<26} {without:>8} {without:>8}  +{n} "
              f"({', '.join(e['id'] for e in got)})")
    print(f"\n  requirements a patch is checked against: unchanged")
    print(f"  grounded numbers delivered: 0 without, {total_with} with")
    print("\nWHAT THIS DOES NOT SHOW: whether the resulting patch sounds better.")
    print("That needs generated patches and somebody listening, and it is the")
    print("only test that settles the question. This one says the guidance")
    print("exists and is grounded, which is a precondition, not a result.")
    return 0


def main(argv: list[str]) -> int:
    if "--check" in argv:
        sys.path.insert(0, HERE)
        import idiom_check                            # noqa: PLC0415
        bad = problems(load(), idiom_check.load_idioms())
        for b in bad:
            print(f"  {b}")
        e = load()
        n_t = sum(1 for x in e.values() if x.get("kind", "technique") == "technique")
        print(f"  {n_t} technique entries + {len(e) - n_t} settings records, "
              f"{len(bad)} problem(s)")
        return 1 if bad else 0
    if "--for" in argv:
        sys.stdout.write(render_for(argv[argv.index("--for") + 1]))
        return 0
    if "--compare" in argv:
        return compare()
    for e in load().values():
        print(render(e))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
