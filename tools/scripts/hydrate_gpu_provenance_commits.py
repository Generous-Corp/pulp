#!/usr/bin/env python3
"""Fetch only the Pulp commits named by checked-in GPU provenance evidence."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import os
import subprocess
import sys
from typing import Any


SHA = re.compile(r"[0-9a-f]{40}")
MAX_COMMITS = 128


class HydrationError(RuntimeError):
    """Checked-in provenance cannot be materialized safely."""


def load_object(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HydrationError(f"cannot read provenance document {path}: {error}") from error
    if not isinstance(value, dict):
        raise HydrationError(f"provenance document is not an object: {path}")
    return value


def required_commits(root: pathlib.Path) -> list[str]:
    handoff = load_object(root / "docs/status/gpu-vellum-handoff.yaml")
    receipt = load_object(
        root / "docs/validation/gpu-probes/m3-a2-real-probes-20260828/receipt.json"
    )
    revisions: set[str] = set()
    for entry in handoff.get("entries", []):
        if not isinstance(entry, dict):
            raise HydrationError("GPU handoff contains a non-object entry")
        for row in entry.get("pulp_paths", []):
            if not isinstance(row, dict):
                raise HydrationError("GPU handoff contains a non-object Pulp path")
            if row.get("repo") != "Generous-Corp/pulp":
                raise HydrationError("GPU handoff Pulp path names a non-Pulp repository")
            revision = row.get("revision")
            if not isinstance(revision, str) or SHA.fullmatch(revision) is None:
                raise HydrationError("GPU handoff Pulp path has an invalid revision")
            revisions.add(revision)
    equivalent_head = receipt.get("verification_equivalent_head")
    if not isinstance(equivalent_head, str) or SHA.fullmatch(equivalent_head) is None:
        raise HydrationError("GPU probe receipt has an invalid verification_equivalent_head")
    revisions.add(equivalent_head)
    if not revisions or len(revisions) > MAX_COMMITS:
        raise HydrationError(
            f"GPU provenance requests {len(revisions)} commits; allowed range is 1..{MAX_COMMITS}"
        )
    return sorted(revisions)


def is_commit(root: pathlib.Path, revision: str) -> bool:
    return subprocess.run(
        ["git", "cat-file", "-e", f"{revision}^{{commit}}"], cwd=root,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    ).returncode == 0


def hydrate(root: pathlib.Path, remote: str) -> tuple[int, int]:
    revisions = required_commits(root)
    missing = [revision for revision in revisions if not is_commit(root, revision)]
    shallow = subprocess.run(
        ["git", "rev-parse", "--is-shallow-repository"], cwd=root,
        text=True, capture_output=True, check=True,
    ).stdout.strip() == "true"
    # A warm self-hosted checkout can retain every object while checkout@v5
    # rewrites the shallow boundary. Object presence therefore does not prove
    # ancestry; always reconnect the exact event history when Git says the
    # repository is shallow.
    if shallow:
        event_ref = os.environ.get("GITHUB_REF", "")
        if not event_ref.startswith("refs/"):
            raise HydrationError("shallow checkout lacks an exact GITHUB_REF to hydrate")
        completed = subprocess.run(
            [
                "git", "fetch", "--no-tags", "--unshallow", remote,
                f"+{event_ref}:refs/pulp-ci/gpu-provenance/event",
            ],
            cwd=root, text=True, capture_output=True, check=False,
        )
        if completed.returncode != 0:
            detail = (completed.stderr or completed.stdout).strip()
            raise HydrationError(f"bounded GPU provenance fetch failed: {detail}")
    unresolved = [revision for revision in revisions if not is_commit(root, revision)]
    if unresolved:
        raise HydrationError(f"GPU provenance commits remain unresolved: {unresolved}")
    non_ancestors = [
        revision for revision in revisions
        if subprocess.run(
            ["git", "merge-base", "--is-ancestor", revision, "HEAD"], cwd=root,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
        ).returncode != 0
    ]
    if non_ancestors:
        raise HydrationError(f"GPU provenance commits are not ancestors of HEAD: {non_ancestors}")
    return len(revisions), len(missing)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).parents[2])
    parser.add_argument("--remote", default="origin")
    args = parser.parse_args(argv)
    try:
        total, fetched = hydrate(args.root.resolve(), args.remote)
    except HydrationError as error:
        print(f"gpu-provenance-hydration: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"gpu-provenance-hydration: PASS total={total} fetched={fetched} cap={MAX_COMMITS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
