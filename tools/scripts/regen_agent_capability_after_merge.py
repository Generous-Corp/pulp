#!/usr/bin/env python3
"""Regenerate the agent capability records after merging main into a branch.

A merge that touches a public capability header conflicts in the generated
files under docs/status/agent-capability-surface/ and
tools/agent-capabilities/. Taking one side of a whole merge to clear them can
empty a branch's diff, so this script does only the safe, mechanical steps:

  1. refuse while any conflict or conflict marker remains outside the two
     generated trees;
  2. resolve conflicts inside those trees by taking main's side;
  3. refuse while a merge is still uncommitted (commit it, then rerun);
  4. run --accept-fingerprint for every header whose fingerprint `--check`
     reports as changed, then --write, then --check;
  5. print `git diff --stat <base>` and fail when the branch's diff against
     the base is empty or much smaller than the previous head's.

It never commits and never pushes.

Exit codes: 0 ok, 2 refused (conflicts or markers outside generated trees),
3 manifest tool failure, 4 merge still in progress, 5 diff guard tripped.

Usage:
    regen_agent_capability_after_merge.py [--base origin/main]
        [--previous-head <sha>] [--main-side ours|theirs] [--manifest PATH]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

GENERATED = ("docs/status/agent-capability-surface/", "tools/agent-capabilities/")
CHANGED = re.compile(r"public header fingerprint changed: (\S+?);")
MARKER = re.compile(r"^(<{7}|={7}|>{7})( |$)", re.MULTILINE)


def run(args: list[str], root: pathlib.Path, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(args, cwd=root, text=True, encoding="utf-8", capture_output=True)
    if check and result.returncode != 0:
        sys.stderr.write(f"{' '.join(args)} failed:\n{result.stdout}{result.stderr}")
        raise SystemExit(3)
    return result


def git(root: pathlib.Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return run(["git", *args], root, check)


def is_generated(path: str) -> bool:
    return path.startswith(GENERATED)


def unmerged(root: pathlib.Path) -> list[str]:
    out = git(root, "diff", "--name-only", "--diff-filter=U").stdout
    return [line for line in out.splitlines() if line]


def merge_in_progress(root: pathlib.Path) -> bool:
    return git(root, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).returncode == 0


def detect_main_side(root: pathlib.Path, base: str) -> str:
    """Merging main into a branch makes main `theirs`; the reverse makes it `ours`."""
    merge_head = git(root, "rev-parse", "MERGE_HEAD", check=False).stdout.strip()
    if not merge_head:
        return "theirs"
    contained = git(root, "merge-base", "--is-ancestor", merge_head, base, check=False)
    return "theirs" if contained.returncode == 0 else "ours"


def marker_files(root: pathlib.Path, base: str) -> list[str]:
    """Files outside the generated trees that carry conflict markers.

    Scans unmerged paths and every file the branch changes against the base."""
    names = set(unmerged(root))
    names.update(git(root, "diff", "--name-only", base, check=False).stdout.splitlines())
    found: list[str] = []
    for name in sorted(names):
        if is_generated(name):
            continue
        path = root / name
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if MARKER.search(text) and "<<<<<<<" in text:
            found.append(name)
    return found


def changed_count(root: pathlib.Path, spec: list[str]) -> int:
    out = git(root, "diff", "--name-only", *spec).stdout
    return len([line for line in out.splitlines() if line])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--previous-head")
    parser.add_argument("--main-side", choices=("ours", "theirs"))
    parser.add_argument("--manifest", default="tools/scripts/agent_capability_manifest.py")
    args = parser.parse_args()

    root = pathlib.Path(git(pathlib.Path.cwd(), "rev-parse", "--show-toplevel").stdout.strip())
    manifest = str(pathlib.Path(args.manifest))

    # 1. Nothing outside the generated trees may be unresolved.
    outside = [p for p in unmerged(root) if not is_generated(p)]
    outside += [p for p in marker_files(root, args.base) if p not in outside]
    if outside:
        sys.stderr.write("refusing: unresolved conflicts or conflict markers outside the "
                         "generated capability trees:\n  " + "\n  ".join(outside) + "\n")
        return 2

    # 2. Generated conflicts take main's side.
    generated = [p for p in unmerged(root) if is_generated(p)]
    if generated:
        side = args.main_side or detect_main_side(root, args.base)
        for path in generated:
            git(root, "checkout", f"--{side}", "--", path)
            git(root, "add", "--", path)
            print(f"resolved {path} with main's side ({side})")

    # 3. The merge must be committed by the caller first.
    if merge_in_progress(root):
        sys.stderr.write("a merge is still in progress: commit the merge (git commit --no-edit), "
                         "then rerun this script on the committed tree\n")
        return 4

    # 4. Accept changed fingerprints, regenerate, verify.
    check = run([sys.executable, manifest, "--check"], root, check=False)
    changed = sorted(set(CHANGED.findall(check.stdout + check.stderr)))
    if changed:
        run([sys.executable, manifest, "--accept-fingerprint", *changed], root)
    if changed or check.returncode != 0:
        run([sys.executable, manifest, "--write"], root)
        final = run([sys.executable, manifest, "--check"], root, check=False)
        if final.returncode != 0:
            sys.stderr.write("--check still failing after --write:\n"
                             f"{final.stdout}{final.stderr}")
            return 3
    print(f"fingerprints accepted: {', '.join(changed) if changed else 'none needed'}")

    # 5. Diff guard.
    print(git(root, "diff", "--stat", args.base).stdout, end="")
    now = changed_count(root, [args.base])
    if now == 0:
        sys.stderr.write(f"refusing: the branch diff against {args.base} is empty\n")
        return 5
    if args.previous_head:
        before = changed_count(root, [f"{args.base}...{args.previous_head}"])
        if now * 2 < before:
            sys.stderr.write(f"refusing: the branch lost files ({before} changed files at "
                             f"{args.previous_head}, {now} now)\n")
            return 5
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
