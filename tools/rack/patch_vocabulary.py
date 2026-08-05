#!/usr/bin/env python3
"""The idiom library, rendered for the model that has to build these patches.

The module contract has `dsp_vocabulary.py` telling the model what a module may
be built from, and modules came out well. Patches had no equivalent, so the
model built from whatever it remembered reading -- which is how an evolving
drone came back a tone. This is the patch side of the same idea.

Two entry points. `render()` produces the text substituted into the contract.
`guard()` checks the ASSEMBLED contract, which is a different question: the
DSP side once shipped a vocabulary that rendered perfectly and reached the model
as an unsubstituted comment marker, and nothing noticed for three runs.

    python3 patch_vocabulary.py                 # print the rendered vocabulary
    python3 patch_vocabulary.py --check <file>  # guard an assembled contract
"""

from __future__ import annotations

import sys

from idiom_check import fragments_for, load_idioms, resolve_intent

MARKER = "<!--PATCH_VOCABULARY-->"

# Below this, something has gone wrong in the library rather than in the prose:
# a family file failed to parse, or the directory did not ship. Checked at the
# point the model would be handed it, not at the point it was written.
MIN_IDIOMS = 50


def needed_modules(idiom: dict) -> dict:
    """Every module the idiom's requirements actually need, with counts.

    `at_least` alone under-states it. A requirement names the roles at both
    ends of a cable, and those modules have to be in the patch for the cable
    to exist -- but only some of them appear in `at_least`, so the contract
    asked for a connection to a module it never asked for. Across the library
    that is 47 requirements.

    The self-test could not catch it: its fixture builds a patch from the
    TOPOLOGY, inventing whatever role a requirement names, so every idiom
    proved satisfiable using modules the model was never told to include. The
    instrument had more information than the thing it was testing.

    Derived rather than written down, so an edited requirement cannot leave a
    stale module list behind it.
    """
    need = dict(idiom.get("at_least") or {})
    for req in idiom.get("topology") or []:
        for key in ("from_module", "to_module"):
            role = req.get(key)
            if not role or role == "any":
                continue
            need.setdefault(role, 1)
    return need


