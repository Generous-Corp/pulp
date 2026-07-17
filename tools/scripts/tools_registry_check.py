#!/usr/bin/env python3
"""tools_registry_check.py — validate docs/status/tools.yaml and generate its digest.

`docs/status/tools.yaml` is the registry of tools an agent should REACH FOR
instead of hand-rolling one. This script is what stops that registry from
becoming a lie, and what delivers it to the place an agent will actually read.

Three jobs:

1. **Validate** every entry — the `path` exists, the `invocation` resolves (its
   interpreter is known and its script is really there; `pulp ...` invocations
   are cross-checked against docs/status/cli-commands.yaml), the owning skill
   exists, `docs:` resolves, and `planned` entries carry a pointer and NO
   invocation. A registry that hands an agent a command for something absent is
   worse than no registry.

2. **Coverage sweep** — every agent-invocable entry point under the swept dirs
   must appear in `tools:` or in `exclude:` with a reason. This is the same
   invariant shape as skill_sync_check.py's "every skill dir MUST have an entry",
   and it is the anti-rot gate: without it, the next tool someone adds is
   invisible again within a month, which is exactly how the incident happened.
   The sweep also rejects STALE exclusions, so the exclude list can't quietly
   accumulate entries for files that no longer exist.

3. **Generate the digest** — the `tools-digest` block in CLAUDE.md, which is
   always in context for both Claude Code (CLAUDE.md) and Codex (via AGENTS.md).
   Generated, never hand-written; `--check` fails on drift.

    python3 tools/scripts/tools_registry_check.py           # verify (exit 1 if bad)
    python3 tools/scripts/tools_registry_check.py --check    # same, explicit
    python3 tools/scripts/tools_registry_check.py --write    # regenerate the digest

Wired into tools/check-docs.sh and a ctest (`tools-registry-check`).
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REGISTRY = ROOT / "docs" / "status" / "tools.yaml"
CLI_MANIFEST = ROOT / "docs" / "status" / "cli-commands.yaml"
DIGEST_DOC = ROOT / "CLAUDE.md"
SKILLS_DIR = ROOT / ".agents" / "skills"
DOCS_DIR = ROOT / "docs"

DIGEST_ID = "tools-digest"
START = re.compile(rf"<!--\s*generated:start\s+id={DIGEST_ID}\s*-->")
END = re.compile(rf"<!--\s*generated:end\s+id={DIGEST_ID}\s*-->")

AVAILABILITY = {"always", "optional-install", "planned"}

# Interpreters we know how to resolve an invocation through.
INTERPRETERS = {"python3", "python", "node", "bash", "sh"}

# Category display order in the digest. Anything not listed sorts last,
# alphabetically — a new category shows up rather than vanishing.
CATEGORY_ORDER = [
    "visual-compare",
    "design-import",
    "import-roundtrip",
    "harness",
    "audio",
]

CATEGORY_BLURB = {
    "visual-compare": "compare a render against its source / a baseline",
    "design-import": "get a design into Pulp",
    "import-roundtrip": "validate an import lane end to end",
    "harness": "coverage + deterministic visual harness",
    "audio": "prove what the audio actually did",
}


def _load_yaml(path: Path):
    # Imported lazily, same as the SDK-consumer tools: PyYAML is not guaranteed
    # on a PEP-668 runner, and a clear message beats an ImportError traceback.
    try:
        import yaml
    except ModuleNotFoundError:
        raise SystemExit(
            "PyYAML is required to parse the tool registry.\n"
            "  pip install pyyaml")
    return yaml.safe_load(path.read_text(encoding="utf-8"))


# ── Entry-point detection ───────────────────────────────────────────────
#
# A file is a "tool" if an agent can RUN it. A library module that only gets
# imported is not a tool — sweeping those in would drown the registry in noise
# and make it unreadable, which is the failure mode we're fixing.

def _is_test_path(rel: str) -> bool:
    name = Path(rel).name
    parts = Path(rel).parts
    return (
        name.startswith("test_")
        or ".test." in name
        or "tests" in parts
        or name == "conftest.py"
    )


def is_entry_point(path: Path) -> bool:
    """True if `path` is an agent-invocable entry point."""
    suffix = path.suffix
    if suffix not in (".py", ".sh", ".mjs"):
        return False
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return False
    if suffix == ".sh":
        return True
    if suffix == ".py":
        return bool(re.search(r'__name__\s*==\s*[\'"]__main__[\'"]', text))
    # .mjs — a shebang, or a CLI main guard.
    return text.startswith("#!") or (
        "import.meta.url" in text and "process.argv" in text)


def _tracked_files() -> list[str] | None:
    """Repo-tracked paths, or None if this isn't a usable git checkout."""
    try:
        r = subprocess.run(["git", "-C", str(ROOT), "ls-files"],
                           capture_output=True, text=True, check=False)
    except OSError:
        return None
    if r.returncode != 0:
        return None
    return r.stdout.split()


