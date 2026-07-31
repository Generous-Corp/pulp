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

from idiom_check import load_idioms, resolve

MARKER = "<!--PATCH_VOCABULARY-->"

# Below this, something has gone wrong in the library rather than in the prose:
# a family file failed to parse, or the directory did not ship. Checked at the
# point the model would be handed it, not at the point it was written.
MIN_IDIOMS = 50


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
    ]
    for family in sorted(families):
        out.append(f"## {family}")
        for slug, idiom in families[family]:
            out.append(f"### {slug}")
            out.append(f"    {idiom.get('is', '')}")
            reqs = idiom.get("topology") or []
            if reqs:
                out.append("    required:")
                for r in reqs:
                    out.append(f"      - {r.get('describe', r.get('id', ''))}")
            right = idiom.get("sounds_right_when")
            if right:
                out.append(f"    right when: {right}")
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
    slug = resolve(prompt, idioms)
    if not slug:
        return ""
    idiom = idioms[slug]
    lines = [
        f"This request is a **{slug}** patch.",
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
    if idiom.get("at_least"):
        need = ", ".join(f"{n} {role}" for role, n in idiom["at_least"].items())
        lines += ["", f"It needs at least: {need}."]
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