def render(idioms: dict | None = None) -> str:
    """The library as the model sees it, grouped by family."""
    idioms = idioms if idioms is not None else load_idioms()
    families: dict[str, list] = {}
    for slug, idiom in sorted(idioms.items()):
        families.setdefault(idiom.get("family", "other"), []).append((slug, idiom))

    out: list[str] = [
        "These are the patch idioms this library knows. Each names a STRUCTURE:",
        "the connections that make a patch that kind of patch. If a request",
        "matches one, build that structure -- the checker asserts it, and a",
        "patch that merely sounds plausible will be rejected by name.",
        "",
        "Records in the `fragment` family are different in one way: they are",
        "techniques that live INSIDE a patch rather than patches. A fragment",
        "never reaches the output on its own -- the host patch does that -- so",
        "build one on top of a whole idiom, never instead of it.",
        "",
    ]
    for family in sorted(families):
        out.append(f"## {family}")
        for slug, idiom in families[family]:
            out.append(f"### {slug}")
            out.append(f"    {idiom.get('is', '')}")
            if idiom.get("kind") == "fragment":
                hosts = idiom.get("hosts")
                out.append(f"    fragment: fills the {idiom.get('slot')} slot of "
                           + (", ".join(hosts) if hosts else "any patch"))
            axis = idiom.get("axis")
            if axis:
                near = ", ".join(f"{d}: {s}" for d, s in
                                 sorted((axis.get("neighbours") or {}).items()))
                out.append(f"    one position ({axis.get('position')}) on the "
                           f"{axis.get('family')} continuum"
                           + (f" — {near}" if near else "")
                           + f"; what moves along it is {axis.get('set_by')}")
            need = needed_modules(idiom)
            if need:
                out.append("    modules: " +
                           ", ".join(f"{n} {role}" for role, n in
                                     sorted(need.items())))
            reqs = idiom.get("topology") or []
            if reqs:
                out.append("    required:")
                for r in reqs:
                    out.append(f"      - {r.get('describe', r.get('id', ''))}")
            right = idiom.get("sounds_right_when")
            if right:
                out.append(f"    right when: {right}")
            listen = idiom.get("listen_for") or {}
            if listen.get("sounds_like"):
                out.append(f"    listen for: {listen['sounds_like']}")
            if listen.get("confusable_with"):
                out.append(f"    not to be confused with: "
                           f"{listen['confusable_with']}")
            mistakes = idiom.get("common_mistakes") or []
            if mistakes:
                out.append("    do not:")
                for m in mistakes:
                    out.append(f"      - {m.get('message', m.get('id', ''))}")
            out.append("")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def for_prompt(prompt: str, idioms: dict | None = None) -> str:
    """The one idiom a prompt asks for, spelled out at length.

    The whole library is a lot of text for a request that plainly means one
    thing. When the prompt resolves, the contract leads with that idiom -- and
    the resolution happens HERE, deterministically, not by asking the model
    which idiom it thinks it is building. A model that picks its own target and
    is then graded against it learns to pick easy targets.
    """
    idioms = idioms if idioms is not None else load_idioms()
    from idiom_check import resolve_all                  # noqa: PLC0415
    read = resolve_all(prompt, idioms)
    intent = read.primary
    slug = intent.slug
    if not slug:
        return ""
    idiom = idioms[slug]
    # How firmly this is put depends on how it was reached. Telling the model
    # "this IS a krell patch" when we only matched a word pushes it to satisfy
    # our guess rather than the request -- and nothing downstream will fail it
    # for missing a target nobody asked for, because a non-gating intent
    # cannot reject. Say which one it is.
    lines = [
        f"This request is a **{slug}** patch." if intent.gating else
        f"No idiom matched this request. The nearest is **{slug}**; treat it "
        f"as a guide, not a target -- the request comes first.",
        "",
        idiom.get("is", ""),
        "",
        "It is not that patch unless ALL of these hold:",
    ]
    for r in idiom.get("topology") or []:
        lines.append(f"  - {r.get('describe', '')}")
    if idiom.get("sounds_right_when"):
        lines += ["", f"It sounds right when: {idiom['sounds_right_when']}"]
    mistakes = idiom.get("common_mistakes") or []
    if mistakes:
        lines += ["", "These are the ways it usually goes wrong:"]
        for m in mistakes:
            lines.append(f"  - {m.get('message', '')}")
    need = needed_modules(idiom)
    if need:
        listed = ", ".join(f"{n} {role}" for role, n in sorted(need.items()))
        lines += ["", f"It needs at least: {listed}."]

    # OTHER ROUTES TO THE SAME REQUEST, and they have to be offered or they may
    # as well not exist. A request for a melodic patch matches four idioms; the
    # model used to be told about one, and when the rack it had made that one
    # awkward it kept failing at it rather than taking a route that was right
    # there. Naming them costs a paragraph and turns one answer into four.
    if read.alternatives:
        lines += ["", "This request has more than one right answer. Any ONE of "
                      "these satisfies it — build whichever suits the modules "
                      "you have, and you will be checked against the one you "
                      "actually built, not against the first:"]
        for other in read.alternatives:
            lines.append(f"  - **{other}**: {idioms[other].get('is', '')}")
            for r in idioms[other].get("topology") or []:
                lines.append(f"      - {r.get('describe', '')}")

    # Things the request also touches that ADD to the patch rather than
    # replacing it. Offered, never required: a bassline that arrives without
    # the reverb somebody mentioned is still a bassline.
    extra = list(read.also) + list(read.fragments)
    if extra:
        lines += ["", "The request also mentions these, which go ON TOP of the "
                      "patch above rather than instead of it. Add them if they "
                      "belong; nothing rejects a patch for leaving one out:"]
        for other in extra:
            lines.append(f"  - {other}: {idioms[other].get('is', '')}")

    # An idiom on a continuum is one position on it, and the neighbouring
    # position is usually a KNOB away. Saying so is the difference between
    # the model building a vibrato and the model building an FM voice when
    # somebody asked for one and got the other.
    axis = idiom.get("axis")
    if axis:
        lines += ["", f"This is one position ({axis['position']}) on the "
                      f"{axis['family']} continuum. What moves along it is "
                      f"{axis['set_by']}."]
        for direction, other in sorted((axis.get("neighbours") or {}).items()):
            lines.append(f"  - {direction}: {other} — "
                         f"{idioms[other].get('is', '')}")

    if idiom.get("kind") == "fragment":
        hosts = idiom.get("hosts")
        lines += ["", f"This is a FRAGMENT: it fills the {idiom['slot']} slot "
                      f"of " + (", ".join(hosts) if hosts else "a patch") +
                      ". It does not reach the output by itself; build it on "
                      "top of a whole patch."]
    else:
        # Two tiers, because listing two dozen techniques in full is the same
        # as listing none: the model skims it. A fragment that NAMES this host
        # was written with it in mind and gets its sentence; the rest are one
        # line of slugs, enough to be looked up in the library above if the
        # request actually asks for one.
        named = [f for f in fragments_for(slug, idioms) if idioms[f].get("hosts")]
        general = [f for f in fragments_for(slug, idioms)
                   if not idioms[f].get("hosts")]
        if named:
            lines += ["", "Fragments written for this patch, if the request "
                          "asks for one — do not add them uninvited:"]
            for f in named:
                lines.append(f"  - {f} ({idioms[f]['slot']}): "
                             f"{idioms[f].get('is', '')}")
        if general:
            by_slot: dict[str, list[str]] = {}
            for f in general:
                by_slot.setdefault(idioms[f]["slot"], []).append(f)
            lines += ["", "Other fragments that would compose with it, by slot "
                          "(again, only if asked for):"]
            for slot, names in sorted(by_slot.items()):
                lines.append(f"  {slot}: {', '.join(names)}")

    # WHAT IS KNOWN ABOUT THE TECHNIQUE, as opposed to about the wiring. Read
    # lazily and skipped silently when absent: the generator worked before this
    # existed and has to keep working when it does not.
    try:
        import knowledge                                 # noqa: PLC0415
        known = knowledge.render_for(slug, None, idioms)
    except Exception:                                    # noqa: BLE001
        known = ""
    if known:
        lines += ["", known.rstrip()]

    listen = idiom.get("listen_for") or {}
    if listen.get("sounds_like"):
        lines += ["", f"It should sound like: {listen['sounds_like']}"]
    if listen.get("confusable_with"):
        lines.append(f"It is NOT: {listen['confusable_with']}")
    return "\n".join(lines) + "\n"


