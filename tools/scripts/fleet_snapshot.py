#!/usr/bin/env python3
"""Regenerate or verify the checked-in advertised-labels snapshot.

`tools/scripts/fleet_advertised_labels.json` is what `runner_topology_check.py
--mode=static` judges every lane against. It is derived from the fleet's
checked-in macOS profiles by tartci's own generator, so this script owns no
label rule and no host list: it discovers profiles by glob in a tartci
checkout, asks tartci to render them, and compares.

    python3 tools/scripts/fleet_snapshot.py --tartci ../tartci --write
    python3 tools/scripts/fleet_snapshot.py --tartci ../tartci --check

Adding or removing a machine is a new or deleted `profiles/*-macos-fleet.toml`
in tartci; nothing in Pulp names a host. `--check` compares registrations only
(provenance metadata is expected to move) and exits 1 naming every host and
lane added or removed, 2 when the tartci checkout or its generator cannot be
read. The hourly runner-topology workflow runs `--check` against a fresh
tartci clone so a stale snapshot surfaces as a tracked finding.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SNAPSHOT = HERE / "fleet_advertised_labels.json"
PROFILE_GLOB = "profiles/*-macos-fleet.toml"
GENERATOR = "scripts/macos_fleet_lanes.py"
REGENERATE = (f"python3 tools/scripts/fleet_snapshot.py --tartci <tartci checkout> --write "
              f"(tartci: python3 {GENERATOR} advertised-labels {PROFILE_GLOB} --json)")


class Unreadable(Exception):
    """The tartci checkout or its generator could not produce a snapshot."""


def discover_profiles(tartci: Path) -> list[Path]:
    profiles = sorted(tartci.glob(PROFILE_GLOB))
    if not profiles:
        raise Unreadable(f"no {PROFILE_GLOB} under {tartci}")
    return profiles


def generate(tartci: Path) -> dict:
    generator = tartci / GENERATOR
    if not generator.is_file():
        raise Unreadable(f"{generator} is missing; this tartci predates advertised-labels")
    profiles = discover_profiles(tartci)
    try:
        proc = subprocess.run(
            [sys.executable, str(generator), "advertised-labels",
             *[str(p) for p in profiles], "--json"],
            capture_output=True, text=True, cwd=tartci, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        raise Unreadable(f"generator failed to run: {exc}") from exc
    if proc.returncode != 0:
        raise Unreadable(f"generator exited {proc.returncode}: {proc.stderr.strip()[:400]}")
    try:
        snapshot = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise Unreadable(f"generator emitted unparseable JSON: {exc}") from exc
    if not isinstance(snapshot.get("registrations"), list):
        raise Unreadable("generator output has no registrations array")
    snapshot.setdefault("generated_from", {})["regenerate"] = REGENERATE
    return snapshot


def _key(reg: dict) -> tuple:
    return (reg.get("host_id"), reg.get("lane"), reg.get("class_label"))


def compare(current: dict, fresh: dict) -> list[str]:
    """Human-readable differences in registrations; empty when in sync."""
    have = {_key(r): r for r in current.get("registrations", [])}
    want = {_key(r): r for r in fresh.get("registrations", [])}
    problems: list[str] = []
    old_hosts = {k[0] for k in have}
    new_hosts = {k[0] for k in want}
    for host in sorted(new_hosts - old_hosts, key=str):
        problems.append(f"host added upstream: {host}")
    for host in sorted(old_hosts - new_hosts, key=str):
        problems.append(f"host removed upstream: {host}")
    for key in sorted(want.keys() - have.keys(), key=str):
        if key[0] in old_hosts:
            problems.append(f"lane added upstream: {key[0]}:{key[1]}"
                            + (f"/{key[2]}" if key[2] else ""))
    for key in sorted(have.keys() - want.keys(), key=str):
        if key[0] in new_hosts:
            problems.append(f"lane removed upstream: {key[0]}:{key[1]}"
                            + (f"/{key[2]}" if key[2] else ""))
    for key in sorted(have.keys() & want.keys(), key=str):
        if have[key] != want[key]:
            fields = sorted(f for f in set(have[key]) | set(want[key])
                            if have[key].get(f) != want[key].get(f))
            problems.append(f"registration changed: {key[0]}:{key[1]}"
                            + (f"/{key[2]}" if key[2] else "") + f" ({', '.join(fields)})")
    if not problems and current.get("registrations") != fresh.get("registrations"):
        problems.append("registration order changed")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="regenerate the snapshot")
    mode.add_argument("--check", action="store_true",
                      help="exit 1 when the snapshot's registrations are stale")
    ap.add_argument("--tartci", type=Path, required=True, help="a tartci checkout")
    ap.add_argument("--snapshot", type=Path, default=DEFAULT_SNAPSHOT)
    args = ap.parse_args(argv)

    try:
        fresh = generate(args.tartci.resolve())
    except Unreadable as exc:
        print(f"fleet-snapshot: UNREADABLE: {exc}", file=sys.stderr)
        return 2
    if args.write:
        args.snapshot.write_text(json.dumps(fresh, indent=2) + "\n")
        hosts = sorted({r["host_id"] for r in fresh["registrations"]})
        print(f"fleet-snapshot: wrote {len(fresh['registrations'])} registration(s) "
              f"across {len(hosts)} host(s): {', '.join(hosts)}")
        return 0
    try:
        current = json.loads(args.snapshot.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"fleet-snapshot: UNREADABLE: {args.snapshot}: {exc}", file=sys.stderr)
        return 2
    problems = compare(current, fresh)
    commit = (fresh.get("generated_from") or {}).get("commit") or "unknown"
    if problems:
        print(f"fleet-snapshot: STALE against tartci {commit}:")
        for problem in problems:
            print(f"  {problem}")
        print("Regenerate: " + REGENERATE)
        return 1
    print(f"fleet-snapshot: OK — registrations match tartci {commit}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
