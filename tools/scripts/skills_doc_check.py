#!/usr/bin/env python3
"""skills_doc_check.py — generate / verify the skill catalogs from the skills.

Two targets are generated from the same source, each `.agents/skills/<name>/SKILL.md`
frontmatter (the `name` + `description` fields), so neither can drift from the
skills themselves:

* `docs/reference/skills.md` — the public catalog.
* the `skills-digest` block in `CLAUDE.md` — the in-context index an agent reads
  on every turn. `.agents/skills` is not directly invocable in every session, so
  this table is the only index of the shipped skills some agents ever see; a
  hand-maintained one silently drifted to 58 rows against 66 skills, which made
  eight skills unreachable from context.

    python3 tools/scripts/skills_doc_check.py            # verify (exit 1 if stale)
    python3 tools/scripts/skills_doc_check.py --check     # same, explicit
    python3 tools/scripts/skills_doc_check.py --write      # regenerate both targets

The verify mode is wired into tools/check-docs.sh and a ctest (`skills-doc-sync`),
so a new or renamed skill that hasn't been reflected in the catalogs fails CI with
the exact `--write` command to fix it.

Two light quality gates keep the catalog useful: every skill must have a
non-empty `name` and a `description` of at least MIN_DESC_LEN characters (a skill
with no real description reads as a blank row and teaches nobody anything).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SKILLS_DIR = ROOT / ".agents" / "skills"
DOC = ROOT / "docs" / "reference" / "skills.md"

# The second target: an always-resident index spliced into CLAUDE.md between
# generated markers, using the same block-splice contract as the tools registry
# digest so the two behave identically.
DIGEST_DOC = ROOT / "CLAUDE.md"
DIGEST_ID = "skills-digest"
START = re.compile(rf"<!--\s*generated:start\s+id={DIGEST_ID}\s*-->")
END = re.compile(rf"<!--\s*generated:end\s+id={DIGEST_ID}\s*-->")

# A description shorter than this is treated as missing — a catalog row has to
# actually explain the skill.
MIN_DESC_LEN = 20

# stdlib-only frontmatter parsing (no PyYAML — this runs on CI runners that may
# not have it, same reason the SDK-consumer tools import yaml lazily).
_BLOCK_SCALAR = {"|", "|-", "|+", ">", ">-", ">+"}


def _frontmatter_lines(text: str) -> list[str]:
    """Return the lines between the opening and closing `---` fences, or []."""
    if not text.startswith("---"):
        return []
    lines = text.splitlines()
    for i in range(1, len(lines)):
        if lines[i].strip() == "---":
            return lines[1:i]
    return []


def _field(fm: list[str], key: str) -> str | None:
    """Read a scalar or block-scalar frontmatter field by key."""
    for i, line in enumerate(fm):
        m = re.match(rf"^{re.escape(key)}:\s*(.*)$", line)
        if not m:
            continue
        val = m.group(1).strip()
        if val in _BLOCK_SCALAR:
            block: list[str] = []
            base_indent: int | None = None
            for follow in fm[i + 1:]:
                if follow.strip() == "":
                    block.append("")
                    continue
                indent = len(follow) - len(follow.lstrip())
                if base_indent is None:
                    if indent == 0:      # not actually an indented block
                        break
                    base_indent = indent
                if indent < base_indent:
                    break
                block.append(follow[base_indent:])
            return "\n".join(block).strip()
        return val.strip().strip('"').strip("'")
    return None


def _summary(description: str) -> str:
    """One-line catalog summary: the first sentence of the description, collapsed."""
    one = " ".join(description.split())
    m = re.search(r"(.+?[.!?])(?:\s|$)", one)
    summary = (m.group(1) if m else one).strip()
    # Table cells can't contain a raw pipe.
    return summary.replace("|", "\\|")


def load_skills() -> tuple[list[dict], list[str]]:
    """Return ([{name, summary}], [problem, ...]) for every skill on disk."""
    skills: list[dict] = []
    problems: list[str] = []
    for skill_md in sorted(SKILLS_DIR.glob("*/SKILL.md")):
        dir_name = skill_md.parent.name
        fm = _frontmatter_lines(skill_md.read_text(encoding="utf-8", errors="ignore"))
        name = _field(fm, "name") or ""
        desc = _field(fm, "description") or ""
        if not name:
            problems.append(f"{dir_name}: SKILL.md has no `name:` in frontmatter")
            name = dir_name
        elif name != dir_name:
            problems.append(
                f"{dir_name}: frontmatter name '{name}' != directory '{dir_name}'")
        if len(desc) < MIN_DESC_LEN:
            problems.append(
                f"{dir_name}: description is missing or too short "
                f"(< {MIN_DESC_LEN} chars) — write a real one-line summary")
        skills.append({"name": name, "summary": _summary(desc) if desc else ""})
    skills.sort(key=lambda s: s["name"])
    return skills, problems


def render(skills: list[dict]) -> str:
    lines = [
        "# Skills",
        "",
        "Skills are Markdown playbooks that teach an AI coding agent how to work",
        "on a specific part of Pulp — the conventions, the gotchas, and the exact",
        "commands for a subsystem. They live in `.agents/skills/<name>/SKILL.md`,",
        "**ship with the Pulp Claude Code plugin** (`plugin.json` points at that",
        "directory), and are read by **both Claude Code and Codex** from the same",
        "source of truth — there is no separate per-agent copy.",
        "",
        "You rarely invoke a skill by name. Each one activates automatically when",
        "your request matches what it covers (its `description` lists the triggers),",
        "and many also have a matching `/slash-command`. The table below is the full",
        f"catalog of the {len(skills)} skills Pulp ships; open a skill's `SKILL.md`",
        "for its complete, authoritative guidance.",
        "",
        "| Skill | What it does |",
        "|-------|--------------|",
    ]
    for s in skills:
        lines.append(f"| `{s['name']}` | {s['summary']} |")
    lines += [
        "",
        "---",
        "",
        "This catalog is generated from each skill's `SKILL.md` frontmatter by",
        "`tools/scripts/skills_doc_check.py`. Do not edit it by hand — after adding",
        "or changing a skill, regenerate it with:",
        "",
        "```bash",
        "python3 tools/scripts/skills_doc_check.py --write",
        "```",
        "",
    ]
    return "\n".join(lines)


def render_digest(skills: list[dict]) -> str:
    """The in-context skills index spliced into CLAUDE.md.

    A real index, not a pointer: it names every shipped skill, because an agent
    that cannot see a skill's name has no way to reach it.
    """
    lines = [
        "| Skill | Purpose |",
        "|-------|---------|",
    ]
    for s in skills:
        lines.append(f"| `{s['name']}` | {s['summary']} |")
    lines += [
        "",
        f"This table of {len(skills)} skills is GENERATED from each",
        "`.agents/skills/<name>/SKILL.md` frontmatter by",
        "`tools/scripts/skills_doc_check.py --write`. Do not edit it by hand.",
    ]
    return "\n".join(lines)


def _splice(doc_text: str, block: str) -> str | None:
    """Replace the marked region. None if the markers are missing/malformed."""
    lines = doc_text.splitlines()
    start = end = None
    for i, l in enumerate(lines):
        if START.search(l):
            start = i
        elif END.search(l):
            end = i
            break
    if start is None or end is None or end < start:
        return None
    return "\n".join(lines[:start + 1] + block.splitlines() + lines[end:]) + "\n"


def digest_problems(skills: list[dict], write: bool) -> list[str]:
    if not DIGEST_DOC.exists():
        return [f"{DIGEST_DOC.name} is missing; cannot generate the "
                f"{DIGEST_ID} block"]
    text = DIGEST_DOC.read_text(encoding="utf-8")
    block = render_digest(skills)
    spliced = _splice(text, block)
    if spliced is None:
        return [f"CLAUDE.md is missing the generated markers "
                f"<!-- generated:start id={DIGEST_ID} --> / "
                f"<!-- generated:end id={DIGEST_ID} -->"]
    if write:
        if spliced != text:
            DIGEST_DOC.write_text(spliced, encoding="utf-8")
            print(f"wrote the {DIGEST_ID} block in CLAUDE.md ({len(skills)} skills)")
        else:
            print(f"{DIGEST_ID} block already up to date")
        return []
    if spliced != text:
        return [f"CLAUDE.md's {DIGEST_ID} block is out of sync with the skills. "
                "Regenerate it: python3 tools/scripts/skills_doc_check.py --write"]
    return []


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true",
                      help="regenerate docs/reference/skills.md and the "
                           "CLAUDE.md skills-digest block")
    mode.add_argument("--check", action="store_true",
                      help="fail if either target is stale (default)")
    args = ap.parse_args(argv)

    skills, problems = load_skills()
    if problems:
        print("ERROR: skill catalog quality problems:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    expected = render(skills)
    if args.write:
        DOC.write_text(expected, encoding="utf-8")
        print(f"wrote {DOC.relative_to(ROOT)} ({len(skills)} skills)")
        failures = digest_problems(skills, write=True)
        if failures:
            print("ERROR: CLAUDE.md skills digest:", file=sys.stderr)
            for f in failures:
                print(f"  - {f}", file=sys.stderr)
            return 1
        return 0

    # check mode (default) — BOTH targets, and each names itself when stale.
    failures: list[str] = []
    if not DOC.exists():
        failures.append(f"{DOC.relative_to(ROOT)} is missing. "
                        "Run: python3 tools/scripts/skills_doc_check.py --write")
    elif DOC.read_text(encoding="utf-8") != expected:
        failures.append(f"{DOC.relative_to(ROOT)} is out of sync with the skills. "
                        "Regenerate it: python3 tools/scripts/skills_doc_check.py --write")
    failures += digest_problems(skills, write=False)
    if failures:
        print("ERROR: skill catalogs are stale:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print(f"docs/reference/skills.md and the CLAUDE.md {DIGEST_ID} block "
          f"in sync ({len(skills)} skills)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
