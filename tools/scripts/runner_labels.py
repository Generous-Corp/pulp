#!/usr/bin/env python3
"""Resolve and validate the label set an ephemeral runner supervisor registers.

GitHub selects a runner only when it carries EVERY label the job requests. So a
supervisor that registers a label set no routing lane asks for produces a runner
that is online, idle, and permanently unreachable: the operator sees healthy free
capacity while jobs queue forever against a lane that runner cannot serve. There
is no error anywhere — not in the runner log, not in the job, not in the API.
That silence is the whole failure mode, and it is why this resolver is fail
closed. A supervisor that cannot determine a selectable label set must refuse to
register rather than contribute a runner that only looks like capacity.

The contract it validates against is `runner_topology.json`, the same reviewed
artifact `runner_topology_check.py` reconciles the live fleet against. Nothing
here invents routing: a label set is acceptable exactly when some declared lane
for the platform could select it.

Host labels (`pulp-host-*`) pin an ephemeral lane to one machine, and the Linux
and Windows lanes all carry one — which is what makes a host-label-less default
unselectable. The tag is taken from an explicit operator declaration
(`--host-tag` / `PULP_RUNNER_HOST_TAG`), or from `shipyard runner tag` ONLY when
that value names a host label some lane actually declares. Shipyard's tag is a
runner-NAMING tag (`<repo>-<tag>-NN`) from a neighbouring vocabulary, not a
routing label: it answers `studio` on the Mac Studio, whose routing label is
`pulp-host-macstudio`. Accepting only exact matches keeps `m1`/`m5` automatic
without guessing a `studio` -> `macstudio` rule this repo nowhere states.

Usage:
    runner_labels.py --platform linux --labels self-hosted,Linux,ARM64,pulp-build-linux
    runner_labels.py --platform windows --labels ... --host-tag m5

Prints the resolved comma-separated label set on stdout and exits 0. On an
unresolvable set it explains the gap on stderr and exits 2.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_CONTRACT = HERE / "runner_topology.json"

HOST_PREFIX = "pulp-host-"
SELF_HOSTED = "self-hosted"

# The OS label each platform is identified by inside a lane's label set. GitHub
# matches labels case-insensitively, and the repo spells them inconsistently
# (`macos` in a supervisor default, `macOS` in a lane), so all comparisons here
# are casefolded.
PLATFORM_LABEL = {"linux": "linux", "windows": "windows", "macos": "macos"}

OK = 0
UNRESOLVABLE = 2


def _fold(labels) -> set[str]:
    return {str(x).casefold() for x in labels}


def load_lanes(contract: Path) -> list[dict]:
    """Self-hosted lanes from the routing contract, as {variable, expect, supervisor}."""
    data = json.loads(contract.read_text())
    lanes = []
    for raw in data.get("lanes", []):
        expect = raw.get("expect")
        if isinstance(expect, list) and SELF_HOSTED in expect:
            lanes.append({
                "variable": raw["variable"],
                "expect": expect,
                # Carried through because routing depends on it: which supervisor
                # provisions a lane decides which host labels it can register,
                # and a lane stripped of that reads as launchd-supervised.
                "supervisor": raw.get("supervisor", DEFAULT_SUPERVISOR),
            })
    return lanes


def lanes_for(platform: str, lanes: list[dict]) -> list[dict]:
    """Lanes whose label set names this platform's OS label."""
    want = PLATFORM_LABEL[platform]
    return [ln for ln in lanes if want in _fold(ln["expect"])]


# The supervisor a lane's runners are provisioned by. Until an x86_64 Proxmox
# machine joined the fleet, every self-hosted lane was served by a launchd-managed
# supervisor on an Apple-Silicon Mac (Tart for Linux, QEMU for Windows), so a lane
# that names no supervisor still means that one.
DEFAULT_SUPERVISOR = "launchd"


def lane_supervisor(lane: dict) -> str:
    """Which supervisor provisions this lane's runners."""
    return lane.get("supervisor", DEFAULT_SUPERVISOR)


def known_host_labels(lanes: list[dict], supervisor: str | None = None) -> set[str]:
    """Every `pulp-host-*` label any declared lane routes to.

    Derived from the contract rather than listed here, so adding a machine is a
    lane edit — the same reviewed artifact that authorizes the routing — and
    never a second table that can drift out of step with it.

    `supervisor` narrows the answer to hosts one supervisor actually provisions.
    That distinction only started to matter once the fleet held more than one:
    the launchd/Tart supervisor registers ARM64 runners because it runs on Apple
    Silicon, so asking whether its defaults route on an x86_64 Proxmox host asks
    a question neither side ever claimed — and answers it "broken".
    """
    return {lbl for ln in lanes
            if supervisor is None or lane_supervisor(ln) == supervisor
            for lbl in ln["expect"]
            if lbl.casefold().startswith(HOST_PREFIX)}


