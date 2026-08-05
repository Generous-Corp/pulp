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
    wanted = {t.casefold() for t in (spec.get("tags") or [])}
    if not wanted:
        return True                      # "any"
    # Case-folded, because a tag is whatever the plugin's author typed.
    # Rack's canonical spelling is "Envelope generator" and Fundamental writes
    # "Envelope Generator", so an exact comparison decided that the ADSR every
    # user has is not an envelope -- and every idiom needing one rejected a
    # correct patch while naming the envelope as the missing part.
    return bool(wanted & {t.casefold() for t in (mod_entry.get("tags") or [])})


def _port_matches(kind: str, role, label: str | None,
                  roles: dict) -> bool:
    spec = roles["ports"].get(kind)
    if spec is None:
        raise KeyError(f"idiom refers to unknown port kind {kind!r}")
    ok_roles = set(spec.get("ports") or [])
    ok_labels = {s.upper() for s in (spec.get("labels") or [])}
    if not ok_roles and not ok_labels:
        return True                      # "any_in" / "any_out"
    # A role may be several things at once: an LFO's square output is a
    # modulation source AND a clock AND a gate, and which one it is depends
    # only on where it is patched.
    mine = set(role) if isinstance(role, list) else ({role} if role else set())
    if mine & ok_roles:
        return True
    # A role that rules the kind OUT, whatever the label says. SQR and PLS are
    # gate labels because an LFO's square fires an envelope -- but an
    # oscillator's pulse output carries the same label at audio rate, and it
    # matched, so a VCO retriggering an envelope thousands of times a second
    # read as "something triggers it". The label fallback exists for modules
    # with no roles at all; it must not overrule a role that is present and
    # says otherwise.
    if mine & {s for s in (spec.get("not_ports") or [])}:
        return False
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


def _uncartographed(inv: dict, mod: dict, kind: str) -> bool:
    """Nobody has ever recorded what THIS module's jacks are.

    Distinct from "this jack is something else". A module with no port map at
    all cannot satisfy a requirement that names a port kind, so every idiom
    needing one rejected every patch built with it -- and the modules with no
    map are the vendor's, which is most of the library. The only quantizer a
    stock Rack has is one of them, so `a bass sound that stays in the key of C`
    was told its quantizer did not reach the oscillator no matter how it was
    wired.

    An unknown port therefore does not PROVE a requirement wrong. It cannot
    prove it right either, which is why `check` counts these and says so: the
    honest verdict is "holds, except where nothing could be checked", not a
    silent pass.
    """
    entry = _entry(inv, mod)
    return not (entry.get("outputs" if kind == "out" else "inputs") or [])


