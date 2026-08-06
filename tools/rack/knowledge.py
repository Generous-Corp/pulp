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

import hashlib
import json
import os
import re
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
STATUSES = ("admitted", "candidate", "quarantined")

# Words that mean this entry has stopped being instrument-agnostic. Checked
# against OUR prose only -- an anchor quotes the source, and the source is
# entitled to say "VCA".
_REALISATION_WORDS = ("vcv", " rack ", "module", "cable", "jack", "patch cable",
                      "vco", "vca", "vcf", "attenuverter", "multiple's")


def load(path: str | None = None, include_candidates: bool = False) -> dict:
    """Technique entries by id; unvalidated candidates are excluded normally.

    Recovery is not admission. A candidate can be linted, reviewed and used by
    a controlled A/B harness, but `for_prompt` and `for_idiom` call this default
    path and therefore cannot put it in a generation contract. Promotion is an
    explicit status edit after validation, never a side effect of extraction.
    """
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
            status = entry.get("status", "admitted")
            eid = entry.get("id")
            if eid and (include_candidates or status == "admitted"):
                if eid in out:
                    raise ValueError(
                        f"duplicate knowledge id {eid!r}; variants must be "
                        "quarantined or merged before generation")
                out[eid] = entry
    return out


def canonical_claim_fingerprint(claim: str) -> str:
    """Stable exact identity for the one canonical wording of a claim."""
    normal = re.sub(r"\s+", " ", claim.strip().lower())
    return hashlib.sha256(normal.encode("utf-8")).hexdigest()


def canonical_locator(evidence: dict) -> str:
    """Edition/page identity used to attach corroboration to one claim row."""
    return ":".join(str(evidence.get(field, "")) for field in
                    ("work_id", "edition_id", "page", "source_sha256"))


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
    entries = entries if entries is not None else load(include_candidates=True)
    bad: list[str] = []
    slugs = set(idioms or {})
    semantic_ids: dict[str, str] = {}

    for eid, entry in sorted(entries.items()):
        prose = _our_prose(entry)

        status = entry.get("status", "admitted")
        if status not in STATUSES:
            bad.append(f"{eid} has status {status!r}, which is not one of "
                       f"{STATUSES}")
        claim = entry.get("claim")
        fingerprint = entry.get("canonical_claim_fingerprint")
        semantic_id = entry.get("canonical_semantic_id")
        if not isinstance(semantic_id, str) or not semantic_id.strip():
            bad.append(f"{eid} has no reviewer-assigned canonical_semantic_id")
        elif semantic_id in semantic_ids:
            bad.append(f"{eid} duplicates the canonical semantic identity already "
                       f"held by {semantic_ids[semantic_id]}; attach its source "
                       "as corroborating evidence to that row")
        else:
            semantic_ids[semantic_id] = eid
        if status != "admitted" or claim or fingerprint:
            if not claim or not fingerprint:
                bad.append(f"{eid} is admission-tracked but has no claim and "
                           "canonical_claim_fingerprint")
            elif canonical_claim_fingerprint(claim) != fingerprint:
                bad.append(f"{eid} has a canonical claim fingerprint that does not "
                           "match its canonical claim")
        evidence = entry.get("evidence") or []
        if status != "admitted" and not evidence:
            bad.append(f"{eid} is {status} with no canonical source locator")
        validation = entry.get("validation") or {}
        if status == "candidate" and validation.get("type") != "guided-audio-ab":
            bad.append(f"{eid} is a candidate without a guided-audio-ab "
                       "promotion plan")
        entry_locators: set[str] = set()
        for source in evidence:
            locator = canonical_locator(source)
            required = ("work_id", "edition_id", "page", "source_sha256",
                        "page_sha256", "shows")
            missing = [field for field in required if not source.get(field)]
            if missing:
                bad.append(f"{eid} has incomplete source evidence: "
                           f"{', '.join(missing)}")
            if locator in entry_locators:
                bad.append(f"{eid} repeats source locator {locator}; one "
                           "evidence row is enough")
            else:
                entry_locators.add(locator)

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
        elif entry["provenance"] == "read" and not any(
                (entry.get("anchor") or {}).get(field)
                for field in ("quote", "page")):
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
            elif n["provenance"] == "read" and not any(
                    (n.get("anchor") or {}).get(field)
                    for field in ("quote", "page")):
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


#: A family word in a request, and how many of that family to offer. Capped
#: because a request for "a bassline" wanting seven bass recipes is a wall of
#: settings rather than help.
_FAMILY_WORDS = {
    "brass": "brass", "string": "strings", "strings": "strings",
    "woodwind": "woodwinds", "woodwinds": "woodwinds", "wind": "woodwinds",
    "keyboard": "keyboards", "keys": "keyboards", "vocal": "vocals",
    "choir": "vocals", "percussion": "untuned percussion",
    "drum": "untuned percussion", "drums": "untuned percussion",
    "bass": "bass", "bassline": "bass", "pad": "pads", "pads": "pads",
    "lead": "leads",
}
# WORDS DELIBERATELY NOT HERE, because in a synthesizer they mean something
# else. "voice" is the architecture of one note ("a subtractive voice"), not a
# request for a choir — it summoned three vocal recipes for a request that had
# nothing to do with singing. "effect" and "effects" are what every module
# does. A recipe surfaced for a sound nobody asked for is worse than none: it
# pushes the patch toward settings for the wrong instrument, and it does it
# confidently.
FAMILY_LIMIT = 3


def for_prompt(prompt: str, entries: dict | None = None) -> list[dict]:
    """The named sounds a request is asking for.

    KEYED BY THE SOUND, NOT BY AN IDIOM, which is the whole point and was the
    whole bug. The catalogue is indexed by instrument name; the only lookup
    that existed took an idiom slug. So 74 recipes sat in the tree, complete
    and verified, and "make me a cello" returned nothing — the wiring LOOKED
    present, because `knowledge` was imported and `render_for` was called, and
    it could never have matched.

    Whole words only. "cello" must not fire on "mandocello", and a substring
    search would have made half the catalogue match half the requests, which is
    worse than matching nothing: a recipe surfaced for a sound nobody asked for
    pushes the patch toward settings for the wrong instrument.

    A family word is a fallback, not an addition. "a brass stab" names no
    recipe in the book and should still land somewhere sensible, but only when
    nothing was named outright.
    """
    import re                                          # noqa: PLC0415
    entries = entries if entries is not None else load()
    words = set(re.findall(r"[a-z]+", prompt.lower()))
    if not words:
        return []

    named = []
    for entry in entries.values():
        if entry.get("kind") != "settings":
            continue
        for name in entry.get("names", []):
            parts = set(name.split())
            if parts and parts <= words:
                named.append(entry)
                break
    if named:
        return sorted(named, key=lambda e: -len(max(e["names"], key=len)))[:FAMILY_LIMIT]

    families = {_FAMILY_WORDS[w] for w in words if w in _FAMILY_WORDS}
    if not families:
        return []
    kin = [e for e in entries.values()
           if e.get("kind") == "settings" and e.get("family") in families]
    return sorted(kin, key=lambda e: e["id"])[:FAMILY_LIMIT]


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
        all_entries = load(include_candidates=True)
        bad = problems(all_entries, idiom_check.load_idioms())
        for b in bad:
            print(f"  {b}")
        e = all_entries
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