def sweep_entry_points(registry: dict) -> list[str]:
    """Every non-test entry point under the swept dirs + allowlisted paths.

    Swept over GIT-TRACKED files, not the working tree. The registry describes
    the committed repo: an untracked scratch script isn't a tool yet, and
    sweeping the working tree would let one agent's WIP fail everyone else's
    push in a shared worktree. The moment such a file IS committed, the sweep
    fires — on the person who committed it, which is where it belongs.
    """
    sweep = registry.get("sweep") or {}
    dirs = [d.rstrip("/") for d in (sweep.get("include_dirs") or [])]
    allow = list(sweep.get("include_paths") or [])

    tracked = _tracked_files()
    if tracked is None:  # not a git checkout (e.g. a source tarball) — walk instead
        tracked = []
        for d in dirs:
            base = ROOT / d
            if base.is_dir():
                tracked += [p.relative_to(ROOT).as_posix()
                            for p in base.rglob("*") if p.is_file()]
        tracked += [f for f in allow if (ROOT / f).is_file()]

    found: set[str] = set()
    for rel in tracked:
        in_scope = rel in allow or any(
            rel == d or rel.startswith(d + "/") for d in dirs)
        if not in_scope:
            continue
        if _is_test_path(rel) or "__pycache__" in rel:
            continue
        p = ROOT / rel
        if p.is_file() and is_entry_point(p):
            found.add(rel)
    return sorted(found)


# ── Invocation resolution ───────────────────────────────────────────────

def _cli_command_names() -> dict[str, list[str]]:
    """{command: [subcommand names]} from cli-commands.yaml."""
    data = _load_yaml(CLI_MANIFEST) or {}
    out: dict[str, list[str]] = {}
    for c in data.get("commands") or []:
        name = c.get("name")
        if not name:
            continue
        out[name] = [s.get("name", "") for s in (c.get("subcommands") or [])]
    return out


def _check_pulp_invocation(tokens: list[str], cli: dict[str, list[str]]) -> str | None:
    """Validate a `pulp <cmd> [sub...]` invocation against cli-commands.yaml."""
    if len(tokens) < 2:
        return "invocation is just `pulp` with no command"
    cmd = tokens[1]
    if cmd not in cli:
        return f"`pulp {cmd}` is not a command in docs/status/cli-commands.yaml"
    subs = [s for s in cli[cmd] if s]
    if not subs:
        return None
    # Collect the non-flag, non-placeholder tokens after the command and try to
    # match progressively longer joins against the declared subcommand names
    # (which are multi-word, e.g. "validate summarize").
    rest: list[str] = []
    for t in tokens[2:]:
        if t.startswith("-") or t.startswith("<"):
            break
        rest.append(t)
    if not rest:
        return None  # e.g. `pulp audio --help`
    for n in range(len(rest), 0, -1):
        if " ".join(rest[:n]) in subs:
            return None
    return (f"`pulp {cmd} {' '.join(rest)}` matches no subcommand of "
            f"`{cmd}` in cli-commands.yaml")


def check_invocation(inv: str, cli: dict[str, list[str]]) -> str | None:
    """Return a problem string, or None if the invocation resolves."""
    tokens = inv.split()
    if not tokens:
        return "invocation is empty"
    head = tokens[0]
    if head == "pulp":
        return _check_pulp_invocation(tokens, cli)
    if head not in INTERPRETERS:
        return (f"invocation starts with unknown interpreter '{head}' "
                f"(expected one of: pulp, {', '.join(sorted(INTERPRETERS))})")
    # `python3 -m pkg.mod` — a module, not a path. The entry's `path` field is
    # what anchors it to the tree, and that is checked separately.
    if "-m" in tokens[1:3]:
        return None
    for t in tokens[1:]:
        if t.startswith("-"):
            continue
        # The first path-ish token is the script.
        if "/" in t or t.endswith((".py", ".sh", ".mjs")):
            if not (ROOT / t).exists():
                return f"invocation references a missing script: {t}"
            return None
        break
    return f"could not find a script path in invocation: {inv!r}"


