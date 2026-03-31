#!/usr/bin/env python3
"""Run outer-loop validation locally and on optional SSH hosts."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = ROOT / "tools" / "deps" / "hosts.local.json"


def load_config(path: Path) -> dict:
    if not path.exists():
        return {"unix_targets": [], "windows_targets": []}
    return json.loads(path.read_text())


def current_branch() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def run(label: str, cmd: list[str]) -> bool:
    print(f"== {label} ==")
    proc = subprocess.run(cmd, cwd=ROOT)
    if proc.returncode != 0:
        print(f"{label}: FAILED ({proc.returncode})")
        return False
    print(f"{label}: OK")
    return True


def unix_remote_command(repo_path: str, branch: str, skip_tests: bool) -> str:
    validate = "./validate-build.sh --quiet"
    if skip_tests:
        validate += " --no-tests"
    return (
        f"cd {shlex.quote(repo_path)} && "
        f"git fetch origin && "
        f"git checkout {shlex.quote(branch)} && "
        f"git pull --ff-only origin {shlex.quote(branch)} && "
        f"{validate}"
    )


def windows_remote_command(repo_path: str, branch: str, skip_tests: bool) -> str:
    repo = repo_path.replace("'", "''")
    no_tests = "$true" if skip_tests else "$false"
    return (
        "$ErrorActionPreference='Stop'; "
        f"$repo='{repo}'; "
        f"git -C $repo fetch origin; "
        f"git -C $repo checkout {branch}; "
        f"git -C $repo pull --ff-only origin {branch}; "
        f"& \"$repo\\validate-build.ps1\" -Quiet -NoTests:{no_tests}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Run local/SSH validation hosts")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="JSON config path")
    parser.add_argument("--branch", default=None, help="Branch to validate remotely")
    parser.add_argument("--skip-tests", action="store_true", help="Skip tests")
    args = parser.parse_args()

    config = load_config(Path(args.config))
    branch = args.branch or current_branch()

    ok = True
    local_cmd = ["bash", "./validate-build.sh", "--quiet", "--ref", branch]
    if args.skip_tests:
        local_cmd.append("--no-tests")
    ok &= run("local", local_cmd)

    for target in config.get("unix_targets", []):
        label = f"ssh {target['host']}"
        cmd = [
            "ssh",
            "-o", "BatchMode=yes",
            target["host"],
            unix_remote_command(target["path"], branch, args.skip_tests),
        ]
        ok &= run(label, cmd)

    for target in config.get("windows_targets", []):
        label = f"ssh {target['host']}"
        ps = windows_remote_command(target["path"], branch, args.skip_tests)
        cmd = [
            "ssh",
            "-o", "BatchMode=yes",
            target["host"],
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-Command",
            ps,
        ]
        ok &= run(label, cmd)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
