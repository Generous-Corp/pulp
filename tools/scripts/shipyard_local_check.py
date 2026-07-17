#!/usr/bin/env python3
"""Advisory check: does `.shipyard.local/` silently disable the macOS gate?

`.shipyard/config.toml` declares `[targets.mac] backend = "local"`, so the
required `macos` check routes to the self-hosted runners in every checkout —
including a fresh worktree with no `.shipyard.local/` at all. That absence is
the CORRECT state, not a gap: the Mac Studio has no `.shipyard.local` and
routes correctly.

The hazard is the opposite one. Shipyard merges `.shipyard.local/config.toml`
on TOP of the project config, so a `[targets.mac]` block there can override
`backend` to a cloud provider. Nothing reports that: `shipyard pr` watches a
redundant GitHub-hosted run, times out at 3600s, and the required `macos`
check — which only the local runner posts — never appears. That override is
why the block in Pulp's own `.shipyard.local/config.toml` is commented out
and annotated `DISABLED 2026-07-09`.

So this check reads that file and warns only when an ACTIVE mac override
points somewhere other than local. It is strictly read-only: it never writes,
copies, or repairs anything. `.shipyard.local/config.toml` is gitignored while
`config.toml.example` is tracked, so a `cp -R` "fix" clobbers a tracked file.
Reporting the gap and leaving the repair to a human is the whole design.

Advisory: a deliberate cloud macOS lane is a legitimate (if rare) choice, and
this runs on the pre-push path where a hard block would strand a real push.

Usage:
    python3 tools/scripts/shipyard_local_check.py [--repo-root PATH]

Exit codes:
    0 — mac routes locally (or no override / no file / unreadable)
    1 — an active `[targets.mac]` override routes macOS off the local runners
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tomllib
from pathlib import Path

LOCAL_CONFIG = ".shipyard.local/config.toml"


def mac_reroute_warning(config_text: str) -> str | None:
    """Warn iff an active `[targets.mac]` sends macOS off the local runners.

    Returns None for the healthy cases: no `[targets.mac]` table (the override
    is absent or commented out), or one that keeps `backend = "local"`. A table
    with no `backend` key inherits the project config's local backend, so it is
    healthy too. Unparseable TOML is Shipyard's error to report, not ours.
    """
    try:
        parsed = tomllib.loads(config_text)
    except tomllib.TOMLDecodeError:
        return None
    mac = parsed.get("targets", {}).get("mac")
    if not isinstance(mac, dict):
        return None
    backend = mac.get("backend")
    if backend is None or backend == "local":
        return None
    provider = mac.get("runner_provider")
    return (
        f"⚠︎ {LOCAL_CONFIG} overrides [targets.mac] backend = \"{backend}\""
        + (f" (runner_provider = \"{provider}\")" if provider else "")
        + "\n"
        "    This overrides the repo's `backend = \"local\"` and routes the macOS\n"
        "    lane off the self-hosted runners. The REQUIRED `macos` check is\n"
        "    posted ONLY by the local runner, so it will never appear: shipyard\n"
        "    watches a redundant cloud run and times out at 3600s.\n"
        "    Confirm with `shipyard status` — it reports `mac: cloud`.\n"
        "    Fix: comment out that [targets.mac] block so `mac` resolves to the\n"
        "    repo's local target. Do NOT copy config.toml.example over it —\n"
        "    the .example is tracked and the live config is gitignored."
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=None)
    args = parser.parse_args(argv)

    root = Path(args.repo_root) if args.repo_root else None
    if root is None:
        try:
            root = Path(subprocess.run(
                ["git", "rev-parse", "--show-toplevel"],
                check=True, capture_output=True, text=True,
            ).stdout.strip())
        except subprocess.CalledProcessError:
            return 0

    config = root / LOCAL_CONFIG
    if not config.exists():
        # The correct state — the repo config's local mac target stands.
        return 0
    warning = mac_reroute_warning(config.read_text())
    if warning is None:
        return 0
    sys.stderr.write(warning + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