# ── Validation ──────────────────────────────────────────────────────────

def validate(registry: dict) -> list[str]:
    problems: list[str] = []
    tools = registry.get("tools") or []
    if not tools:
        return ["registry has no `tools:` entries"]

    cli = _cli_command_names()
    seen: set[str] = set()

    for t in tools:
        name = t.get("name")
        if not name:
            problems.append(f"entry with no `name`: {t!r}")
            continue
        if name in seen:
            problems.append(f"{name}: duplicate name")
        seen.add(name)

        avail = t.get("availability")
        if avail not in AVAILABILITY:
            problems.append(
                f"{name}: availability '{avail}' not in {sorted(AVAILABILITY)}")
            continue

        if not (t.get("use_when") or "").strip():
            problems.append(f"{name}: `use_when` is required — it is the "
                            f"matching surface an agent scans")
        if not t.get("category"):
            problems.append(f"{name}: `category` is required")

        # Owning skill must exist.
        skill = t.get("skill")
        if skill and not (SKILLS_DIR / skill / "SKILL.md").is_file():
            problems.append(f"{name}: skill '{skill}' has no "
                            f".agents/skills/{skill}/SKILL.md")

        # docs: must resolve (anchor stripped), same rule as check-docs.sh.
        doc = t.get("docs")
        if doc and not (DOCS_DIR / doc.split("#", 1)[0]).is_file():
            problems.append(f"{name}: docs references missing docs/{doc}")

        if avail == "planned":
            # The whole point: never hand an agent a command for something that
            # isn't in the tree.
            if t.get("invocation"):
                problems.append(f"{name}: availability=planned must NOT carry an "
                                f"`invocation` — planned tools get a pointer only")
            if not (t.get("pointer") or "").strip():
                problems.append(f"{name}: availability=planned requires a `pointer`")
            if t.get("path"):
                problems.append(f"{name}: availability=planned must not claim a "
                                f"`path` (it isn't in the tree)")
            continue

        # Shipped entries: path must exist.
        path = t.get("path")
        if not path:
            problems.append(f"{name}: `path` is required")
        elif not (ROOT / path).exists():
            problems.append(f"{name}: path does not exist: {path}")

        inv = t.get("invocation")
        if not inv:
            problems.append(f"{name}: `invocation` is required for "
                            f"availability={avail}")
        else:
            problem = check_invocation(inv, cli)
            if problem:
                problems.append(f"{name}: {problem}")

        if avail == "optional-install" and not (t.get("install") or "").strip():
            problems.append(f"{name}: availability=optional-install requires an "
                            f"`install` field saying how to get it")

    return problems


def check_coverage(registry: dict) -> list[str]:
    """Every swept entry point must be registered or explicitly excluded."""
    problems: list[str] = []
    found = sweep_entry_points(registry)

    # A registered `path` covers an entry point at that path OR under it (a
    # package like tools/audio/quality-lab covers quality_lab/cli.py).
    registered = [t["path"] for t in (registry.get("tools") or []) if t.get("path")]
    excludes = registry.get("exclude") or []
    excluded = {e.get("path") for e in excludes if e.get("path")}

    def covered(rel: str) -> bool:
        if rel in excluded:
            return True
        for p in registered:
            if rel == p or rel.startswith(p.rstrip("/") + "/"):
                return True
        return False

    for rel in found:
        if not covered(rel):
            problems.append(
                f"UNREGISTERED TOOL: {rel} is an agent-invocable entry point "
                f"under a swept directory but is absent from docs/status/tools.yaml. "
                f"Add it to `tools:`, or to `exclude:` with a reason.")

    # Stale exclusions rot too.
    found_set = set(found)
    for e in excludes:
        p = e.get("path")
        if not p:
            problems.append(f"exclude entry with no `path`: {e!r}")
            continue
        if not (e.get("reason") or "").strip():
            problems.append(f"exclude {p}: needs a `reason`")
        if not (ROOT / p).exists():
            problems.append(f"exclude {p}: path no longer exists — drop it")
        elif p not in found_set:
            problems.append(f"exclude {p}: is not a detected entry point, so "
                            f"excluding it is a no-op — drop it")

    return problems


