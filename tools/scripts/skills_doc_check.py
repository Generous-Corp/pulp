#!/usr/bin/env python3
"""skills_doc_check.py — generate / verify docs/reference/skills.md from the skills.

`docs/reference/skills.md` is the single public catalog of every skill Pulp
ships. It is GENERATED from each `.agents/skills/<name>/SKILL.md` frontmatter
(the `name` + `description` fields), so the catalog can never drift from the
skills themselves.

    python3 tools/scripts/skills_doc_check.py            # verify (exit 1 if stale)
    python3 tools/scripts/skills_doc_check.py --check     # same, explicit
    python3 tools/scripts/skills_doc_check.py --write      # regenerate the doc

The verify mode is wired into tools/check-docs.sh and a ctest (`skills-doc-sync`),
so a new or renamed skill that hasn't been reflected in the catalog fails CI with
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


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true",
                      help="regenerate docs/reference/skills.md")
    mode.add_argument("--check", action="store_true",
                      help="fail if the doc is stale (default)")
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
        return 0

    # check mode (default)
    if not DOC.exists():
        print(f"ERROR: {DOC.relative_to(ROOT)} is missing. "
              "Run: python3 tools/scripts/skills_doc_check.py --write", file=sys.stderr)
        return 1
    actual = DOC.read_text(encoding="utf-8")
    if actual != expected:
        print(f"ERROR: {DOC.relative_to(ROOT)} is out of sync with the skills. "
              "Regenerate it: python3 tools/scripts/skills_doc_check.py --write",
              file=sys.stderr)
        return 1
    print(f"docs/reference/skills.md in sync ({len(skills)} skills)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
