#!/usr/bin/env python3
"""Ask a corpus of real patches what our gate gets wrong.

    corpus_sweep.py labels     # which real jack labels does nothing recognise
    corpus_sweep.py idioms     # which requirements reject real patches
    corpus_sweep.py coverage   # how much of the corpus we can judge at all

Patches humans built, played and shared should overwhelmingly pass. So a
requirement that rejects a large fraction of them is a defect in the
requirement, and the COUNT is the finding: one rejected patch is an anecdote,
forty rejected by the same rule is a bug with a queue behind it.

`labels` is the sharper of the two and comes first. `_port_matches` falls back
to `label.upper() in ok_labels` -- WHOLE-STRING equality -- so "GATE 1 CV"
does not match a set containing "GATE", and a correctly wired patch is
rejected for a jack it did wire. Two of five gate defects found in one day
were that. Sweeping the corpus for jack labels nothing recognises finds the
rest of the class at once, over real usage, with no idiom resolution on top to
add noise of its own.

That noise is the reason `coverage` exists. Our gate resolves roles through
the installed inventory, so a module nobody here owns fails every requirement
touching it -- for a reason that is not a gate defect. Rejection rates are
reported only over patches we can actually judge, and the excluded count is
printed rather than hidden, because a rate over an unstated denominator is a
number that cannot be argued with.
"""

from __future__ import annotations

import json
import os
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import idiom_check as ic                                   # noqa: E402
import patch_corpus                                        # noqa: E402

# Kinds that match everything by construction; counting them would say a label
# is recognised when nothing about it was read.
WILDCARD = {"any_in", "any_out"}


def held_patches() -> list[tuple[dict, dict]]:
    """(provenance, patch json) for every corpus patch that parses."""
    index = patch_corpus.load_index()
    out = []
    for meta in index["patches"].values():
        if "file" not in meta:
            continue
        path = os.path.join(patch_corpus.PATCH_DIR, meta["file"])
        # A .vcv is a zstd tar, not JSON — see patch_corpus.read_patch.
        doc = patch_corpus.read_patch(path)
        if doc is not None:
            out.append((meta, doc))
    return out


def coverage_of(patch: dict, inv: dict) -> tuple[int, int]:
    """(modules we have an inventory entry for, modules in the patch)."""
    mods = patch.get("modules") or []
    known = sum(1 for m in mods if ic._entry(inv, m))
    return known, len(mods)


def _direction(kind: str) -> str:
    return "out" if kind.endswith("_out") else "in"


def _tokens(label: str) -> set:
    return set(label.upper().replace("/", " ").replace("-", " ")
               .replace(",", " ").split())


def _label_suggests(label: str, spec: dict) -> bool:
    """Would a person reading this label call the jack this kind?

    Token containment, which is what the whole-string rule is being measured
    AGAINST -- not a proposed implementation. "GATE 1 CV" contains GATE, so a
    person calls it a gate input; `label.upper() in ok_labels` does not, and
    that gap is the defect this sweep exists to size.
    """
    words = _tokens(label)
    for known in (spec.get("labels") or []):
        k = known.upper()
        if k in words or (" " in k and k in label.upper()):
            return True
    return False


