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

The library holds two sizes of record. A `whole` idiom is a patch, from a
source to the way out. A `fragment` is a technique that lives inside one -- a
vibrato that fades in, a lag that turns a jump into a glide -- checked exactly
the same way, minus the audible tail, which belongs to whatever it decorates.
See KINDS below.

    python3 idiom_check.py <patch.vcv> [idiom-slug]
    python3 idiom_check.py --self-test          # every idiom's own mistakes

With no slug the idiom is resolved from the patch's name; `--self-test` is the
important one, and is described at `self_test` below. It also runs
`library_problems`, which asks whether the RECORDS are well formed -- a
different question from whether they can reject a patch, and the one that rots
silently.
"""

from __future__ import annotations

import json
import os
import sys

from typing import NamedTuple

HERE = os.path.dirname(os.path.abspath(__file__))
IDIOM_DIR = os.path.join(HERE, "patch_idioms")


# TWO KINDS OF RECORD, BECAUSE TECHNIQUE COMES IN TWO SIZES.
#
# A `whole` idiom is a patch: it names every connection from a source to the
# way out, and a patch either is one or is not. That is the shape every record
# in this library had, and it is the wrong shape for most of what a technique
# book actually teaches. Strange's units are not patches — a vibrato that
# decorates any voice, an envelope that opens two things at once, a lag that
# turns a jump into a glide — they are things you do INSIDE a patch, and
# writing them as whole idioms would mean inventing an arbitrary host for each
# and then checking the host instead of the technique.
#
# So a `fragment` names a technique and nothing else. It carries the same
# topology in the same words, is asserted by the same `check`, and is proved
# able to fail by the same negative controls. The one rule that distinguishes
# it: A FRAGMENT MAY NOT REQUIRE THE AUDIBLE TAIL. "and it reaches the output"
# belongs to the host, and a fragment that claimed it would be unsatisfiable
# in every patch that puts anything after it. `library_problems` enforces that,
# because a fragment quietly requiring an output is a fragment nothing can
# ever compose with.
KINDS = ("whole", "fragment")

# Which part of a host patch a fragment decorates. A closed set, so a slot is
# a thing you can ask for ("what could fill the timbre slot of this voice?")
# rather than free text nobody can index.
#
# Deliberately declared BY THE FRAGMENT rather than listed on the host. A host
# listing its permitted fragments is an N×M table that goes stale the moment
# somebody adds a fragment and forgets a host — and the failure is invisible,
# because a missing entry looks exactly like "nothing fits here". One
# declaration per fragment cannot rot that way, and the host-to-fragment
# listing is derived from it (`fragments_for`).
SLOTS = ("pitch", "timbre", "level", "timing", "space", "control")

# How this record came to be written, which is NOT the same question as whether
# it is correct.
#
#   read      the text is in `.corpus/` and the record quotes it in `anchor`.
#             provenance_check.py verifies the quote and demotes anything that
#             fails, so this tier is the only one that can be trusted without
#             taking somebody's word.
#   canon     standard technique written from general knowledge of the
#             practice. The honest label for most of a library like this, and
#             not a lesser one — a technique being canon is what makes it worth
#             recording. It does not become `read` because the technique also
#             appears in a book somebody could have consulted; it almost always
#             does, and that is the whole reason it is canon.
#   inferred  reasoned out from an adjacent technique.
#
# These strings feed a published acknowledgements notice. That is why the
# distinction is enforced rather than trusted: a notice built from citations
# nobody verified thanks authors for work that was never read.
PROVENANCE = ("read", "canon", "inferred")


# --------------------------------------------------------------------------
# the library


def load_roles() -> dict:
    with open(os.path.join(IDIOM_DIR, "_roles.json")) as f:
        return json.load(f)


def load_measurements() -> dict:
    """Every number the gate reports, keyed by its dotted field name."""
    with open(os.path.join(IDIOM_DIR, "_measurements.json")) as f:
        return json.load(f).get("fields", {})


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
            # A file may carry a default family and an idiom may override it.
            # Provenance and taxonomy are different axes: the ARP 2600 patches
            # arrived together and belong apart, one to voice, one to texture,
            # one to rhythm. Grouping them by the manual they came from would
            # file them under where they were found rather than under what
            # they are, which is the one thing the model reads the grouping
            # for.
            idiom.setdefault("family", doc.get("family", name[:-5]))
            idiom.setdefault("kind", doc.get("kind", "whole"))
            out[slug] = idiom
    return out


def wholes(idioms: dict) -> dict:
    return {s: i for s, i in idioms.items() if i.get("kind") != "fragment"}


def fragments(idioms: dict) -> dict:
    return {s: i for s, i in idioms.items() if i.get("kind") == "fragment"}


def fragments_for(slug: str, idioms: dict | None = None) -> list[str]:
    """The fragments that can decorate one host idiom, by slot.

    Derived, never written down: a fragment says which hosts it suits (or says
    nothing, meaning any), and this reads that. The inverse table would be a
    second place to forget an entry.
    """
    idioms = idioms if idioms is not None else load_idioms()
    out = []
    for fslug, frag in sorted(fragments(idioms).items()):
        hosts = frag.get("hosts")
        if hosts is None or slug in hosts:
            out.append(fslug)
    return out


def resolve_exact(prompt: str, idioms: dict | None = None) -> str | None:
    """Which idiom a prompt is asking for, decided here and not by the model.

    The model naming its own idiom and then being graded against that claim is
    the model grading its own homework: every rejection teaches it to claim
    something less demanding. So the claim is made outside it, from the words
    the person actually typed.

    `names` match the idiom by name; `implies` match a description of it, which
    is the half that would otherwise never be tested -- most people do not know
    the word "krell", they ask for a patch that plays itself.

    A WHOLE IDIOM OUTRANKS A FRAGMENT AT THE SAME STRENGTH. Both are askable --
    "add a delayed vibrato" is a direct request for a fragment and should
    resolve to one -- but a request that matches both equally well is asking
    for a patch, not for a decoration, and resolving it to the decoration would
    grade the patch on the one part of it nobody asked about. Before this the
    tie was broken by whichever file happened to be read first.
    """
    idioms = idioms if idioms is not None else load_idioms()
    text = " " + " ".join(prompt.lower().replace("-", " ").split()) + " "

    best: tuple[tuple[int, int], str] | None = None
    for slug, idiom in idioms.items():
        rank = 0 if idiom.get("kind") == "fragment" else 1
        for name in idiom.get("names", []):
            if f" {name.lower()} " in text:
                # A name is a direct request and outranks any description.
                score = (1000 + len(name), rank)
                if best is None or score > best[0]:
                    best = (score, slug)
        for phrase in idiom.get("implies", []):
            if f" {phrase.lower()} " in text:
                score = (len(phrase), rank)
                if best is None or score > best[0]:
                    best = (score, slug)
    return best[1] if best else None


# Endings that change a word's part of speech without changing what it names.
# "melodic"/"melody", "evolving"/"evolve", "rhythmic"/"rhythm" are the same
# request spelled for different grammar, and an exact substring match sees
# three different words.
_ENDINGS = ("ically", "ingly", "ing", "ical", "ies", "ied", "ic", "al",
            "ed", "es", "s", "y")

# Words that appear in almost every prompt and in almost every idiom phrase,
# so a shared one is evidence of nothing.
_EMPTY = {"a", "an", "the", "and", "or", "of", "with", "for", "in", "on",
          "to", "that", "this", "it", "its", "some", "something", "make",
          "makes", "made", "me", "my", "please", "patch", "sound", "sounds",
          "simple", "basic", "nice", "good", "using", "only", "just", "very",
          "really", "highly", "bit", "little", "more", "less",
          # Words about the request rather than the music. "modules" stemmed
          # to "modul" and was reported as a word the library did not know,
          # which is true and useless -- every prompt says it.
          "module", "modules", "modul", "rack", "build", "create", "want",
          "need", "like", "use", "few", "get",
          # Counts. "four bars of blorp" matched kick-drum on "four", which
          # says nothing about what kind of patch was asked for.
          "one", "two", "three", "four", "five", "six", "seven", "eight",
          "nine", "ten", "sixteen", "bar", "bars", "beat", "beats"}


def _stem(word: str) -> str:
    """A crude stem: enough to see "melodic" and "melody" as one word.

    Deliberately not a real stemmer. A real one is a dependency and a source
    of surprises, and the job here is only to stop grammar from hiding a match
    that a reader would call obvious. It is allowed to be wrong; nothing it
    decides can reject a patch (see `resolve_intent`).
    """
    w = word.lower()
    for end in _ENDINGS:
        if len(w) > len(end) + 3 and w.endswith(end):
            return w[: -len(end)]
    return w


def _words(text: str) -> set[str]:
    keep = "".join(c if c.isalnum() or c.isspace() else " " for c in text)
    return {_stem(w) for w in keep.lower().split()
            if w not in _EMPTY and len(w) > 2}


def reading(prompt: str, idioms: dict | None = None) -> dict:
    """What the library recognises in a request, and what it does not.

    Deliberately not a classifier and deliberately not a gate. `resolve_intent`
    already picks one idiom, which is a single answer to a request that is
    usually several things at once -- "a shimmering ambient pad" is a texture,
    a mood and a voice, and forcing it into one slug throws two of them away.

    This throws nothing away and decides nothing. It splits the request into
    the words that carry meaning and says, for each, which idioms use that
    word -- and, more usefully, which words NOTHING uses.

    The unknown list is the point. A word the library has never seen is the
    part of a request we are provably not checking, and it is invisible in
    every other output: a patch that satisfies "pad" and ignores "shimmering"
    passes everything, because nothing ever said "shimmering" was a word we
    do not know. Saying so costs nothing and is the difference between a
    reading somebody can correct and an answer they have to take on trust.
    """
    idioms = idioms if idioms is not None else load_idioms()
    asked = _words(prompt)
    known: dict[str, list[str]] = {}
    for slug, idiom in idioms.items():
        words = _words(" ".join(list(idiom.get("names", [])) +
                                list(idiom.get("implies", []))) +
                       " " + slug.replace("-", " "))
        for w in asked & words:
            known.setdefault(w, []).append(slug)
    return {"known": {w: sorted(s) for w, s in sorted(known.items())},
            "unknown": sorted(asked - set(known))}


class Intent(NamedTuple):
    """What a prompt was taken to be asking for, and how sure we are.

    `gating` is the load-bearing field and the reason this is not just a slug.
    An idiom reached by NAME is a direct request and may reject a patch that
    fails it. An idiom reached by resemblance is a guess about what somebody
    meant, and a guess must never reject: rejecting on a resemblance teaches
    the model to satisfy our guess instead of the request, and leaves the
    person no way to see that the check was hollow. Same rule the affordance
    tiers follow -- a `guessed` label informs and never disqualifies.
    """
    slug: str | None
    how: str        # "named" | "implied" | "nearest" | "none"
    gating: bool
    why: str


def resolve_intent(prompt: str, idioms: dict | None = None) -> Intent:
    """Always answer. Never silently check nothing.

    `resolve_exact` returns None whenever a request is worded in a way no
    idiom happens to spell, and the pipeline then falls through to wiring and
    audibility -- the two checks almost every patch passes. That is how a
    request for a melody was graded only on whether it made a noise.

    Being loud about the gap was the first fix and it was not enough: a
    transcript line tells a person that nothing was checked, it does not check
    anything. So resolution is total. When no idiom is named or implied we
    look for the nearest one by word stem, and when even that finds nothing we
    say so explicitly rather than returning a hole for a caller to interpret.

    What the tiers may do differs, and that is the point:

      named/implied  the request said it; may reject a patch
      nearest        a resemblance; informs the model, never rejects
      none           nothing resembled it; informs, never rejects

    A caller that only wants the strong answer reads `.gating`. A caller that
    wants to tell the model what is expected reads `.slug` whatever the tier.
    """
    idioms = idioms if idioms is not None else load_idioms()

    exact = resolve_exact(prompt, idioms)
    if exact is not None:
        named = any(f" {n.lower()} " in
                    " " + " ".join(prompt.lower().replace("-", " ").split()) + " "
                    for n in idioms.get(exact, {}).get("names", []))
        how = "named" if named else "implied"
        return Intent(exact, how, True,
                      f"the request {'names' if named else 'describes'} "
                      f"{exact}")

    asked = _words(prompt)
    if asked:
        # Rarity, not count. Counting shared words picked `wandering-drone`
        # for "a shimmering ambient pad" -- two common words, "ambient" and
        # "pad" -- over the idiom actually called `shimmer`, which shared the
        # one word that meant something. A word appearing in one idiom's
        # phrasing is evidence; a word appearing in twenty is furniture.
        # A word from the idiom's NAME outranks any number of words from its
        # description -- the same precedence `resolve_exact` already applies,
        # for the same reason. "a shimmering ambient pad" shares two
        # description words with `wandering-drone` and one NAME with the idiom
        # called `shimmer`, and `shimmer` is plainly the better answer.
        named_words = {slug: _words(" ".join(idiom.get("names", [])) + " " +
                                    slug.replace("-", " "))
                       for slug, idiom in idioms.items()}
        vocab = {slug: named_words[slug] |
                       _words(" ".join(idiom.get("implies", [])))
                 for slug, idiom in idioms.items()}
        seen: dict[str, int] = {}
        for words in vocab.values():
            for w in words:
                seen[w] = seen.get(w, 0) + 1

        best: tuple[float, str] | None = None
        for slug, words in vocab.items():
            shared = asked & words
            if not shared:
                continue
            score = sum((8.0 if w in named_words[slug] else 1.0) / seen[w]
                        for w in shared)
            if best is None or score > best[0]:
                best = (score, slug)

        # One furniture word is not a resemblance. A guide picked on a word
        # like "pad" is worse than saying nothing, because it points the model
        # somewhere and reads as though we understood the request.
        if best is not None and best[0] >= 0.25:
            shared = sorted(asked & vocab[best[1]],
                            key=lambda w: seen[w])
            return Intent(best[1], "nearest", False,
                          f"no idiom matched; {best[1]} is the nearest by "
                          f"wording ({', '.join(shared)}), so it is offered as "
                          f"a guide and cannot reject this patch")

    return Intent(None, "none", False,
                  "no idiom matched this request and none resembled it; only "
                  "the wiring and audibility of the patch will be checked")


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
        found, blind = _satisfied(patch, by_id, inv, roles, req, candidates,
                                  blind_ok=True)
        if found:
            if blind:
                unchecked.append(
                    f"{req.get('id') or 'a requirement'}: the cable is there, "
                    f"but {' and '.join(blind)} has no port map, so which jack "
                    f"it lands on could not be checked")
        else:
            # Named in the words a person would recognise, because a rejection
            # that says "requirement 3 failed" teaches nobody anything.
            problems.append(req.get("describe") or
                            f"missing: {req.get('from_module', 'any')} → "
                            f"{req.get('to_module', 'any')}")

    # A TECHNIQUE CAN BE DEFINED BY WHAT IS NOT PATCHED.
    #
    # A filter played as an oscillator is a filter with nothing in its audio
    # input: the resonance is the sound. Every positive requirement it could
    # carry is also true of an ordinary filtered voice, so written with
    # `topology` alone the idiom is a sentence that cannot fail. `forbidden`
    # already refuses a MODULE; this refuses a CONNECTION, which is the shape
    # an absence usually has.
    #
    # A blind match does not count as a violation. Elsewhere an unreadable
    # jack is leniency in the direction of accepting the patch; here the same
    # leniency runs the other way, and rejecting a patch because a module
    # nobody cartographed MIGHT have the forbidden cable would punish exactly
    # the modules we know least about.
    for req in idiom.get("forbidden_topology", []):
        found, _ = _satisfied(patch, by_id, inv, roles, req, candidates,
                              blind_ok=False)
        if found:
            problems.append(req.get("describe") or
                            f"should not contain: {req.get('from_module', 'any')}"
                            f" → {req.get('to_module', 'any')}")

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


def _satisfied(patch: dict, by_id: dict, inv: dict, roles: dict, req: dict,
               candidates, blind_ok: bool) -> tuple[bool, list[str]]:
    """Is there a cable answering `req`, and which of its ends went unread.

    Shared by the requirements a patch MUST satisfy and the ones it must NOT,
    so the two can never disagree about what "this cable exists" means. The
    only difference between the two callers is `blind_ok`: whether a cable
    landing on a jack nobody has cartographed counts.
    """
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
        if blind and not blind_ok:
            # The caller is asking whether a FORBIDDEN cable is there. A jack
            # nobody has read is not evidence that it is.
            continue
        return True, blind
    return False, []


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
# A STOPGAP, and scoped like one. Matching names cannot work in general
# because one name means different things on different modules -- "frequency"
# is pitch on an oscillator, time on an LFO, timbre on a filter's cutoff, all
# three confidently labelled on this machine -- which is why
# `affordances.py` reads each module once instead, and why this list may
# answer `structure` and nothing else. It survives for the case where a name
# really does settle
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
        raised = False
        why = None
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
                why = (f"nothing in this patch has measured params that "
                       f"{'vary' if must == 'vary' else 'carry'} "
                       f"{' or '.join(words)}, so reading cannot settle it "
                       f"— the gate that listens has to")
                continue
            broke = _judge(flag, words, req, must, hits)
            if broke:
                findings.extend(broke)
                raised = True
            else:
                why = why or ("reading found nothing wrong, which is not the "
                              "same as finding it right")

        # READING CAN ONLY EVER REJECT. A requirement that was not violated
        # has not been SATISFIED -- it has failed to be disproved, and those
        # are different facts. A krell whose modulation amounts happen to be
        # non-zero has proved nothing about whether its timing wanders: what
        # makes it wander is a sample-and-hold feeding an envelope, which is
        # topology, and no param value can stand in for it.
        #
        # This was live for one commit and a machine that had classified its
        # modules found it: the same patch that correctly deferred on an
        # unclassified machine came back silently settled once its modules
        # were read, because a value that was merely not-zero looked like an
        # answer. Silence bought with a precondition is exactly the defect
        # this whole layer exists to end.
        if not raised and "listening" in (spec.get("settled_by") or
                                          ["listening"]):
            deferrals.append(Deferral(
                flag, [],
                why or "settled by listening, not by reading"))
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
# the library's own faults
#
# Everything below asks a question about the LIBRARY rather than about a
# patch. The self-test proves an idiom can reject a patch; nothing proved an
# idiom was well-formed, and the new fields are exactly the kind that rot
# silently: a `hosts` list naming a slug somebody renamed, an axis whose
# neighbour does not name it back, a `listen_for` predicting a number the gate
# does not measure. Each of those reads as data and checks nothing.


_INF = float("inf")
_OPS_SET = (">=", "<=", ">", "<", "==")


def _interval(op: str, value) -> tuple:
    """One condition as the range of values that satisfy it."""
    v = float(value)
    return {">=": (v, True, _INF, False),
            ">": (v, False, _INF, False),
            "<=": (-_INF, False, v, True),
            "<": (-_INF, False, v, False),
            "==": (v, True, v, True)}[op]


def _disjoint(a: tuple, b: tuple) -> bool:
    """Whether two ranges share no value at all."""
    lo, lo_inc = max((a[0], a[1]), (b[0], b[1]))
    hi, hi_inc = min((a[2], not a[3]), (b[2], not b[3]))
    hi_inc = not hi_inc
    if lo > hi:
        return True
    return lo == hi and not (lo_inc and hi_inc)


def _hpp_members(path: str) -> set:
    """Every measurement member name the gate declares, read from its header."""
    import re                              # noqa: PLC0415 - only used here
    try:
        text = open(path).read()
    except OSError:
        return set()
    return set(re.findall(r"^\s*(?:int|double|[A-Za-z]+Measure)\s+(\w+)\s*[=;]",
                          text, re.M))


def library_problems(idioms: dict | None = None,
                     roles: dict | None = None) -> list[str]:
    """Everything structurally wrong with the library itself, in words.

    Not a check on any patch. A check on whether the records could check a
    patch -- which is the failure the rest of this file cannot see, because a
    field nothing reads passes every test by never being consulted.
    """
    idioms = idioms if idioms is not None else load_idioms()
    roles = roles if roles is not None else load_roles()
    measures = load_measurements()
    behaviours = load_behaviours()
    with open(os.path.join(HERE, "patch_behaviour_thresholds.json")) as f:
        flags = json.load(f).get("flags", {})
    bad: list[str] = []

    # 1. The measurement namespace is the gate's, not ours. Two directions:
    #    a field the thresholds already use that this file does not declare
    #    would let a calibration reference an undeclared number; a field this
    #    file declares that the gate does not measure is a prediction nothing
    #    can ever be compared against.
    members = _hpp_members(os.path.join(HERE, "patch_behaviour.hpp"))
    for field in sorted(measures):
        group, _, leaf = field.partition(".")
        if members and (group not in members or leaf not in members):
            bad.append(f"_measurements.json declares {field}, which "
                       f"patch_behaviour.hpp does not measure")
    for flag, spec in sorted(flags.items()):
        for key in ("needs", "all_of", "any_of"):
            for cond in spec.get(key) or []:
                if cond.get("field") not in measures:
                    bad.append(f"the {flag} threshold reads "
                               f"{cond.get('field')}, which "
                               f"_measurements.json does not declare")

    # 2. A word may belong to one idiom. Two records claiming the same name or
    #    the same phrase make resolution a coin toss settled by which file was
    #    read first, and the loser is unreachable without anybody noticing.
    claimed: dict[str, str] = {}
    for slug, idiom in sorted(idioms.items()):
        for phrase in list(idiom.get("names", [])) + list(idiom.get("implies", [])):
            key = phrase.lower().strip()
            if key in claimed and claimed[key] != slug:
                bad.append(f"{slug} and {claimed[key]} both claim the phrase "
                           f"{phrase!r}, so which one a request reaches "
                           f"depends on file order")
            claimed.setdefault(key, slug)

    # 3. Two idioms asserting the same structure are one idiom with two names:
    #    no patch can ever satisfy one and fail the other, so the distinction
    #    they claim is not a distinction this checker can make.
    #
    #    Unless they SAY so. A divider and a multiplier are one structure at
    #    two settings, and so are an attenuverter and an offset — the knob
    #    moves, the cables do not. Declaring both on one `axis` is the honest
    #    version of that: it keeps the two records, because the model needs
    #    both words, and states in data that what separates them is a control
    #    this checker cannot read. Silence is what this rule refuses. Four
    #    pairs in the library were silently identical when it was written.
    shapes: dict[tuple, str] = {}
    for slug, idiom in sorted(idioms.items()):
        shape = (tuple(sorted((r.get("from_module", "any"),
                               r.get("from_port", "any_out"),
                               r.get("to_module", "any"),
                               r.get("to_port", "any_in"),
                               bool(r.get("same_module")),
                               bool(r.get("different_module")))
                              for r in idiom.get("topology") or [])),
                 tuple(sorted((idiom.get("at_least") or {}).items())),
                 tuple(sorted((r.get("from_module", "any"),
                               r.get("to_module", "any"),
                               r.get("to_port", "any_in"))
                              for r in idiom.get("forbidden_topology") or [])))
        if shape in shapes:
            mine = (idiom.get("axis") or {}).get("family")
            theirs = (idioms[shapes[shape]].get("axis") or {}).get("family")
            if mine is None or mine != theirs:
                bad.append(
                    f"{slug} asserts exactly the structure {shapes[shape]} "
                    f"does, so no patch can tell them apart; put both on one "
                    f"axis if the difference is a setting rather than a cable")
        shapes[shape] = slug

    for slug, idiom in sorted(idioms.items()):
        kind = idiom.get("kind")
        if kind not in KINDS:
            bad.append(f"{slug} has kind {kind!r}, which is not one of {KINDS}")

        # 4. A fragment is a technique, not a patch. Two rules make it
        #    composable, and both are the kind a well-meaning edit breaks.
        if kind == "fragment":
            slot = idiom.get("slot")
            if slot not in SLOTS:
                bad.append(f"{slug} is a fragment with slot {slot!r}, which is "
                           f"not one of {SLOTS}")
            for req in idiom.get("topology") or []:
                if req.get("to_module") == "output":
                    bad.append(f"{slug} is a fragment requiring a cable to the "
                               f"output; the audible tail belongs to the host, "
                               f"and requiring it here makes the fragment "
                               f"unsatisfiable in every patch that puts "
                               f"anything after it")
            for host in idiom.get("hosts") or []:
                if host not in idioms:
                    bad.append(f"{slug} says it decorates {host}, which is not "
                               f"an idiom")
                elif idioms[host].get("kind") == "fragment":
                    bad.append(f"{slug} says it decorates {host}, which is "
                               f"itself a fragment")
        elif idiom.get("slot"):
            bad.append(f"{slug} is a whole patch and carries a slot; slots say "
                       f"where a fragment goes, and a whole patch goes nowhere")

        # A forbidden connection is the one requirement that holds by DEFAULT:
        # the minimal patch never contains the cable, so the ban is satisfied
        # without ever being consulted. Cutting a cable cannot break it either,
        # so it needs a control that PATCHES one.
        if idiom.get("forbidden_topology") and not any(
                m.get("do") == "wire"
                for m in idiom.get("common_mistakes") or []):
            bad.append(f"{slug} forbids a connection and never patches one, so "
                       f"the ban holds by default and proves nothing")

        # 5. An axis is a continuum written down: the members are separate
        #    idioms with separate structures, linked so neither the model nor
        #    a reader mistakes one position for the whole technique.
        axis = idiom.get("axis")
        if axis is not None:
            if not axis.get("family") or not axis.get("position"):
                bad.append(f"{slug}'s axis needs both a family and a position")
            # The point of an axis is that the positions differ by something
            # NOT in the cables. Naming that something is the whole admission;
            # an axis without it is a claim that two records differ, with no
            # statement of how.
            if len(str(axis.get("set_by") or "")) < 8:
                bad.append(f"{slug}'s axis does not say what control moves "
                           f"along it, which is the one thing an axis is for")
            for other, spec in sorted(idioms.items()):
                if other == slug or (spec.get("axis") or {}).get("family") \
                        != axis.get("family"):
                    continue
                if spec["axis"].get("position") == axis.get("position"):
                    bad.append(f"{slug} and {other} claim the same position "
                               f"{axis.get('position')!r} on the "
                               f"{axis.get('family')} axis")
            for direction, other in (axis.get("neighbours") or {}).items():
                if direction not in ("more", "less"):
                    bad.append(f"{slug}'s axis neighbour {direction!r} is "
                               f"neither 'more' nor 'less'")
                if other not in idioms:
                    bad.append(f"{slug}'s axis points {direction} at {other}, "
                               f"which is not an idiom")
                    continue
                mine = axis.get("family")
                theirs = (idioms[other].get("axis") or {}).get("family")
                if theirs != mine:
                    bad.append(f"{slug} puts {other} on the {mine} axis, but "
                               f"{other} is on {theirs!r}")

        # 6. Provenance. `source` has always been prose, and prose cannot be
        #    sorted: "Standard practice" and "Allen Strange's account of…" are
        #    the same type to a program, so a notice generated from that field
        #    alone would either flatten everything into an implied citation or
        #    need somebody to sort the strings by hand every time the library
        #    grows. `provenance` is the sortable half. Whether a `read` claim
        #    is TRUE is provenance_check.py's job, against the corpus; whether
        #    it is well formed is this one's.
        tier = idiom.get("provenance")
        if tier not in PROVENANCE:
            bad.append(f"{slug} has provenance {tier!r}, which is not one of "
                       f"{PROVENANCE}")
        elif tier == "read":
            anchor = idiom.get("anchor")
            if not isinstance(anchor, dict) or not anchor.get("doc") \
                    or not anchor.get("quote"):
                bad.append(f"{slug} says it was read and carries no anchor, so "
                           f"nothing can ever check it")
        elif idiom.get("anchor"):
            bad.append(f"{slug} is {tier} and still carries an anchor; an "
                       f"anchor is the evidence for a `read` claim and means "
                       f"nothing without one")
        if len(str(idiom.get("source") or "")) < 10:
            bad.append(f"{slug} does not say where it came from")

        # 7. The calibration. Free text is what a person hears; `expect` is
        #    the same claim in the numbers the gate reports, and it is the only
        #    ground truth the thresholds have. It has to be comparable to them,
        #    and it must not CONTRADICT the ones this idiom already asks for --
        #    an idiom flagged `sustained` whose calibration predicts four
        #    attacks a second is telling the gate and the listener two
        #    different things, and only one of them can be right.
        listen = idiom.get("listen_for")
        if listen is not None:
            if len(str(listen.get("sounds_like") or "")) < 20:
                bad.append(f"{slug}'s listen_for says nothing about what you "
                           f"would hear")
            for exp in listen.get("expect") or []:
                field, op = exp.get("field"), exp.get("op")
                if field not in measures:
                    bad.append(f"{slug} expects {field}, which the gate does "
                               f"not measure")
                    continue
                if op not in _OPS_SET or not isinstance(exp.get("value"),
                                                        (int, float)):
                    bad.append(f"{slug}'s expectation on {field} is not a "
                               f"comparison the gate could make")
                    continue
                mine = _interval(op, exp["value"])
                for flag, on in (idiom.get("behaviour") or {}).items():
                    if not on or flag not in flags:
                        continue
                    # `any_of` is a disjunction: disagreeing with one branch of
                    # it is not a contradiction, so only the conjunctive
                    # conditions are compared.
                    for key in ("needs", "all_of"):
                        for cond in flags[flag].get(key) or []:
                            if cond.get("field") != field:
                                continue
                            theirs = _interval(cond["op"], cond["value"])
                            if _disjoint(mine, theirs):
                                bad.append(
                                    f"{slug} expects {field} {op} "
                                    f"{exp['value']}, and its own {flag} flag "
                                    f"requires {field} {cond['op']} "
                                    f"{cond['value']} — the calibration and "
                                    f"the gate cannot both be satisfied")
    return bad



# --------------------------------------------------------------------------
# which fragments a patch already carries


def fragments_present(patch: dict, inv: dict, idioms: dict | None = None,
                      roles: dict | None = None) -> list[str]:
    """The fragments this patch satisfies, whatever it was asked for.

    Reported, never gating. A patch asked for a bassline that also happens to
    carry a portamento has done something the request did not name, and saying
    so is how a person finds out the library recognised more of their patch
    than the one idiom it was graded on.
    """
    idioms = idioms if idioms is not None else load_idioms()
    roles = roles if roles is not None else load_roles()
    return [slug for slug, frag in sorted(fragments(idioms).items())
            if not check(patch, inv, frag, roles)]


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

    if kind == "wire":
        # The negative control for an idiom defined by an ABSENCE. Cutting a
        # cable cannot break "nothing feeds the filter"; patching one can, and
        # a forbidden requirement nobody has watched reject anything is the
        # same empty sentence a positive one would be.
        #
        # Jack indices come from `_PORT_INDEX`, exactly as `synthesize` picks
        # them, because this only ever runs against the fixture rack that
        # function builds. It is not a general patch editor.
        target = mistake.get("requirement")
        req = next((r for r in mistake.get("_forbidden_topology", [])
                    if r.get("id") == target), None)
        if req is None:
            return None
        dst = next((m for m in out.get("modules", [])
                    if _module_matches(req.get("to_module", "any"),
                                       _entry(inv, m), roles)), None)
        src = next((m for m in out.get("modules", [])
                    if _module_matches(req.get("from_module", "any"),
                                       _entry(inv, m), roles)
                    and (m is not dst or req.get("same_module"))), None)
        if src is None or dst is None:
            return None
        out.setdefault("cables", []).append({
            "outputModuleId": src.get("id"),
            "outputId": _PORT_INDEX.get(req.get("from_port", "any_out"), 0),
            "inputModuleId": dst.get("id"),
            "inputId": _PORT_INDEX.get(req.get("to_port", "any_in"), 0),
        })
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
    forbidden = 0

    # A malformed record is a check nobody can rely on, and unlike a missing
    # negative control it does not announce itself: an axis pointing at a slug
    # that no longer exists, or a calibration naming a number the gate never
    # reports, reads as data and asserts nothing.
    for problem in library_problems(idioms, roles):
        print(f"  WRONG  {problem}")
        bad += 1

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
            mistake["_forbidden_topology"] = idiom.get("forbidden_topology", [])
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
            if mistake.get("do") == "wire":
                forbidden += 1
            if verbose:
                print(f"  ok     {slug}/{mistake.get('id')} → {got[0]}")

    print(f"\n{len(idioms)} idioms "
          f"({len(fragments(idioms))} fragments), {checked} negative controls "
          f"({forbidden} by patching a forbidden cable), {bad} wrong")
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
        # PWM and SYNC sit in the two slots nothing indexes, so the block stays
        # eight jacks long: the fan-in bump below adds 8 to reach "another jack
        # of the same kind", and a ninth entry would silently hand a mixer's
        # second channel a sync input.
        ins = ["IN", "V/OCT", "GATE", "CLK", "CV", "TRIG", "PWM", "SYNC"]
        in_roles = ["Audio", "Pitch", "Gate", "Clock", "Cv", "Trigger", "Cv",
                    "Trigger"]
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
    "pwm_in": 6, "sync_in": 7, "any_in": 0,
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

    slug = argv[2] if len(argv) > 2 else resolve_exact(
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

    # What ELSE the library recognises here, reported whatever the verdict.
    # A patch is graded against one idiom and is usually doing more than one
    # thing; naming the techniques it carries is how somebody finds out the
    # library saw more of their patch than the line it was judged on.
    also = [f for f in fragments_present(patch, inv, idioms) if f != slug]

    if not problems:
        print(f"  {slug}: holds")
        # A hold with a gap in it says so. Silence here would be the same
        # sentence for "every jack was verified" and "one of them could not
        # be looked at", which are not the same claim.
        for note in unchecked:
            print(f"    (not fully checked) {note}")
        if also:
            print(f"    (also carries) {', '.join(also)}")
        return 0
    print(f"  {slug}: not a {slug} patch yet —")
    for p in problems:
        print(f"    - {p}")
    if also:
        print(f"    (it does carry) {', '.join(also)}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