def check(patch: dict, inv: dict, idiom: dict, roles: dict | None = None,
          unchecked: list | None = None) -> list[str]:
    """Every requirement of `idiom` this patch fails, in words. [] means it holds.

    `unchecked`, when given, collects the requirements that were satisfied only
    because a module has no port map. Those are honest holds with a gap in
    them, and a caller that wants to say so can; a caller that does not is no
    worse off than before, when the same patches were simply rejected.
    """
    roles = roles if roles is not None else load_roles()
    by_id = {m.get("id"): m for m in patch.get("modules", [])}
    problems: list[str] = []
    if unchecked is None:
        unchecked = []

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

        # A signal that passes through a multiple, an attenuator or a slew is
        # still that signal. "The random voltage reaches the envelope's time"
        # is just as true through a mult -- which is how people actually patch
        # -- so the sources are widened to anything a source reaches through a
        # chain of those. Without this the checker rejects correct patches for
        # being tidily wired, which is the worst thing a checker can do.
        transparent = {mid for mid, m in by_id.items()
                       if any(roles["roles"].get(r, {}).get("transparent")
                              and _module_matches(r, _entry(inv, m), roles)
                              for r in roles["roles"])}
        # The widening is KIND-AWARE. A relayed source is exempt from the
        # port check at the far end -- a mult's own jacks are "1 2 3" and role
        # Cv whatever is fed in -- so the kind has to be established at the
        # cable that ENTERS the chain, or the exemption would let anything
        # through a multiple satisfy any requirement: an audio signal into an
        # envelope's trigger would read as a gate.
        # `relayed` is tracked SEPARATELY from `reached`, not as the part of it
        # that was added late. When from_module is "any" every module is a
        # candidate already, so "was it added by widening" is always false --
        # and the exemption below became dead code for exactly the
        # requirements that need it most, the ones that say a gate may come
        # from anything. Direct wiring held and the same patch through a
        # multiple did not.
        reached = set(froms)
        relayed = set()          # arrived by passing through a mult etc.
        changed = True
        while changed:
            changed = False
            for c in patch.get("cables", []):
                src, dst = c.get("outputModuleId"), c.get("inputModuleId")
                if dst not in transparent or dst in relayed:
                    continue
                if src not in reached and src not in relayed:
                    continue
                if src in relayed:
                    pass                      # already vouched for upstream
                else:
                    origin = by_id.get(src)
                    if origin is None:
                        continue
                    orole, olabel = _port_info(inv, origin, "out",
                                               c.get("outputId", 0))
                    if not _port_matches(from_port, orole, olabel, roles) \
                       and not _uncartographed(inv, origin, "out"):
                        continue              # wrong kind entering the chain
                relayed.add(dst)
                changed = True
        froms = reached | relayed

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
            # A port nobody has cartographed is UNKNOWN, not wrong. Rejecting
            # on it made every idiom unsatisfiable for the vendor modules that
            # make up most of any real rack.
            blind: list[str] = []
            # A multiple's own jacks are generic: its outputs are "1 2 3", role
            # Cv, whatever is fed into them. So the KIND of signal was settled
            # upstream, at the module the widening started from, and asking a
            # mult's output to look like a gate rejects the patch for the
            # relaying it was widened to allow. It did: a kick drum clocked
            # from an LFO's SQR through a multiple -- the ordinary way to fire
            # two envelopes from one clock -- was told nothing triggered it.
            if src_id in relayed:
                pass
            elif not _port_matches(from_port, srole, slabel, roles):
                if not _uncartographed(inv, src, "out"):
                    continue
                blind.append(f"{src.get('plugin')}/{src.get('model')}")
            if not _port_matches(to_port, drole, dlabel, roles):
                if not _uncartographed(inv, dst, "in"):
                    continue
                blind.append(f"{dst.get('plugin')}/{dst.get('model')}")
            found = True
            if blind:
                unchecked.append(
                    f"{req.get('id') or f'{from_role} → {to_role}'}: the cable "
                    f"is there, but {' and '.join(blind)} has no port map, so "
                    f"which jack it lands on could not be checked")
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
# what a behaviour requires, and whether the patch wrote it


BEHAVIOUR_PATH = os.path.join(IDIOM_DIR, "_behaviour.json")


def load_behaviours() -> dict:
    """Every behaviour flag's requirements, keyed by flag."""
    with open(BEHAVIOUR_PATH) as f:
        return json.load(f).get("behaviours", {})


# The words that mark a sequencer param as part of its PATTERN rather than
# its transport: the values a person would program a melody into. Matched
# against the names CARTOG measured, so a vendor's own spelling decides.
#
# A STOPGAP, and scoped like one. Matching names was measured at 73% over the
# 419 real params on this machine, and it fails hardest on the modules with
# the most character -- which is why `affordances.py` reads each module once
# instead. This survives for the single case where a name really does settle
# it (a param called "Step 3" on a module the library tags as a Sequencer),
# and only until that module has been classified: a classification supersedes
# it entirely, per module, as the background pass reaches them. Deleting it
# today would leave every unclassified machine with no melody check at all,
# which is the bug this whole arc exists to have fixed.
PATTERN_PARAM_WORDS = ("STEP", "PITCH", "NOTE", "SEMITONE", "DEGREE", "CV")


def pattern_param_ids(entry: dict) -> list:
    """The param ids holding a sequencer's programmable pattern, by name."""
    out = []
    for q in (entry.get("params") or []):
        if not isinstance(q, dict):
            continue
        name = str(q.get("name") or "").upper()
        if any(w in name for w in PATTERN_PARAM_WORDS):
            out.append(int(q.get("id", -1)))
    return out


def _classified(entry: dict) -> bool:
    """Whether this module has been READ, which is not the same as having
    affordances. A module whose every knob is a page switch is read and
    carries none, and the name-matching stopgap below must stay asleep
    underneath an answer that already exists."""
    if entry.get("classified"):
        return True
    return any(isinstance(q, dict) and q.get("affords")
               for q in (entry.get("params") or []))


