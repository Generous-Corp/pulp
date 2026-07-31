#!/usr/bin/env python3
"""Does this patch actually do the thing it was asked for?

The gates that existed before this one asked whether a patch made a sound.
Every patch that disappointed passed them: a drone that came back a tone, a
krell that played one note, a bouncing ball with no trigger source. Sound is
necessary and says nothing about whether the patch is what was requested.

An idiom names a STRUCTURE -- "a random voltage sampled to set an envelope's
decay, with end-of-cycle retriggering it" is a method, and a method is
checkable. This module checks it, against the same library the model is told
about, so a rejection can say which connection is missing rather than "no".

    python3 idiom_check.py <patch.vcv> [idiom-slug]
    python3 idiom_check.py --self-test          # every idiom's own mistakes

With no slug the idiom is resolved from the patch's name; `--self-test` is the
important one, and is described at `self_test` below.
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IDIOM_DIR = os.path.join(HERE, "patch_idioms")


# --------------------------------------------------------------------------
# the library


def load_roles() -> dict:
    with open(os.path.join(IDIOM_DIR, "_roles.json")) as f:
        return json.load(f)


def load_idioms() -> dict:
    """Every idiom, keyed by slug. Families are files; slugs are global."""
    out: dict[str, dict] = {}
    if not os.path.isdir(IDIOM_DIR):
        return out
    for name in sorted(os.listdir(IDIOM_DIR)):
        if not name.endswith(".json") or name.startswith("_"):
            continue
        with open(os.path.join(IDIOM_DIR, name)) as f:
            doc = json.load(f)
        for idiom in doc.get("idioms", []):
            slug = idiom.get("slug")
            if not slug:
                continue
            if slug in out:
                raise ValueError(f"two idioms share the slug {slug!r}")
            idiom.setdefault("family", doc.get("family", name[:-5]))
            out[slug] = idiom
    return out


def resolve(prompt: str, idioms: dict | None = None) -> str | None:
    """Which idiom a prompt is asking for, decided here and not by the model.

    The model naming its own idiom and then being graded against that claim is
    the model grading its own homework: every rejection teaches it to claim
    something less demanding. So the claim is made outside it, from the words
    the person actually typed.

    `names` match the idiom by name; `implies` match a description of it, which
    is the half that would otherwise never be tested -- most people do not know
    the word "krell", they ask for a patch that plays itself.
    """
    idioms = idioms if idioms is not None else load_idioms()
    text = " " + " ".join(prompt.lower().replace("-", " ").split()) + " "

    best: tuple[int, str] | None = None
    for slug, idiom in idioms.items():
        for name in idiom.get("names", []):
            if f" {name.lower()} " in text:
                # A name is a direct request and outranks any description.
                score = 1000 + len(name)
                if best is None or score > best[0]:
                    best = (score, slug)
        for phrase in idiom.get("implies", []):
            if f" {phrase.lower()} " in text:
                score = len(phrase)
                if best is None or score > best[0]:
                    best = (score, slug)
    return best[1] if best else None


# --------------------------------------------------------------------------
# matching a patch against one idiom


def _module_matches(role: str, mod_entry: dict, roles: dict) -> bool:
    spec = roles["roles"].get(role)
    if spec is None:
        raise KeyError(f"idiom refers to unknown role {role!r}")
    wanted = set(spec.get("tags") or [])
    if not wanted:
        return True                      # "any"
    return bool(wanted & set(mod_entry.get("tags") or []))


def _port_matches(kind: str, role: str | None, label: str | None,
                  roles: dict) -> bool:
    spec = roles["ports"].get(kind)
    if spec is None:
        raise KeyError(f"idiom refers to unknown port kind {kind!r}")
    ok_roles = set(spec.get("ports") or [])
    ok_labels = {s.upper() for s in (spec.get("labels") or [])}
    if not ok_roles and not ok_labels:
        return True                      # "any_in" / "any_out"
    if role and role in ok_roles:
        return True
    # The label fallback matters: a module nobody has cartographed has no port
    # roles at all, and skipping those patches would exempt the modules most
    # likely to be miswired.
    return bool(label and label.upper() in ok_labels)


def _entry(inv: dict, mod: dict) -> dict:
    return (inv.get(mod.get("plugin"), {}).get("modules", {})
               .get(mod.get("model"), {}))


def _port_info(inv: dict, mod: dict, kind: str, index: int):
    """(role, label) for one port, or (None, None) when unknown."""
    entry = _entry(inv, mod)
    names = entry.get("outputs" if kind == "out" else "inputs") or []
    roles = entry.get("roles_out" if kind == "out" else "roles_in") or []
    label = names[index] if index < len(names) else None
    role = roles[index] if index < len(roles) else None
    return role, label


def check(patch: dict, inv: dict, idiom: dict, roles: dict | None = None) -> list[str]:
    """Every requirement of `idiom` this patch fails, in words. [] means it holds."""
    roles = roles if roles is not None else load_roles()
    by_id = {m.get("id"): m for m in patch.get("modules", [])}
    problems: list[str] = []

    # Which modules could play each named part.
    def candidates(role: str) -> set:
        return {mid for mid, m in by_id.items()
                if _module_matches(role, _entry(inv, m), roles)}

    for req in idiom.get("topology", []):
        from_role = req.get("from_module", "any")
        to_role = req.get("to_module", "any")
        from_port = req.get("from_port", "any_out")
        to_port = req.get("to_port", "any_in")
        want_same = bool(req.get("same_module"))

        froms, tos = candidates(from_role), candidates(to_role)
        found = False
        for c in patch.get("cables", []):
            src_id, dst_id = c.get("outputModuleId"), c.get("inputModuleId")
            if src_id not in froms or dst_id not in tos:
                continue
            if want_same and src_id != dst_id:
                continue
            if req.get("different_module") and src_id == dst_id:
                continue
            src, dst = by_id.get(src_id), by_id.get(dst_id)
            if src is None or dst is None:
                continue
            srole, slabel = _port_info(inv, src, "out", c.get("outputId", 0))
            drole, dlabel = _port_info(inv, dst, "in", c.get("inputId", 0))
            if not _port_matches(from_port, srole, slabel, roles):
                continue
            if not _port_matches(to_port, drole, dlabel, roles):
                continue
            found = True
            break

        if not found:
            # Named in the words a person would recognise, because a rejection
            # that says "requirement 3 failed" teaches nobody anything.
            problems.append(req.get("describe") or
                            f"missing: {from_role} → {to_role}")

    for req in idiom.get("forbidden", []):
        role = req.get("module", "any")
        if candidates(role):
            problems.append(req.get("describe") or f"should not contain a {role}")

    least = idiom.get("at_least", {})
    for role, n in least.items():
        have = len(candidates(role))
        if have < n:
            problems.append(f"needs at least {n} {role} module(s), found {have}")

    return problems


# --------------------------------------------------------------------------
# the negative controls


def apply_mistake(patch: dict, mistake: dict, inv: dict, roles: dict) -> dict | None:
    """A copy of `patch` with one idiomatic mistake made in it.

    This is what turns sixty prose warnings into sixty negative controls. A
    topology requirement nobody has ever seen FAIL is not a check -- it is a
    sentence that happens to be true of whatever was tested.
    """
    out = json.loads(json.dumps(patch))
    kind = mistake.get("do")

    if kind == "cut":
        # Remove the cables satisfying one requirement, by its id.
        target = mistake.get("requirement")
        req = next((r for r in mistake.get("_topology", [])
                    if r.get("id") == target), None)
        if req is None:
            return None
        by_id = {m.get("id"): m for m in out.get("modules", [])}

        def matches(c) -> bool:
            src, dst = by_id.get(c.get("outputModuleId")), by_id.get(c.get("inputModuleId"))
            if src is None or dst is None:
                return False
            if not _module_matches(req.get("from_module", "any"), _entry(inv, src), roles):
                return False
            if not _module_matches(req.get("to_module", "any"), _entry(inv, dst), roles):
                return False
            if req.get("same_module") and src.get("id") != dst.get("id"):
                return False
            srole, slabel = _port_info(inv, src, "out", c.get("outputId", 0))
            drole, dlabel = _port_info(inv, dst, "in", c.get("inputId", 0))
            return (_port_matches(req.get("from_port", "any_out"), srole, slabel, roles)
                    and _port_matches(req.get("to_port", "any_in"), drole, dlabel, roles))

        before = len(out.get("cables", []))
        out["cables"] = [c for c in out.get("cables", []) if not matches(c)]
        if len(out["cables"]) == before:
            return None                  # nothing to cut: not a real control
        return out

    if kind == "drop_module":
        role = mistake.get("module")
        keep, dropped = [], set()
        for m in out.get("modules", []):
            if not dropped and _module_matches(role, _entry(inv, m), roles):
                dropped.add(m.get("id"))
                continue
            keep.append(m)
        if not dropped:
            return None
        out["modules"] = keep
        out["cables"] = [c for c in out.get("cables", [])
                         if c.get("outputModuleId") not in dropped
                         and c.get("inputModuleId") not in dropped]
        return out

    return None


def self_test(verbose: bool = True) -> int:
    """Prove every idiom can FAIL, using its own list of common mistakes.

    A requirement that has never been seen to reject anything is indistinguish-
    able from a requirement that cannot reject anything. So for each idiom we
    build the smallest patch that satisfies it, confirm it passes, then make
    each documented mistake in turn and require the checker to fail AND to name
    the right thing.
    """
    roles = load_roles()
    idioms = load_idioms()
    inv = _fixture_inventory(roles)
    bad = 0
    checked = 0

    for slug, idiom in sorted(idioms.items()):
        patch = synthesize(idiom, inv, roles)
        if patch is None:
            print(f"  WRONG  {slug}: cannot be built — two of its requirements "
                  f"want the same input jack, and Rack takes one cable per input")
            bad += 1
            continue

        problems = check(patch, inv, idiom, roles)
        if problems:
            print(f"  WRONG  {slug}: its own minimal patch fails it: {problems}")
            bad += 1
            continue

        mistakes = idiom.get("common_mistakes") or []
        if not mistakes:
            print(f"  WRONG  {slug}: no common_mistakes, so nothing proves "
                  f"this idiom can reject anything")
            bad += 1
            continue

        for mistake in mistakes:
            mistake = dict(mistake)
            mistake["_topology"] = idiom.get("topology", [])
            broken = apply_mistake(patch, mistake, inv, roles)
            if broken is None:
                print(f"  WRONG  {slug}/{mistake.get('id')}: the mistake could "
                      f"not be made, so it controls nothing")
                bad += 1
                continue
            got = check(broken, inv, idiom, roles)
            if not got:
                print(f"  WRONG  {slug}/{mistake.get('id')}: still passes with "
                      f"the mistake made")
                bad += 1
                continue
            checked += 1
            if verbose:
                print(f"  ok     {slug}/{mistake.get('id')} → {got[0]}")

    print(f"\n{len(idioms)} idioms, {checked} negative controls, {bad} wrong")
    return 1 if bad else 0


# --------------------------------------------------------------------------
# a fixture rack, so the self-test needs no installed library


def _fixture_inventory(roles: dict) -> dict:
    """One module per role, carrying that role's tags and generous ports.

    Deliberately synthetic: the self-test is about whether the IDIOMS can fail,
    and running it against whatever happens to be installed would make it pass
    or fail for reasons that have nothing to do with the library.
    """
    modules = {}
    for role, spec in roles["roles"].items():
        tags = list(spec.get("tags") or [])
        # The jack list repeats so a fan-in module's second and third inputs
        # still resolve to the same KIND. Without the repeats the mixer's
        # extra jacks fell off the end of the list and the idiom failed its
        # own patch -- a fixture artefact reported as a library fault.
        ins = ["IN", "V/OCT", "GATE", "CLK", "CV", "TRIG", "TIME", "SPARE"]
        in_roles = ["Audio", "Pitch", "Gate", "Clock", "Cv", "Trigger", "Cv", "Cv"]
        outs = ["OUT", "GATE", "EOC", "CV", "SAW", "CLK", "TRIG", "SPARE"]
        out_roles = ["Audio", "Gate", "Trigger", "Cv", "Audio", "Clock",
                     "Trigger", "Cv"]
        modules[role] = {
            "name": role.upper(),
            "tags": tags,
            "inputs": ins * 4,
            "roles_in": in_roles * 4,
            "outputs": outs * 4,
            "roles_out": out_roles * 4,
        }
    return {"Fixture": {"modules": modules}}


_PORT_INDEX = {
    "audio_in": 0, "pitch_in": 1, "gate_in": 2, "clock_in": 3, "cv_in": 4,
    "any_in": 0,
    "audio_out": 0, "gate_out": 1, "cycle_out": 2, "cv_out": 3,
    "clock_out": 5, "any_out": 0,
}


def synthesize(idiom: dict, inv: dict, roles: dict) -> dict | None:
    """The smallest patch satisfying an idiom, built from the fixture rack."""
    modules: list[dict] = []
    ids: dict[str, int] = {}

    def module_for(role: str, key: str | None = None) -> int:
        # The KEY distinguishes two instances; the MODEL is what the inventory
        # is looked up by. Conflating them made the second oscillator of an FM
        # pair a module with no tags, so it matched no role and the idiom
        # failed its own patch.
        key = key or role
        if key not in ids:
            ids[key] = len(modules) + 1
            modules.append({"id": ids[key], "plugin": "Fixture", "model": role})
        return ids[key]

    for role, n in (idiom.get("at_least") or {}).items():
        for i in range(n):
            module_for(role, role if i == 0 else f"{role}#{i}")

    # Rack allows exactly ONE cable per input: it keeps the last and silently
    # drops the rest. An idiom whose requirements need two cables in the same
    # jack cannot be built, and the fixture has to refuse it here or the
    # self-test certifies an idiom no real patch can ever satisfy. This is
    # precisely what happened to the first bouncing-ball record.
    taken: set[tuple[int, int]] = set()
    cables = []
    for req in idiom.get("topology", []):
        f_role = req.get("from_module", "any")
        t_role = req.get("to_module", "any")
        src = module_for(f_role)
        dst = src if req.get("same_module") else module_for(t_role)
        if not req.get("same_module") and req.get("different_module") and src == dst:
            dst = module_for(t_role, t_role + "#alt")
        in_index = _PORT_INDEX.get(req.get("to_port", "any_in"), 0)
        if (dst, in_index) in taken:
            if roles["roles"].get(t_role, {}).get("fan_in"):
                # A mixer or a multiple really does take several cables of a
                # kind, so it gets another jack.
                while (dst, in_index) in taken:
                    in_index += 8
            elif f_role == t_role and (src, in_index) not in taken:
                # Two requirements between one pair of same-role modules is a
                # MUTUAL link -- cross-modulation, where each modulates the
                # other. The second cable runs the other way.
                src, dst = dst, src
                taken.add((dst, in_index))
            else:
                # Everything else takes one cable per input: Rack's own rule,
                # and an idiom that needs two is unbuildable.
                return None
        taken.add((dst, in_index))
        cables.append({
            "outputModuleId": src,
            "outputId": _PORT_INDEX.get(req.get("from_port", "any_out"), 0),
            "inputModuleId": dst,
            "inputId": in_index,
        })

    if not modules:
        return None
    return {"modules": modules, "cables": cables}


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test(verbose="--quiet" not in argv)
    if len(argv) < 2:
        print(__doc__)
        return 2

    patch = json.load(open(argv[1]))
    sys.path.insert(0, HERE)
    import patch as patch_mod           # noqa: PLC0415 - optional, heavy
    inv = patch_mod.inventory()
    idioms = load_idioms()

    slug = argv[2] if len(argv) > 2 else resolve(
        os.path.basename(argv[1]).replace("-", " "), idioms)
    if not slug:
        print("no idiom claimed and none implied by the name — nothing to check")
        return 0
    idiom = idioms.get(slug)
    if idiom is None:
        print(f"no such idiom: {slug}")
        return 2

    problems = check(patch, inv, idiom)
    if not problems:
        print(f"  {slug}: holds")
        return 0
    print(f"  {slug}: not a {slug} patch yet —")
    for p in problems:
        print(f"    - {p}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