def sweep_labels(verbose: bool = True) -> int:
    """Jacks a person would call kind K that `_port_matches` denies are K.

    The first version of this asked whether ANY of the fourteen port kinds
    matched each jack, and got 100% -- a number that could not have been
    anything else. Nearly every cartographed port carries a role, a role
    matches its kind directly, and `cv_in`/`cv_out` between them accept most
    roles there are. "Something matched" was never the question.

    The question is whether the SPECIFIC kind an idiom names matches. An idiom
    asking for `gate_in` does not care that the jack also reads as `cv_in`; it
    rejects the patch. So this counts jacks whose LABEL says kind K and whose
    matcher says not-K, which is exactly the shape of a false rejection.
    """
    import patch as patch_mod                              # noqa: PLC0415
    inv = patch_mod.inventory()
    roles = ic.load_roles()
    corpus = held_patches()
    if not corpus:
        print("no corpus held — run patch_corpus.py fetch first")
        return 0

    missed: Counter = Counter()
    vetoed: Counter = Counter()
    contested: Counter = Counter()
    examples: dict = {}
    ends = 0

    for meta, patch in corpus:
        by_id = {m.get("id"): m for m in (patch.get("modules") or [])}
        for cable in (patch.get("cables") or []):
            for end, kind in (("output", "out"), ("input", "in")):
                mod = by_id.get(cable.get(f"{end}ModuleId"))
                idx = cable.get(f"{end}Id")
                if mod is None or not isinstance(idx, int):
                    continue
                role, label = ic._port_info(inv, mod, kind, idx)
                if not label:
                    continue           # uncartographed: a different gap
                ends += 1
                for pk, spec in roles["ports"].items():
                    if pk in WILDCARD or _direction(pk) != kind:
                        continue
                    if not _label_suggests(label, spec):
                        continue
                    if ic._port_matches(pk, role, label, roles):
                        continue
                    mine = set(role) if isinstance(role, list) else (
                        {role} if role else set())
                    key = (pk, label)
                    # Three very different situations wear the same shape, and
                    # lumping them together would turn a real defect into a
                    # number nobody can act on.
                    #
                    #   VETOED   the role is in `not_ports` -- deliberate, and
                    #            the one rule here added on purpose.
                    #   CONTESTED the port has a role, and it is not this
                    #            kind's. "Main Out L" is role Audio; refusing
                    #            to call it a CV output is CORRECT, and only
                    #            my token heuristic thinks otherwise because
                    #            "OUT" sits in cv_out's label list.
                    #   MISSED   nothing contradicts the label -- either the
                    #            kind matches on labels ALONE (`sync_in` has
                    #            no roles at all) or the port has no role. The
                    #            label fallback is the whole rule, and
                    #            whole-string equality is why it failed.
                    if mine & set(spec.get("not_ports") or []):
                        vetoed[key] += 1
                    elif mine and not (mine & set(spec.get("ports") or [])) \
                            and spec.get("ports"):
                        contested[key] += 1
                    else:
                        missed[key] += 1
                    examples.setdefault(
                        key, (f"{mod.get('plugin')}/{mod.get('model')}",
                              sorted(mine) or ["(no role)"], meta.get("url", "")))

    print(f"corpus: {len(corpus)} patches   labelled jack ends: {ends}")
    print(f"MISSED   {sum(missed.values()):4d} ends / {len(missed):3d} pairs"
          f"   label says the kind, nothing contradicts it, matcher says no")
    print(f"contested{sum(contested.values()):4d} ends / {len(contested):3d} pairs"
          f"   the port's role says otherwise — correct")
    print(f"vetoed   {sum(vetoed.values()):4d} ends / {len(vetoed):3d} pairs"
          f"   deliberate not_ports rule — correct\n")
    if not verbose or not missed:
        return len(missed)

    print(f"{'uses':>5}  {'port kind':<11} {'jack label':<26} {'role':<16} module")
    print(f"{'-'*5}  {'-'*11} {'-'*26} {'-'*16} {'-'*22}")
    for (pk, label), n in missed.most_common(30):
        mod, mine, _ = examples[(pk, label)]
        print(f"{n:5d}  {pk:<11} {label[:26]:<26} {','.join(mine)[:16]:<16} {mod}")
    print("\nEach row is a patch our gate would reject for a jack it did wire.")
    return len(missed)


def sweep_coverage() -> None:
    """How much of the corpus we can judge at all."""
    import patch as patch_mod                              # noqa: PLC0415
    inv = patch_mod.inventory()
    corpus = held_patches()
    buckets = Counter()
    for meta, patch in corpus:
        known, total = coverage_of(patch, inv)
        if not total:
            buckets["empty"] += 1
            continue
        frac = known / total
        buckets["all modules known" if frac == 1.0 else
                "most known (>=80%)" if frac >= 0.8 else
                "some known (>=50%)" if frac >= 0.5 else
                "mostly unknown"] += 1
    print(f"corpus: {len(corpus)} patches")
    for name in ("all modules known", "most known (>=80%)",
                 "some known (>=50%)", "mostly unknown", "empty"):
        if buckets[name]:
            print(f"  {buckets[name]:4d}  {name}")
    print("\nOnly the judgeable ones can carry a rejection rate: a module we do "
          "not own fails every requirement it touches, and that is not a gate "
          "defect.")


def sweep_idioms(min_coverage: float = 0.8) -> None:
    """Which requirements reject real patches, grouped and counted."""
    import patch as patch_mod                              # noqa: PLC0415
    inv = patch_mod.inventory()
    roles = ic.load_roles()
    idioms = ic.load_idioms()
    corpus = held_patches()

    rejects: Counter = Counter()
    tried: Counter = Counter()
    witness: dict[tuple, str] = {}
    judged = skipped = 0

    for meta, patch in corpus:
        known, total = coverage_of(patch, inv)
        if not total or known / total < min_coverage:
            skipped += 1
            continue
        judged += 1
        # What did this patch claim to be? Its own words are the nearest thing
        # to the request a user would have typed.
        prompt = " ".join([meta.get("title", "")] + (meta.get("tags") or []))
        slug = ic.resolve_exact(prompt, idioms)
        if not slug or slug not in idioms:
            continue
        problems = ic.check(patch, inv, idioms[slug], roles, [])
        tried[slug] += 1
        for p in problems:
            rejects[(slug, p)] += 1
            witness.setdefault((slug, p), meta.get("url", ""))

    print(f"corpus: {len(corpus)} patches   judged: {judged}   "
          f"skipped for low module coverage: {skipped}")
    if not rejects:
        print("no requirement rejected a judgeable patch")
        return
    print(f"\n{'rejects':>7}  {'of':>4}  idiom / requirement")
    print(f"{'-'*7}  {'-'*4}  {'-'*60}")
    for (slug, problem), n in rejects.most_common(25):
        print(f"{n:7d}  {tried[slug]:4d}  {slug}: {problem[:60]}")
        print(f"{'':15}{witness[(slug, problem)]}")


def main(argv: list[str]) -> int:
    mode = argv[1] if len(argv) > 1 else ""
    if mode == "labels":
        sweep_labels()
    elif mode == "idioms":
        sweep_idioms()
    elif mode == "coverage":
        sweep_coverage()
    else:
        print(__doc__.strip().split("\n\n")[1])
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