def affording(entry: dict, words, roles: dict | None = None) -> list:
    """The param ids on one module that CONFIDENTLY carry any of `words`.

    Guesses are excluded here and nowhere else, because this is the function
    every rejection is built on. A `guessed` affordance may suggest something
    to the model and may never be the evidence a patch is refused on -- a
    wrong guess that only costs a suggestion is free, and a wrong guess that
    costs a rejection teaches the model to avoid the module.
    """
    wanted = {w.lower() for w in ([words] if isinstance(words, str) else words)}
    if _classified(entry):
        return [q["id"] for q in (entry.get("params") or [])
                if isinstance(q, dict) and isinstance(q.get("id"), int)
                and q.get("affords") in wanted
                and q.get("affordance_confidence") == "known"]
    # Unclassified: the one name-based reading that holds up, and only for
    # the affordance it holds up for.
    if "structure" in wanted:
        roles = roles if roles is not None else load_roles()
        if _module_matches("sequencer", entry, roles):
            return pattern_param_ids(entry)
    return []


class Finding:
    """One behaviour requirement a patch fails, WITH the numbers behind it.

    A rejection that says "not melodic" gives a retry nothing to act on and
    gives a person nothing to disagree with. `measured` carries what was
    actually counted -- how many params afford the thing, how many the patch
    wrote, how many distinct values it wrote -- so the next attempt can be
    told which knob to move and a human reading the log can see the check was
    wrong when it is.
    """

    def __init__(self, behaviour: str, affordances, module: str,
                 must: str, measured: dict, text: str):
        self.behaviour = behaviour
        self.affordances = tuple(affordances)
        self.module = module
        self.must = must
        self.measured = dict(measured)
        self.text = text

    def __str__(self) -> str:
        return self.text

    def __repr__(self) -> str:
        return f"<Finding {self.behaviour}/{self.must} {self.module} {self.measured}>"


class Deferral:
    """A behaviour nothing here could settle, named so the gap is visible.

    Distinct from a pass, and distinct from a failure. A sequencer that keeps
    its pattern in module `data` rather than in params, or one nobody has
    scanned, cannot be read -- and the silent version of that was how a patch
    playing one held note passed every check it was given.

    `behaviour` is the flag exactly as the idiom spells it, which is also what
    `patch_behaviour.evaluate()` takes, so a caller hands a deferral straight
    to the gate that renders audio. This class deliberately carries NO
    measurement name and NO threshold: that lane owns the predicate set, and
    two copies of it would drift with this one losing, since nothing here ever
    renders a sample.
    """

    def __init__(self, behaviour: str, affordances, why: str):
        self.behaviour = behaviour
        self.affordances = tuple(affordances)
        self.why = why

    def __str__(self) -> str:
        return f"{self.behaviour}: {self.why}"

    def __repr__(self) -> str:
        return f"<Deferral {self.behaviour}: {self.why}>"


def _written(mod: dict) -> dict:
    return {int(p["id"]): p.get("value")
            for p in (mod.get("params") or [])
            if isinstance(p, dict) and isinstance(p.get("id"), int)}


def check_behaviour(patch: dict, inv: dict, idiom: dict,
                    roles: dict | None = None,
                    behaviours: dict | None = None) -> tuple[list, list]:
    """(findings, deferrals) for every behaviour this idiom declares.

    Topology says the right modules are wired the right way. It cannot say
    whether the music exists: a sequencer can be clocked, reach the
    oscillator and fire the envelope with every step still sitting at its
    default, and the result is one held note through perfect wiring. That
    patch shipped, passed every check it was given, and a person had to
    listen to find out.

    So the idiom's `behaviour` flags -- data that has sat in these JSON files
    unread since they were written -- each name the affordances that have to
    be WRITTEN, and how. What can be proved by reading is proved here; what
    cannot is returned as a deferral rather than as a pass, because a check
    that reports nothing when it saw nothing is indistinguishable from a
    check that reports nothing because everything was fine.
    """
    roles = roles if roles is not None else load_roles()
    behaviours = behaviours if behaviours is not None else load_behaviours()
    flags = idiom.get("behaviour") or {}
    findings: list = []
    deferrals: list = []

    for flag, on in sorted(flags.items()):
        if not on:
            continue
        spec = behaviours.get(flag)
        if spec is None:
            continue                    # a flag nobody has given meaning yet
        for req in spec.get("requires", []):
            words = req.get("any_of") or []
            must = req.get("must")
            # `any_of` IS ORDERED, and the order carries a judgement.
            # "melodic" reads ["structure", "pitch"]: a patch holding anything
            # that carries the notes themselves is judged on THAT, and only a
            # patch with nothing of the kind falls through to pitch. Treating
            # the list as a set instead told a patch containing a dual
            # oscillator and a pattern-in-data sequencer to detune its two
            # oscillators against each other, which is a chord and not a
            # melody -- confident, specific, and the wrong instruction.
            hits = []                   # (module name, ids, written) per module
            for word in words:
                hits = _carriers(patch, inv, [word], must, roles)
                if hits:
                    words = [word]
                    break
            if not hits:
                deferrals.append(Deferral(
                    flag, words,
                    f"nothing in this patch has measured params that "
                    f"{'vary' if must == 'vary' else 'carry'} "
                    f"{' or '.join(words)}, so reading the patch cannot "
                    f"settle it — the gate that listens has to"))
                continue
            findings.extend(_judge(flag, words, req, must, hits))

        if not spec.get("requires"):
            deferrals.append(Deferral(
                flag, [], "settled by listening, not by reading"))
    return findings, deferrals


