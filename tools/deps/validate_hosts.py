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


def current_sha() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    value = result.stdout.strip()
    if len(value) != 40 or any(character not in "0123456789abcdef" for character in value):
        raise RuntimeError(f"could not bind remote validation to exact source SHA: {value!r}")
    return value


def run(label: str, cmd: list[str]) -> bool:
    print(f"== {label} ==")
    proc = subprocess.run(cmd, cwd=ROOT)
    if proc.returncode != 0:
        print(f"{label}: FAILED ({proc.returncode})")
        return False
    print(f"{label}: OK")
    return True


def unix_remote_command(
    repo_path: str,
    branch: str,
    skip_tests: bool,
    render_toolchain: bool = False,
    expected_sha: str | None = None,
) -> str:
    validate = "./validate-build.sh --quiet"
    if skip_tests:
        validate += " --no-tests"
    repo = shlex.quote(repo_path)
    branch_q = shlex.quote(branch)
    render_validate = ""
    if render_toolchain:
        if expected_sha is None:
            raise ValueError("render-toolchain remote validation requires an exact source SHA")
        expected_sha_q = shlex.quote(expected_sha)
        render_validate = f"""
actual_sha="$(git rev-parse HEAD)"
if [ "$actual_sha" != {expected_sha_q} ]; then
    echo "ERROR: render-toolchain remote source $actual_sha does not match expected {expected_sha}" >&2
    exit 1
fi
python3 tools/deps/validate_render_update.py --cache-only
"""
    return f"""
set -e
export PATH="$HOME/bin:$HOME/.local/bin:$PATH"
cd {repo}
git rev-parse --is-inside-work-tree >/dev/null
if git remote get-url origin >/dev/null 2>&1; then
    git fetch origin
    if git ls-remote --exit-code --heads origin {branch_q} >/dev/null 2>&1; then
        git checkout {branch_q} >/dev/null 2>&1 || git checkout -b {branch_q} --track origin/{branch_q}
        git pull --ff-only origin {branch_q}
    elif git show-ref --verify --quiet refs/heads/{branch_q}; then
        git checkout {branch_q}
    else
        echo "warning: branch {branch} not available on remote or locally; validating current checkout" >&2
    fi
else
    if git show-ref --verify --quiet refs/heads/{branch_q}; then
        git checkout {branch_q}
    else
        echo "warning: no origin remote configured; validating current checkout" >&2
    fi
fi
{validate}{render_validate}
""".strip()


def windows_remote_command(repo_path: str, branch: str, skip_tests: bool) -> str:
    repo = repo_path.replace("'", "''")
    branch_q = branch.replace("'", "''")
    no_tests = "$true" if skip_tests else "$false"
    return (
        "$ErrorActionPreference='Stop'; "
        f"$repo='{repo}'; "
        f"$branch='{branch_q}'; "
        "if (-not (Test-Path $repo)) { throw \"repo path missing: $repo\" }; "
        "if (-not (Test-Path (Join-Path $repo '.git'))) { throw \"not a git checkout: $repo\" }; "
        "git -C $repo rev-parse --is-inside-work-tree *> $null; "
        "if ((git -C $repo remote get-url origin) 2>$null) { "
        "  git -C $repo fetch origin; "
        "  git ls-remote --exit-code --heads origin $branch *> $null; "
        "  if ($LASTEXITCODE -eq 0) { "
        "    git -C $repo checkout $branch *> $null; "
        "    if ($LASTEXITCODE -ne 0) { git -C $repo checkout -b $branch --track (\"origin/\" + $branch) *> $null }; "
        "    git -C $repo pull --ff-only origin $branch; "
        "  } elseif ((git -C $repo show-ref --verify --quiet (\"refs/heads/\" + $branch)) 2>$null) { "
        "    git -C $repo checkout $branch *> $null; "
        "  } else { "
        "    Write-Warning \"branch $branch not available on remote or locally; validating current checkout\"; "
        "  } "
        "} else { "
        "  if ((git -C $repo show-ref --verify --quiet (\"refs/heads/\" + $branch)) 2>$null) { "
        "    git -C $repo checkout $branch *> $null; "
        "  } else { "
        "    Write-Warning \"no origin remote configured; validating current checkout\"; "
        "  } "
        "} "
        f"& \"$repo\\validate-build.ps1\" -Quiet -NoTests:{no_tests}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Run local/SSH validation hosts")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="JSON config path")
    parser.add_argument("--branch", default=None, help="Branch to validate remotely")
    parser.add_argument("--skip-tests", action="store_true", help="Skip tests")
    parser.add_argument(
        "--render-toolchain",
        action="store_true",
        help="also prove the pinned render provider, warm-cache integrity, and mixed-provider path",
    )
    args = parser.parse_args()

    config = load_config(Path(args.config))
    branch = args.branch or current_branch()
    expected_sha = current_sha() if args.render_toolchain else None

    ok = True
    local_cmd = ["bash", "./validate-build.sh", "--quiet", "--ref", branch]
    if args.skip_tests:
        local_cmd.append("--no-tests")
    ok &= run("local", local_cmd)
    if args.render_toolchain:
        ok &= run(
            "local render toolchain",
            [sys.executable, "tools/deps/validate_render_update.py"],
        )

    for target in config.get("unix_targets", []):
        label = f"ssh {target['host']}"
        cmd = [
            "ssh",
            "-o", "BatchMode=yes",
            target["host"],
            unix_remote_command(
                target["path"], branch, args.skip_tests, args.render_toolchain,
                expected_sha,
            ),
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
