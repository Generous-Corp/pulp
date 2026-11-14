#!/usr/bin/env python3
"""Run the PR-head affected-test contract before a local push.

The merge-group macOS job is the first place many PR-owned tests used to run.
That made a deterministic test failure eject every entry in the batch.  On a
light authoring host, reuse the same bounded selector used by CI while the
head is still cheap to repair.  A host without a configured build reports a
visible skip, never a false pass; the required CI lane remains authoritative.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def find_build(root: Path, requested: str | None) -> Path | None:
    candidates = [Path(requested)] if requested else []
    candidates.extend(root / name for name in ("build", "build-release", "build-debug"))
    for candidate in candidates:
        if candidate.is_dir() and (candidate / "CMakeCache.txt").is_file():
            return candidate.resolve()
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--build-dir")
    parser.add_argument("--budget-secs", type=float, default=600.0)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args(argv)
    root = args.root.resolve()
    build = find_build(root, args.build_dir or os.environ.get("PULP_GATES_BUILD_DIR"))
    if build is None:
        print(
            "prepush-affected-tests: SKIPPED — no configured build tree; "
            "run `pulp build` (or use a light host/VM) before pushing.",
            file=sys.stderr,
        )
        return 77
    runner = root / "tools" / "ci" / "pr_head_affected_tests.py"
    command = [
        sys.executable,
        str(runner),
        "--build-dir",
        str(build),
        "--base",
        args.base,
        "--head",
        "HEAD",
        "--budget-secs",
        str(args.budget_secs),
        "--jobs",
        str(args.jobs),
    ]
    print(f"prepush-affected-tests: running against {args.base} from {build}", file=sys.stderr)
    return subprocess.run(command, cwd=root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
