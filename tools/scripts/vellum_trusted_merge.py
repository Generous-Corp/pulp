#!/usr/bin/env python3
"""Build an exact base+head merge commit without trusting GitHub's merge ref."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


SHA_RE = re.compile(r"[0-9a-f]{40}")


class TrustedMergeError(RuntimeError):
    """The requested merge could not be proven or constructed."""


def _git(
    repo: Path,
    *args: str,
    input_text: str | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["GIT_NO_REPLACE_OBJECTS"] = "1"
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        check=False,
    )
    if check and result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise TrustedMergeError(detail or f"git {' '.join(args)} failed")
    return result


def _exact_commit(repo: Path, value: str, label: str) -> str:
    if not SHA_RE.fullmatch(value):
        raise TrustedMergeError(f"{label} is not a full lowercase commit SHA")
    resolved = _git(repo, "rev-parse", "--verify", f"{value}^{{commit}}").stdout.strip()
    if resolved != value:
        raise TrustedMergeError(
            f"{label} resolved to {resolved}, expected exact commit {value}"
        )
    return resolved


def build_trusted_merge(repo: Path, base: str, head: str) -> str:
    """Return a synthetic merge commit with exactly ``base`` and ``head`` parents."""

    base = _exact_commit(repo, base, "base")
    head = _exact_commit(repo, head, "head")
    merged = _git(repo, "merge-tree", "--write-tree", base, head, check=False)
    if merged.returncode != 0:
        detail = (merged.stderr or merged.stdout).strip()
        raise TrustedMergeError(
            "base and head do not merge cleanly"
            + (f": {detail}" if detail else "")
        )

    first_line = merged.stdout.splitlines()[0] if merged.stdout else ""
    if not SHA_RE.fullmatch(first_line):
        raise TrustedMergeError("git merge-tree did not return an exact tree SHA")
    tree = _git(repo, "rev-parse", "--verify", f"{first_line}^{{tree}}").stdout.strip()
    if tree != first_line:
        raise TrustedMergeError("git merge-tree returned an incoherent tree object")

    env = os.environ.copy()
    env.update(
        {
            "GIT_NO_REPLACE_OBJECTS": "1",
            "GIT_AUTHOR_NAME": "Pulp Vellum trusted gate",
            "GIT_AUTHOR_EMAIL": "vellum-trusted-gate@pulp.invalid",
            "GIT_AUTHOR_DATE": "2000-01-01T00:00:00Z",
            "GIT_COMMITTER_NAME": "Pulp Vellum trusted gate",
            "GIT_COMMITTER_EMAIL": "vellum-trusted-gate@pulp.invalid",
            "GIT_COMMITTER_DATE": "2000-01-01T00:00:00Z",
        }
    )
    committed = subprocess.run(
        ["git", "-C", str(repo), "commit-tree", tree, "-p", base, "-p", head],
        input=f"Trusted Vellum candidate for {base} + {head}\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        check=False,
    )
    if committed.returncode != 0:
        raise TrustedMergeError(
            committed.stderr.strip() or "git commit-tree failed"
        )
    candidate = committed.stdout.strip()
    parents = _git(repo, "rev-list", "--parents", "-n", "1", candidate).stdout.split()
    if parents != [candidate, base, head]:
        raise TrustedMergeError("synthetic merge commit has unexpected parents")
    if _git(repo, "rev-parse", f"{candidate}^{{tree}}").stdout.strip() != tree:
        raise TrustedMergeError("synthetic merge commit has unexpected tree")
    return candidate


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        candidate = build_trusted_merge(args.repo, args.base, args.head)
    except TrustedMergeError as exc:
        print(f"vellum-trusted-merge: {exc}", file=sys.stderr)
        return 1
    if args.output:
        args.output.write_text(candidate + "\n", encoding="utf-8")
    else:
        print(candidate)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
