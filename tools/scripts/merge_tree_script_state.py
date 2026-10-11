#!/usr/bin/env python3
"""Run the cheap, script-owned drift checks on HEAD merged with main.

The author refreshes a branch with ``git merge origin/main``.  This helper lets
the local gates inspect that same hypothetical tree before the merge exists,
so generated script manifests cannot silently validate only the old branch.
Compiled/build checks intentionally do not run here.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


STALE = "your branch is stale against main; merge origin/main and regenerate"


def git(root: Path, *args: str, check: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", "-C", str(root), *args], text=True,
                          capture_output=True, check=check)


def _run(cmd: list[str], cwd: Path, env: dict[str, str], timeout: int = 20) -> int:
    try:
        proc = subprocess.run(cmd, cwd=cwd, env=env, text=True,
                              timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"merge-tree script-state: {cmd[0]} unavailable or timed out: {exc}", file=sys.stderr)
        return 2
    return proc.returncode


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", type=Path, default=Path.cwd())
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--head", default="HEAD")
    args = ap.parse_args(argv)
    root = args.repo_root.resolve()
    base = git(root, "rev-parse", "--verify", args.base)
    head = git(root, "rev-parse", "--verify", args.head)
    if base.returncode or head.returncode:
        print("merge-tree script-state: cannot resolve base or head", file=sys.stderr)
        return 2
    base_sha, head_sha = base.stdout.strip(), head.stdout.strip()
    merged = git(root, "merge-tree", "--write-tree", base_sha, head_sha)
    if merged.returncode != 0 or not merged.stdout.strip().splitlines():
        print(f"merge-tree script-state: {STALE} (would be DIRTY)", file=sys.stderr)
        return 1
    tree = merged.stdout.strip().splitlines()[0]
    commit = subprocess.run(
        ["git", "-C", str(root), "commit-tree", tree, "-p", base_sha, "-p", head_sha],
        input="merge-tree script-state\n", text=True, capture_output=True,
        env={**os.environ, "GIT_AUTHOR_NAME": "pulp-gates", "GIT_AUTHOR_EMAIL": "gates@localhost",
             "GIT_COMMITTER_NAME": "pulp-gates", "GIT_COMMITTER_EMAIL": "gates@localhost"},
        check=False,
    )
    if commit.returncode:
        print("merge-tree script-state: could not materialize merge tree", file=sys.stderr)
        return 2
    synthetic = commit.stdout.strip()
    tmp = Path(tempfile.mkdtemp(prefix="pulp-merge-tree-", dir=root.parent))
    try:
        added = subprocess.run(["git", "-C", str(root), "worktree", "add", "--detach", str(tmp), synthetic],
                               text=True, capture_output=True)
        if added.returncode:
            print(added.stderr, file=sys.stderr)
            return 2
        env = os.environ.copy()
        env["PULP_GATES_NO_CONFIGURE"] = "1"
        checks: list[tuple[str, list[str]]] = [
            ("script-test-inputs-drift", [sys.executable, str(tmp / "tools/scripts/gates_script_inputs.py"),
                                            "--base", base_sha, "--repo-root", str(tmp)]),
            ("changed-surface-script-families-drift", [sys.executable, str(tmp / "tools/scripts/changed_surface_script_families.py"),
                                                        "--repo-root", str(tmp), "--static", "--base", base_sha,
                                                        "--head", synthetic]),
            ("consumption-census-schema", [sys.executable, str(tmp / "tools/scripts/consumption_census.py"),
                                            "--source-root", str(tmp), "--validate-only"]),
            ("agent-capability-manifest", [sys.executable, str(tmp / "tools/scripts/agent_capability_manifest.py"), "--check"]),
            ("vellum-inheritance-ledger", [sys.executable, str(tmp / "tools/scripts/vellum_watch_preflight.py"),
                                            "--repo", str(tmp), "--base", base_sha, "--head", synthetic,
                                            "--enforce"]),
            ("unregistered-tests", [sys.executable, str(tmp / "tools/scripts/unregistered_tests_check.py"),
                                    "--repo-root", str(tmp)]),
        ]
        failed = 0
        for name, cmd in checks:
            if not Path(cmd[1]).exists():
                continue
            print(f"merge-tree script-state: {name}", file=sys.stderr)
            rc = _run(cmd, tmp, env)
            if rc:
                failed = 1
        if failed:
            print(f"merge-tree script-state: {STALE}", file=sys.stderr)
        return failed
    finally:
        subprocess.run(["git", "-C", str(root), "worktree", "remove", "--force", str(tmp)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