def _carriers(patch: dict, inv: dict, words, must: str, roles: dict) -> list:
    """(name, ids, written) for every module that could satisfy a requirement."""
    out = []
    for m in patch.get("modules", []):
        entry = _entry(inv, m)
        ids = affording(entry, words, roles)
        if not ids:
            continue
        if must == "vary" and len(ids) < 2:
            # One knob cannot differ from itself. Asking it to would be a
            # requirement no patch could ever satisfy, which is not a check
            # -- it is a wall.
            continue
        out.append((entry.get("name") or m.get("model") or "a module",
                    ids, _written(m)))
    return out


def _judge(flag: str, words, req: dict, must: str, hits: list) -> list:
    """The findings for one requirement over the modules that could satisfy it."""
    remedy = req.get("remedy") or ""
    named = " or ".join(words)

    if must == "vary":
        candidates = []
        for name, ids, written in hits:
            values = [written[i] for i in ids if i in written]
            distinct = {round(float(v), 6) for v in values
                        if isinstance(v, (int, float))}
            candidates.append((name, ids, values, distinct))
        # ANY module carrying the variation satisfies the intent. A patch
        # with a written melody on one sequencer and a second sequencer left
        # at its defaults is still a melodic patch, and rejecting it would
        # punish the arrangement rather than the fault.
        if any(len(d) >= 2 for _, _, _, d in candidates):
            return []
        out = []
        for name, ids, values, distinct in candidates:
            measured = {"params": len(ids), "written": len(values),
                        "distinct": len(distinct)}
            what = ("writes none of them" if not values else
                    f"writes {len(values)} of them, all to the same value")
            out.append(Finding(
                flag, words, name, must, measured,
                f"{name} has {len(ids)} params that carry {named} "
                f"({'the notes themselves' if 'structure' in words else named}) "
                f"and the patch {what} — measured: {len(ids)} params, "
                f"{len(values)} written, {len(distinct)} distinct value(s). "
                + remedy))
        return out

    if must == "nonzero":
        out = []
        for name, ids, written in hits:
            zeros = [i for i in ids
                     if isinstance(written.get(i), (int, float))
                     and float(written[i]) == 0.0]
            # EVERY one of them, not any of them. An eight-row attenuverter
            # with four rows deliberately zeroed is four unused channels and
            # a working patch; the fault this names is a module with no route
            # left to move at all. Requiring only one zero would reject the
            # ordinary arrangement and teach the model to avoid the module.
            if not zeros or len(zeros) != len(ids):
                continue
            out.append(Finding(
                flag, words, name, must,
                {"params": len(ids), "zeroed": len(zeros), "ids": zeros},
                f"{name}'s {named} params are all written to exactly zero "
                f"(param{'s' if len(zeros) > 1 else ''} "
                + ", ".join(str(i) for i in zeros) +
                f") — measured: {len(zeros)} of {len(ids)} at zero. "
                + remedy))
        return out

    return []


def check_written(patch: dict, inv: dict, idiom: dict,
                  roles: dict | None = None) -> list[str]:
    """`check_behaviour`'s findings as sentences, for callers that want prose.

    Deferrals are deliberately dropped here: a caller taking a list of
    strings is deciding whether to REJECT, and a thing nobody could measure
    is not grounds to reject anything.
    """
    findings, _ = check_behaviour(patch, inv, idiom, roles)
    return [str(f) for f in findings]


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

    unchecked: list[str] = []
    problems = check(patch, inv, idiom, None, unchecked)
    if not problems:
        print(f"  {slug}: holds")
        # A hold with a gap in it says so. Silence here would be the same
        # sentence for "every jack was verified" and "one of them could not
        # be looked at", which are not the same claim.
        for note in unchecked:
            print(f"    (not fully checked) {note}")
        return 0
    print(f"  {slug}: not a {slug} patch yet —")
    for p in problems:
        print(f"    - {p}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
