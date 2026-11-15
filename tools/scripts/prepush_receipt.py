#!/usr/bin/env python3
"""Append one JSON line per push to a per-host receipt log.

A CI failure that a local gate should have caught is hard to explain after the
fact: the push either skipped the hook, demoted a gate, ran on a gate host that
refuses lanes, or the gate ran and missed it. `.githooks/pre-push` calls this on
exit with what it observed, so a later failure can be traced to one of those.

Record fields: ts, host, worktree, hooks_path, hooks_path_is_githooks, refs (the
pushed-ref records git gave the hook), pushed_head, head, base, merge_base,
head_is_merge, merge_from_base, gate_host, env (the bypass knobs: the PULP_*
toggles' values, and only presence for SHIPYARD_AGENT_ID and the Shipyard-origin
markers; no other environment is read), gates (each `ran` with its exit status,
or `skipped` with a reason), exit_code and outcome.

The log is ~/.local/state/pulp/prepush-receipts.jsonl (PULP_PREPUSH_RECEIPT_LOG
overrides it), in a 0700 directory, file 0600. Writing it never fails a push:
every error is a one-line warning on stderr and exit 0.

    prepush_receipt.py --root DIR --events FILE --exit-code N [--base REF]
                       [--gate-host 0|1] < pushed-ref-records
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import socket
import subprocess
import sys
from pathlib import Path

# Values are recorded for these (they are on/off toggles, never secrets).
VALUE_KNOBS = (
    "PULP_SKIP_PREPUSH",
    "PULP_DISABLE_PREPUSH_GATES",
    "PULP_DISABLE_PREPUSH_DIFF_COVER",
    "PULP_SKIP_DIFF_COVER",
    "PULP_SKIP_PREPUSH_AFFECTED",
    "PULP_PREPUSH_ALLOW_NOT_CHECKED",
    "PULP_SKIP_PR_BATCH_ADVICE",
    "PULP_PREPUSH_BASE",
    "PULP_REFRESH_PUSH_POLICY",
)
# Only whether these are set is recorded.
PRESENCE_KNOBS = ("SHIPYARD_AGENT_ID", "SHIPYARD_PR_RUNNING", "PULP_VIA_SHIPYARD")


def log_path() -> Path:
    override = os.environ.get("PULP_PREPUSH_RECEIPT_LOG")
    if override:
        return Path(override)
    state = os.environ.get("XDG_STATE_HOME") or str(Path.home() / ".local" / "state")
    return Path(state) / "pulp" / "prepush-receipts.jsonl"


def git(root: str, *args: str) -> str:
    try:
        proc = subprocess.run(["git", "-C", root, *args], capture_output=True,
                              text=True, encoding="utf-8", timeout=10, check=False)
    except (OSError, subprocess.SubprocessError):
        return ""
    return proc.stdout.strip() if proc.returncode == 0 else ""


def parse_refs(text: str) -> list[dict]:
    refs = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4:
            refs.append({"local_ref": parts[0], "local_sha": parts[1],
                         "remote_ref": parts[2], "remote_sha": parts[3]})
    return refs


def parse_events(path: str) -> list[dict]:
    gates: list[dict] = []
    try:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return gates
    for line in lines:
        kind, _, rest = line.partition("\t")
        name, _, detail = rest.partition("\t")
        if kind == "ran":
            gates.append({"gate": name, "status": "ran",
                          "exit": int(detail) if detail.lstrip("-").isdigit() else None})
        elif kind == "skipped":
            gates.append({"gate": name, "status": "skipped", "reason": detail})
        elif kind == "demoted":
            gates.append({"gate": name, "status": "demoted", "reason": detail})
    return gates


def build_record(args: argparse.Namespace, stdin_text: str) -> dict:
    root = args.root
    refs = parse_refs(stdin_text)
    head = git(root, "rev-parse", "HEAD")
    base = args.base or ""
    merge_base = git(root, "merge-base", base, "HEAD") if base else ""
    parents = git(root, "rev-list", "--parents", "-n", "1", "HEAD").split()[1:]
    base_sha = git(root, "rev-parse", "--verify", "--quiet", base + "^{commit}") if base else ""
    hooks_path = git(root, "config", "--get", "core.hooksPath")
    pushed = [r["local_sha"] for r in refs if set(r["local_sha"]) != {"0"}]
    return {
        "ts": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "host": socket.gethostname(),
        "worktree": root,
        "hooks_path": hooks_path,
        "hooks_path_is_githooks": hooks_path.rstrip("/") == ".githooks",
        "refs": refs,
        "pushed_head": pushed[0] if pushed else head,
        "head": head,
        "base": base,
        "merge_base": merge_base,
        "head_is_merge": len(parents) > 1,
        "merge_from_base": bool(base_sha) and base_sha in parents,
        "gate_host": args.gate_host == "1",
        "env": {
            **{k: os.environ[k] for k in VALUE_KNOBS if k in os.environ},
            **{k: (k in os.environ) for k in PRESENCE_KNOBS},
        },
        "gates": parse_events(args.events) if args.events else [],
        "exit_code": args.exit_code,
        "outcome": "allowed" if args.exit_code == 0 else "blocked",
    }


def append(record: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        os.chmod(path.parent, 0o700)
    except OSError:
        pass
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600)
    try:
        os.fchmod(fd, 0o600)
        os.write(fd, (json.dumps(record, sort_keys=True) + "\n").encode("utf-8"))
    finally:
        os.close(fd)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--root", required=True)
    parser.add_argument("--events", default="")
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--base", default="")
    parser.add_argument("--gate-host", default="0")
    args = parser.parse_args(argv)
    try:
        stdin_text = "" if sys.stdin.isatty() else sys.stdin.read()
        append(build_record(args, stdin_text), log_path())
    except Exception as exc:  # a receipt never fails a push
        print(f"[pre-push] receipt not written: {type(exc).__name__}: {exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
