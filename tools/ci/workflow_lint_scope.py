#!/usr/bin/env python3
"""Decide and report workflow-lint.yml on every event, merge groups included.

workflow-lint.yml carries no `paths:` filter, so it reports on every pull
request, merge-queue batch and push to main. This script is its scope and its
verdict:

    workflow_lint_scope.py scope --files changed.txt   # prints true or false
    workflow_lint_scope.py scope --from-event          # reads the event env
    workflow_lint_scope.py verdict --scope-result R --run T \
        --job lint=R --job generated-version-bump-contract=R

`scope` matches the changed paths against tools/ci/workflow_lint_paths.txt
with the same glob rules GitHub's path filters use (shared with
release_path_changed.py). It fails toward running the suites: an unreadable
diff, a new branch, or 300+ changed files (the compare API's listing cap)
prints `true`.

`verdict` is the aggregate `workflow-lint` job's exit code: success only when
the scope job succeeded and, when it chose to run, every suite job succeeded.
A skipped suite passes only when the scope said not to run it.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_path_changed import touches  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
PATHS_FILE = REPO / "tools" / "ci" / "workflow_lint_paths.txt"
ZERO_SHA = "0" * 40


def load_globs(path: Path = PATHS_FILE) -> list[str]:
    globs: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            globs.append(line)
    return globs


def event_range(env: dict[str, str]) -> tuple[str, str] | None:
    """The (base, head) the event changes, or None when it cannot be read."""
    event = env.get("EVENT", "")
    if event == "pull_request":
        base, head = env.get("PR_BASE", ""), env.get("PR_HEAD", "")
    elif event == "merge_group":
        base, head = env.get("MQ_BASE", ""), env.get("MQ_HEAD", "")
    elif event == "push":
        base, head = env.get("PUSH_BEFORE", ""), env.get("PUSH_AFTER", "")
    else:
        return None
    if not base or not head or base == ZERO_SHA:
        return None
    return base, head


def compare_files(repo: str, base: str, head: str) -> list[str]:
    """Changed paths base...head, both sides of a rename."""
    out = subprocess.run(
        ["gh", "api", f"repos/{repo}/compare/{base}...{head}"],
        check=True, capture_output=True, text=True, timeout=120,
    ).stdout
    files: list[str] = []
    for entry in json.loads(out).get("files") or []:
        files.append(entry["filename"])
        if entry.get("previous_filename"):
            files.append(entry["previous_filename"])
    return files


def decide(files: list[str] | None, globs: list[str]) -> bool:
    """None (an unread diff) runs the suites."""
    if files is None:
        return True
    return touches(files, globs)


def cmd_scope(args: argparse.Namespace) -> int:
    globs = load_globs(args.paths)
    if not globs:
        print(f"no globs in {args.paths}", file=sys.stderr)
        return 2
    files: list[str] | None = None
    if args.files:
        files = [f for f in args.files.read_text(encoding="utf-8").splitlines() if f.strip()]
    else:
        rng = event_range(dict(os.environ))
        if rng is None:
            print("event range unreadable; running every suite", file=sys.stderr)
        else:
            try:
                files = compare_files(args.repo or os.environ.get("GITHUB_REPOSITORY", ""), *rng)
            except (subprocess.SubprocessError, OSError, ValueError, KeyError) as exc:
                print(f"compare failed ({exc}); running every suite", file=sys.stderr)
    print("true" if decide(files, globs) else "false")
    return 0


def verdict(scope_result: str, run: str, jobs: dict[str, str]) -> tuple[int, str]:
    if scope_result != "success":
        return 1, f"scope job ended {scope_result or 'without a result'}; nothing was judged"
    if run == "false":
        ran = sorted(name for name, result in jobs.items() if result != "skipped")
        if ran:
            return 1, f"out of scope but suites did not skip: {', '.join(ran)}"
        return 0, "change touches no workflow-lint path; suites skipped"
    if run != "true":
        return 1, f"scope produced no decision ({run!r})"
    failed = sorted(f"{name}={result}" for name, result in jobs.items() if result != "success")
    if failed:
        return 1, f"workflow-lint suites did not pass: {', '.join(failed)}"
    return 0, "every workflow-lint suite passed"


def cmd_verdict(args: argparse.Namespace) -> int:
    jobs: dict[str, str] = {}
    for item in args.job:
        name, _, result = item.partition("=")
        jobs[name] = result
    if not jobs:
        print("verdict needs at least one --job", file=sys.stderr)
        return 2
    rc, message = verdict(args.scope_result, args.run, jobs)
    level = "notice" if rc == 0 else "error"
    print(f"::{level} title=workflow-lint::{message}")
    return rc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    scope = sub.add_parser("scope")
    scope.add_argument("--paths", type=Path, default=PATHS_FILE)
    src = scope.add_mutually_exclusive_group(required=True)
    src.add_argument("--files", type=Path, help="newline-separated changed paths")
    src.add_argument("--from-event", action="store_true")
    scope.add_argument("--repo", default="")
    scope.set_defaults(func=cmd_scope)
    ver = sub.add_parser("verdict")
    ver.add_argument("--scope-result", required=True)
    ver.add_argument("--run", required=True)
    ver.add_argument("--job", action="append", default=[], help="name=result")
    ver.set_defaults(func=cmd_verdict)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