# ── Digest ──────────────────────────────────────────────────────────────

def digest_handle(inv: str) -> str:
    """The typeable handle for a tool, derived from its canonical invocation.

    The digest has to name the thing you RUN. A `path` is wrong for the CLI
    tools — `tools/cli/cmd_audio.cpp` is a source file no agent can invoke —
    so take the invocation up to its first placeholder/flag and drop a bare
    interpreter, leaving e.g. `pulp audio validate summarize` or
    `tools/import-design/montage.py`.
    """
    tokens = inv.split()
    if not tokens:
        return inv
    head: list[str] = []
    rest = tokens
    # `python -m pkg.mod` — the module IS the name; keep the whole prefix.
    if len(tokens) > 2 and tokens[1] == "-m":
        head, rest = tokens[:3], tokens[3:]
    elif tokens[0] in INTERPRETERS:
        rest = tokens[1:]  # drop the bare interpreter
    for t in rest:
        if t.startswith(("-", "<", "[")):
            break
        head.append(t)
    return " ".join(head) or inv


def render_digest(registry: dict) -> str:
    tools = [t for t in (registry.get("tools") or [])
             if t.get("availability") != "planned"]

    by_cat: dict[str, list[dict]] = {}
    for t in tools:
        by_cat.setdefault(t.get("category", "other"), []).append(t)

    def cat_key(c: str) -> tuple[int, str]:
        return (CATEGORY_ORDER.index(c), "") if c in CATEGORY_ORDER else (
            len(CATEGORY_ORDER), c)

    lines = [
        "### Registered tools — check before hand-rolling",
        "",
        "**These already exist. Do not write a script that does one of these jobs.**",
        "Full detail (inputs, outputs, owning skill): `docs/status/tools.yaml`.",
        "Reach for the tool whose *use when* matches your need; open its `skill`",
        "for the real guidance. If nothing here fits, say so — then hand-roll.",
        "",
    ]
    for cat in sorted(by_cat, key=cat_key):
        blurb = CATEGORY_BLURB.get(cat)
        lines.append(f"**{cat}**" + (f" — {blurb}" if blurb else ""))
        for t in sorted(by_cat[cat], key=lambda x: x["name"]):
            mark = " *(needs install)*" if t["availability"] == "optional-install" else ""
            lines.append(f"- {t['use_when']}{mark} → `{digest_handle(t['invocation'])}`")
        lines.append("")
    lines += [
        "This digest is GENERATED from `docs/status/tools.yaml` by",
        "`tools/scripts/tools_registry_check.py --write`. Do not edit it by hand.",
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


def digest_problems(registry: dict, write: bool) -> list[str]:
    text = DIGEST_DOC.read_text(encoding="utf-8")
    block = render_digest(registry)
    spliced = _splice(text, block)
    if spliced is None:
        return [f"CLAUDE.md is missing the generated markers "
                f"<!-- generated:start id={DIGEST_ID} --> / "
                f"<!-- generated:end id={DIGEST_ID} -->"]
    if write:
        if spliced != text:
            DIGEST_DOC.write_text(spliced, encoding="utf-8")
            print(f"wrote the {DIGEST_ID} block in CLAUDE.md")
        else:
            print(f"{DIGEST_ID} block already up to date")
        return []
    if spliced != text:
        return ["CLAUDE.md's tools digest is out of sync with "
                "docs/status/tools.yaml. Regenerate it: "
                "python3 tools/scripts/tools_registry_check.py --write"]
    return []


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true",
                      help="regenerate the tools digest in CLAUDE.md")
    mode.add_argument("--check", action="store_true",
                      help="fail on any registry/coverage/digest problem (default)")
    args = ap.parse_args(argv)

    if not REGISTRY.is_file():
        print(f"ERROR: {REGISTRY.relative_to(ROOT)} is missing", file=sys.stderr)
        return 1
    registry = _load_yaml(REGISTRY) or {}

    problems = validate(registry) + check_coverage(registry)
    # Only regenerate from a registry that is actually valid — writing a digest
    # from a broken registry would publish the breakage into every agent's context.
    if problems:
        print("ERROR: tools registry problems:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    problems = digest_problems(registry, write=args.write)
    if problems:
        print("ERROR: tools digest problems:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    if not args.write:
        n = len(registry.get("tools") or [])
        print(f"tools registry OK ({n} tools; coverage sweep clean; digest in sync)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