def selecting_lanes(labels, lanes: list[dict]) -> list[dict]:
    """Lanes a runner with these labels could serve (GitHub's ALL-labels rule)."""
    have = _fold(labels)
    return [ln for ln in lanes if _fold(ln["expect"]) <= have]


def shipyard_tag() -> str | None:
    """This box's Shipyard runner tag, or None when Shipyard cannot answer."""
    if not shutil.which("shipyard"):
        return None
    try:
        out = subprocess.run(["shipyard", "runner", "tag"], capture_output=True,
                             text=True, timeout=15, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    tag = out.stdout.strip() if out.returncode == 0 else ""
    return tag or None


def resolve(platform: str, labels: list[str], host_tag: str | None,
            lanes: list[dict], probe_shipyard=shipyard_tag):
    """Append a host label when one can be determined authoritatively.

    Returns (labels, note). `note` records how the tag was obtained (or why it
    could not be), for the error message when validation then fails.
    """
    if any(lbl.casefold().startswith(HOST_PREFIX) for lbl in labels):
        return labels, "host label already present in the requested set"

    known = known_host_labels(lanes)
    if host_tag:
        want = f"{HOST_PREFIX}{host_tag}"
        if want.casefold() not in _fold(known):
            valid = ", ".join(sorted(h[len(HOST_PREFIX):] for h in known)) or "(none)"
            return labels, (f"declared host tag '{host_tag}' has no lane: no "
                            f"declared lane routes to '{want}' (declared: {valid})")
        return labels + [want], f"host label from an explicit declaration: {want}"

    tag = probe_shipyard()
    if tag is None:
        return labels, "no host tag declared and `shipyard runner tag` unavailable"
    want = f"{HOST_PREFIX}{tag}"
    if want.casefold() in _fold(known):
        return labels + [want], f"host label from `shipyard runner tag`: {want}"
    return labels, (
        f"`shipyard runner tag` says '{tag}', but no declared lane routes to "
        f"'{want}'. Shipyard's tag names runners (`<repo>-{tag}-NN`); it is a "
        f"different vocabulary from the routing label and is not assumed to map "
        f"onto one")


def explain(platform: str, labels: list[str], note: str,
            platform_lanes: list[dict], lanes: list[dict]) -> str:
    known = sorted(h[len(HOST_PREFIX):] for h in known_host_labels(platform_lanes))
    lines = [
        f"no declared {platform} lane can select a runner with these labels:",
        f"    {','.join(labels)}",
        f"  ({note})",
        "",
        "GitHub selects a runner only when it carries EVERY requested label, so",
        "registering this set yields a runner that is online, idle, and can never",
        "be picked — while jobs queue against the lanes below.",
        "",
        f"Declared {platform} lanes ({DEFAULT_CONTRACT.name}):",
    ]
    have = _fold(labels)
    for ln in platform_lanes:
        missing = sorted(x for x in ln["expect"] if x.casefold() not in have)
        lines.append(f"    {ln['variable']}")
        lines.append(f"        wants   {','.join(ln['expect'])}")
        lines.append(f"        missing {','.join(missing) or '(nothing)'}")
    if not platform_lanes:
        lines.append("    (none — the contract declares no lane for this platform)")
    lines += [
        "",
        "Fix one of:",
        f"  - declare this machine's host tag:  PULP_RUNNER_HOST_TAG=<tag>"
        f"  (declared: {', '.join(known) or 'none'})",
        "    or pass --host-tag <tag>",
        "  - pass a complete label set with --labels / PULP_RUNNER_LABELS",
        f"  - if this is genuinely new routing, add the lane to "
        f"tools/scripts/{DEFAULT_CONTRACT.name} first — it is the reviewed",
        "    artifact the live-fleet checker reconciles against",
    ]
    return "\n".join(lines)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--platform", required=True, choices=sorted(PLATFORM_LABEL))
    p.add_argument("--labels", required=True,
                   help="comma-separated label set the supervisor would register")
    p.add_argument("--host-tag", default=os.environ.get("PULP_RUNNER_HOST_TAG") or None)
    p.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT)
    args = p.parse_args(argv)

    labels = [x.strip() for x in args.labels.split(",") if x.strip()]
    if not labels:
        print("error: --labels is empty", file=sys.stderr)
        return UNRESOLVABLE

    lanes = load_lanes(args.contract)
    platform_lanes = lanes_for(args.platform, lanes)
    resolved, note = resolve(args.platform, labels, args.host_tag, platform_lanes)

    if not selecting_lanes(resolved, platform_lanes):
        print("✗ " + explain(args.platform, resolved, note, platform_lanes, lanes),
              file=sys.stderr)
        return UNRESOLVABLE

    print(",".join(resolved))
    return OK


if __name__ == "__main__":
    sys.exit(main())
