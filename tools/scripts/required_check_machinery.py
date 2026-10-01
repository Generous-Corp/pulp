#!/usr/bin/env python3
"""Name the files in a pull request that can decide a required check.

Every required context on ``main`` is posted by a GitHub Actions workflow, and
a pull request's own merge group runs the workflow YAML that pull request
carries. A change to one of those workflows, or to the code that decides
whether a merge group may reuse a pull-request run, therefore decides its own
required checks. That is accepted repository policy, not something CI can
refuse, so this tool only makes it visible: the advisory
`Required-check machinery (advisory)` check run, posted from the protected base
by ``.github/workflows/required-check-machinery.yml``, carries its answer. A
flagged pull request gets a ``neutral`` conclusion, so it renders differently
from an unflagged ``success`` without reading as a failure to chase.

The answer is computed from the protected base's copy of this file and of the
files it reads, never from the pull request's.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

RULESET = ".github/rulesets/main-protection.json"

# The workflows designed to post each required context. A context missing here
# makes every workflow file count as machinery, so a new required check cannot
# silently fall outside the report.
CONTEXT_PRODUCERS = {
    "macos": (".github/workflows/build.yml", ".github/workflows/build-macos.yml"),
    "Enforce version & skill sync": (".github/workflows/version-skill-check.yml",),
    "Build + prove + (owner-gated) deploy": (".github/workflows/wclap-cloudflare.yml",),
    "Vellum freeze": (".github/workflows/vellum-freeze-check.yml",),
    "Vellum trusted freeze": (".github/workflows/vellum-trusted-gate.yml",),
    "drift-fast": (".github/workflows/drift-fast.yml",),
}

# Code a merge group runs to decide whether the macOS suite may be skipped by
# reusing a pull-request run (the receipt policy paths and the test selection
# a receipt must match).
REUSE_MACHINERY = (
    ".github/workflows/build.yml",
    ".agents/contract.toml",
    "tools/scripts/classify_changes.py",
    "tools/scripts/protected_merge_receipt.py",
    "tools/ci/ctest_gate_args.py",
)

# Where required checks and merge rules are declared, and this report itself.
RULES = (
    ".github/rulesets/",
    ".github/CODEOWNERS",
    ".github/workflows/required-check-machinery.yml",
    "tools/scripts/required_check_machinery.py",
)

LOCAL_USES = re.compile(r"^\s*(?:-\s+)?uses:\s*\./([^\s@#]+)", re.MULTILINE)


def required_contexts(repo: Path) -> list[str]:
    ruleset = json.loads((repo / RULESET).read_text(encoding="utf-8"))
    contexts: list[str] = []
    for rule in ruleset.get("rules", []):
        if rule.get("type") == "required_status_checks":
            for check in rule["parameters"]["required_status_checks"]:
                contexts.append(check["context"])
    return contexts


def producer_files(repo: Path, contexts: list[str]) -> tuple[set[str], list[str]]:
    """Producer workflows plus the local actions and reusable workflows they call."""
    unmapped = [context for context in contexts if context not in CONTEXT_PRODUCERS]
    pending = [path for context in contexts for path in CONTEXT_PRODUCERS.get(context, ())]
    found: set[str] = set()
    while pending:
        path = pending.pop()
        if path in found:
            continue
        found.add(path)
        target = repo / path
        if target.is_dir():
            files = [p for p in target.iterdir() if p.name in ("action.yml", "action.yaml")]
        elif target.is_file():
            files = [target]
        else:
            files = []
        for file in files:
            for used in LOCAL_USES.findall(file.read_text(encoding="utf-8")):
                pending.append(used.rstrip("/"))
    return found, unmapped


def classify(files: list[str], repo: Path,
             contexts: list[str] | None = None) -> dict[str, list[str]]:
    """Map each flagged file to why it is flagged; unflagged files are omitted.

    ``contexts`` is the live required-context list; ``None`` reads the
    committed ruleset. An empty list means the required checks could not be
    read, and every workflow file counts.
    """
    if contexts is None:
        contexts = required_contexts(repo)
    producers, unmapped = producer_files(repo, contexts)
    if not contexts:
        widen = "workflow (required checks unreadable)"
    elif unmapped:
        widen = "workflow (unmapped required check: " + ", ".join(unmapped) + ")"
    else:
        widen = ""
    flagged: dict[str, list[str]] = {}

    def within(path: str, entry: str) -> bool:
        return path == entry or path.startswith(entry.rstrip("/") + "/")

    for path in files:
        reasons = []
        if any(within(path, entry) for entry in REUSE_MACHINERY):
            reasons.append("receipt reuse")
        if any(within(path, entry) for entry in producers):
            reasons.append("required-check workflow")
        elif widen and path.startswith((".github/workflows/", ".github/actions/")):
            reasons.append(widen)
        if any(within(path, entry) for entry in RULES):
            reasons.append("merge rules")
        if reasons:
            flagged[path] = reasons
    return flagged


def read_contexts(path: Path | None) -> list[str]:
    """The live context list a workflow fetched; unreadable or malformed is empty."""
    if path is None:
        return []
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        return []
    return value


def _short_name(path: str) -> str:
    parts = path.split("/")
    if parts[-1] in ("action.yml", "action.yaml") and len(parts) > 1:
        return parts[-2]
    return parts[-1]


def describe(flagged: dict[str, list[str]], limit: int = 140) -> str:
    """The status description: GitHub truncates it at 140 characters."""
    if not flagged:
        return "touches no required-check machinery"
    prefix = "touches required-check machinery: "
    names = [_short_name(path) for path in sorted(flagged)]
    shown: list[str] = []
    for index, name in enumerate(names):
        rest = len(names) - index - 1
        suffix = f" (+{rest} more)" if rest else ""
        candidate = prefix + ", ".join(shown + [name]) + suffix
        if len(candidate) > limit:
            break
        shown.append(name)
    rest = len(names) - len(shown)
    text = prefix + ", ".join(shown) + (f" (+{rest} more)" if rest else "")
    return text[:limit]


def changed_files(repo: Path, base: str, head: str) -> list[str]:
    merge_base = subprocess.run(
        ["git", "-C", str(repo), "merge-base", base, head],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    out = subprocess.run(
        ["git", "-C", str(repo), "diff", "--name-only", "--no-renames", merge_base, head],
        check=True, capture_output=True, text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=Path("."))
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--diff", nargs=2, metavar=("BASE", "HEAD"))
    source.add_argument("--files", nargs="*")
    live = parser.add_mutually_exclusive_group()
    live.add_argument("--contexts-file", type=Path,
                      help="JSON list of the live required contexts; unreadable or empty widens")
    live.add_argument("--contexts-unavailable", action="store_true",
                      help="the live required contexts could not be read; widen")
    args = parser.parse_args(argv)
    files = args.files if args.files is not None else changed_files(args.repo, *args.diff)
    if args.contexts_unavailable:
        contexts: list[str] | None = []
    elif args.contexts_file is not None:
        contexts = read_contexts(args.contexts_file)
    else:
        contexts = None
    flagged = classify(files, args.repo, contexts)
    result = {
        "flagged": flagged,
        "description": describe(flagged),
        "conclusion": "neutral" if flagged else "success",
        "required_checks_read": contexts is None or bool(contexts),
    }
    json.dump(result, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
