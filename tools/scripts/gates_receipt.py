#!/usr/bin/env python3
"""Head-keyed receipts for a passing `tools/scripts/gates.sh` run.

gates.sh runs every command of the required `Enforce version & skill sync`
job, and the pre-push hook does not. A push of a head gates.sh never passed
turned CI into the discovery channel: 35 heads in one week went red on a
cheap required check the PR itself broke. The receipt ties the push to a
passing run on that exact commit.

    gates_receipt.py write --root R --head SHA --base REF --exit-code N \
        [--dirty-at-start] [--not-checked N]
        Called by gates.sh on its success paths. Writes
        <git-common-dir>/pulp-gates/<SHA> only when the exit code is 0, HEAD is
        still SHA, and no tracked file differed from HEAD at the start or at
        the end. Otherwise it writes nothing and says why. Always exits 0: a
        missing receipt only means the next push is refused.

    gates_receipt.py check --root R  < pre-push stdin records
        Called by .githooks/pre-push. Exits 0 when every pushed ref is allowed,
        1 (with the command to run) when one is not, 2 on an internal error
        (the hook refuses on any non-zero exit). Allowed without a receipt:
        deletions, refs/tags/*, and commits already on origin/main.

The owner-only override PULP_GATES_RECEIPT_OWNER_OVERRIDE=1 allows the push
and appends a line to <git-common-dir>/pulp-gates/owner-overrides.log. It is
for the repository owner in an emergency, not for agents: an agent runs
`tools/scripts/gates.sh origin/main` instead.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ZERO_SHA = "0" * 40
OVERRIDE_ENV = "PULP_GATES_RECEIPT_OWNER_OVERRIDE"
MAIN_REF = "refs/remotes/origin/main"


def _git(root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, encoding="utf-8")


def receipt_dir(root: Path) -> Path:
    res = _git(root, "rev-parse", "--path-format=absolute", "--git-common-dir")
    if res.returncode != 0 or not res.stdout.strip():
        raise RuntimeError(f"git rev-parse --git-common-dir failed: {res.stderr.strip()}")
    return Path(res.stdout.strip()) / "pulp-gates"


def tracked_dirty(root: Path) -> list[str]:
    """Tracked paths whose working-tree or index content differs from HEAD.

    Same definition as the pre-push hook's dirty-tree warning: untracked files
    do not count (build output and editor files are not part of the commit).
    """
    res = _git(root, "diff", "--name-only", "--ignore-submodules", "HEAD")
    if res.returncode != 0:
        return ["<git diff failed>"]
    return [line for line in res.stdout.splitlines() if line.strip()]


def _now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def write(args: argparse.Namespace) -> int:
    root = Path(args.root)
    head = args.head
    if args.exit_code != 0:
        print(f"gates: no receipt for {head[:12]} (gates exited {args.exit_code}).", file=sys.stderr)
        return 0
    if args.dirty_at_start:
        print(f"gates: no receipt for {head[:12]}: tracked files differed from HEAD when gates.sh "
              "started, so the gates did not judge the commit. Commit or stash, then re-run.",
              file=sys.stderr)
        return 0
    now_head = _git(root, "rev-parse", "HEAD").stdout.strip()
    if now_head != head:
        print(f"gates: no receipt: HEAD moved from {head[:12]} to {now_head[:12] or '?'} during the run.",
              file=sys.stderr)
        return 0
    dirty = tracked_dirty(root)
    if dirty:
        print(f"gates: no receipt for {head[:12]}: tracked files differ from HEAD ({', '.join(dirty[:5])}).",
              file=sys.stderr)
        return 0
    base_sha = _git(root, "rev-parse", "--verify", "--quiet", f"{args.base}^{{commit}}").stdout.strip()
    body = {
        "head": head,
        "base": args.base,
        "base_sha": base_sha,
        "ts": _now(),
        "host": socket.gethostname(),
        "not_checked": args.not_checked,
    }
    try:
        d = receipt_dir(root)
        d.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=d, prefix=f".{head}.")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(body, fh, sort_keys=True)
            fh.write("\n")
        os.replace(tmp, d / head)
    except (OSError, RuntimeError) as exc:
        print(f"gates: receipt not written ({exc}).", file=sys.stderr)
        return 0
    print(f"gates: receipt written for {head} (pre-push accepts this exact head).", file=sys.stderr)
    return 0


def has_receipt(d: Path, sha: str) -> bool:
    try:
        data = json.loads((d / sha).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return isinstance(data, dict) and data.get("head") == sha


def on_main(root: Path, sha: str) -> bool:
    if _git(root, "rev-parse", "--verify", "--quiet", MAIN_REF).returncode != 0:
        return False
    return _git(root, "merge-base", "--is-ancestor", sha, MAIN_REF).returncode == 0


def check(args: argparse.Namespace) -> int:
    root = Path(args.root)
    records = [line.split() for line in sys.stdin.read().splitlines() if line.strip()]
    d = receipt_dir(root)
    if os.environ.get(OVERRIDE_ENV) == "1":
        heads = " ".join(r[1] for r in records if len(r) == 4) or "-"
        line = f"{_now()}\t{socket.gethostname()}\t{root}\t{heads}\n"
        try:
            d.mkdir(parents=True, exist_ok=True)
            with open(d / "owner-overrides.log", "a", encoding="utf-8") as fh:
                fh.write(line)
        except OSError as exc:
            print(f"[pre-push] gates receipt: override log not written ({exc}).", file=sys.stderr)
        print(f"[pre-push] gates receipt: NOT CHECKED, {OVERRIDE_ENV}=1 (owner-only; logged to "
              f"{d / 'owner-overrides.log'}).", file=sys.stderr)
        return 0
    missing: list[tuple[str, str]] = []
    for rec in records:
        if len(rec) != 4:
            print(f"[pre-push] gates receipt: malformed pushed-ref record: {' '.join(rec)}", file=sys.stderr)
            return 2
        _local_ref, local_sha, remote_ref, _remote_sha = rec
        if local_sha == ZERO_SHA or remote_ref.startswith("refs/tags/"):
            continue
        if has_receipt(d, local_sha) or on_main(root, local_sha):
            continue
        missing.append((remote_ref, local_sha))
    if not missing:
        return 0
    for remote_ref, sha in missing:
        print(f"✗ pre-push: no passing gates.sh receipt for {sha} ({remote_ref}).", file=sys.stderr)
    print("  The required `Enforce version & skill sync` checks run every command gates.sh runs.", file=sys.stderr)
    print("  Check out that exact head with a clean tree and run:", file=sys.stderr)
    print("      tools/scripts/gates.sh origin/main", file=sys.stderr)
    print("  then push again without amending. A passing run records the receipt.", file=sys.stderr)
    return 1


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write")
    w.add_argument("--root", required=True)
    w.add_argument("--head", required=True)
    w.add_argument("--base", required=True)
    w.add_argument("--exit-code", type=int, required=True)
    w.add_argument("--dirty-at-start", action="store_true")
    w.add_argument("--not-checked", type=int, default=0)
    c = sub.add_parser("check")
    c.add_argument("--root", required=True)
    args = p.parse_args(argv)
    try:
        return write(args) if args.cmd == "write" else check(args)
    except RuntimeError as exc:
        print(f"[pre-push] gates receipt: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
