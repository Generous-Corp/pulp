#!/usr/bin/env python3
"""Resolve `runs-on` for every release-cli leg — one platform -> runs-on map.

Used by `.github/workflows/release-cli.yml`'s `resolve-macos-runner` job. Each
build/smoke leg then indexes the map by `matrix.platform`, so routing is DATA
rather than a chain of ternaries inside the `runs-on:` key.

Why this exists
---------------
Before this, only the macOS legs were variable-driven; every other leg fell through
to `|| matrix.os` — a literal GitHub-hosted label. That was the *only* reason Linux
and Windows releases could not run on the self-hosted VMs that were already booted
and idle on the local pool. The VMs were there; the wiring was not.

The fluidity invariant
----------------------
With EVERY variable unset, this must reproduce the previous hard-coded routing
exactly (see `HOSTED`). Opting a leg onto the local pool is `gh variable set`;
reverting it is `gh variable unset`. Neither requires a code change, which is the
whole point — if the local pool is down, or you simply want GitHub's runners back,
it must be one command and take effect on the next tag.

A malformed variable fails LOUD. A release routed to a runner that does not exist
would sit queued forever, and a job that never starts is the single failure mode
this pipeline is worst at noticing — see `release_reconcile.py`'s STUCK_QUEUE.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

# The GitHub-hosted label each leg used before per-leg routing existed. These
# defaults ARE the fluidity invariant.
HOSTED: dict[str, object] = {
    "darwin-arm64": ["macos-15"],
    "darwin-x64": ["macos-15"],
    "linux-x64": "ubuntu-24.04",
    "linux-arm64": "ubuntu-24.04-arm",
    "windows-x64": "windows-latest",
    "windows-arm64": "windows-11-arm",
}

# Per-platform env vars, in priority order. The first non-empty one wins.
# The legacy macOS variables are honoured so an existing config keeps working.
SOURCES: dict[str, tuple[str, ...]] = {
    "darwin-arm64": ("DARWIN_ARM64", "LOCAL_MACOS", "NAMESPACE_JSON"),
    "darwin-x64": ("DARWIN_X64",),
    "linux-x64": ("LINUX_X64",),
    "linux-arm64": ("LINUX_ARM64",),
    "windows-x64": ("WINDOWS_X64",),
    "windows-arm64": ("WINDOWS_ARM64",),
}


def resolve(env: dict[str, str]) -> dict[str, object]:
    """platform -> runs-on. Pure: takes the environment, returns the map."""
    out: dict[str, object] = {}
    for platform, keys in SOURCES.items():
        chosen = None
        for key in keys:
            raw = (env.get(key) or "").strip()
            if not raw:
                continue
            try:
                chosen = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise SystemExit(
                    f"{key} is not valid JSON: {raw!r} ({exc}). Refusing to guess — "
                    f"a release routed to a runner that does not exist queues forever."
                )
            if not chosen:
                raise SystemExit(f"{key} resolved to an empty runs-on: {raw!r}")
            break
        out[platform] = HOSTED[platform] if chosen is None else chosen
    return out


def describe(env: dict[str, str], resolved: dict[str, object]) -> list[str]:
    """Human-readable routing summary — printed into the job log."""
    lines = []
    for platform, keys in SOURCES.items():
        src = next((k for k in keys if (env.get(k) or "").strip()), None)
        where = "LOCAL/override" if src else "GitHub-hosted (default)"
        lines.append(f"  {platform:14} {where:24} via {src or '—':16} -> {resolved[platform]}")
    return lines


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--github-output", action="store_true",
                    help="emit map=/runs_on_json= lines for $GITHUB_OUTPUT")
    args = ap.parse_args(argv)

    env = dict(os.environ)
    resolved = resolve(env)

    for line in describe(env, resolved):
        print(line, file=sys.stderr)

    if args.github_output:
        print("map=" + json.dumps(resolved))
        # The Namespace-profile step keys off how the macOS leg resolved.
        print("runs_on_json=" + json.dumps(resolved["darwin-arm64"]))
    else:
        print(json.dumps(resolved, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