def guard(contract: str) -> list[str]:
    """Everything wrong with an ASSEMBLED contract, about to go to the model.

    Checking the renderer proves the renderer works. This checks the thing the
    model actually receives, which is where the DSP side failed: the vocabulary
    was fine and the substitution silently did not happen, so the model was
    handed a comment marker and invented its own DSP for three runs before
    anyone looked at the prompt.
    """
    problems = []
    if MARKER in contract:
        problems.append(
            f"the contract still contains {MARKER} -- the vocabulary was never "
            "substituted, and the model will be handed a comment")
    if "<!--" in contract:
        problems.append("the contract contains an unsubstituted <!-- marker")
    slugs = [s for s in load_idioms() if f"### {s}" in contract or f"**{s}**" in contract]
    if len(slugs) < MIN_IDIOMS and "**" not in contract:
        problems.append(
            f"only {len(slugs)} idioms reached the contract, expected at least "
            f"{MIN_IDIOMS} -- the library did not ship or failed to parse")
    return problems


def main(argv: list[str]) -> int:
    if "--check" in argv:
        path = argv[argv.index("--check") + 1]
        problems = guard(open(path).read())
        for p in problems:
            print(f"  {p}")
        print("  contract is sound" if not problems else "")
        return 1 if problems else 0
    if "--for" in argv:
        print(for_prompt(argv[argv.index("--for") + 1]))
        return 0
    sys.stdout.write(render())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
