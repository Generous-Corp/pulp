#!/usr/bin/env python3
"""Generate the CI routing + landing digest inside CLAUDE.md.

The block between `<!-- generated:start id=ci-routing-digest -->` and its end
marker is rendered ONLY from inputs that change deliberately: the routing
contract (`runner_topology.json` lanes and overrides), the advertised-labels
snapshot, and fixed trap text. Workflow files are never read, so a
workflow-only PR (a new selector variable, a renamed workflow) cannot stale a
required ctest; those facts stay in `runner_topology_check.py --mode=static`. It is always in an agent's context, so a routing fact that
changes in the contract reaches the next session without anyone remembering to
edit prose.

It carries dates, never ages: an age computed at generation time would make the
block stale every morning and turn `--check` into a calendar.

    python3 tools/scripts/ci_routing_digest.py --check   # exit 1 when stale (ctest)
    python3 tools/scripts/ci_routing_digest.py --write   # regenerate the block
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))

import runner_topology_check as rtc  # noqa: E402
import runner_topology_static as st  # noqa: E402
from tools_registry_check import _splice  # noqa: E402

BLOCK_ID = "ci-routing-digest"
DOC = ROOT / "CLAUDE.md"


def _hosts(registrations: list[st.Registration]) -> str:
    return ", ".join(sorted({reg.host_id for reg in registrations}))


def _lane_line(lane: rtc.Lane, contract: rtc.Contract, snapshot: st.Snapshot) -> str:
    """One required lane, from the contract and snapshot alone.

    Workflow files are deliberately not read: a workflow-only PR must never
    stale this block. Only the event-class lane names its workflow in the
    contract, so only it gets a full reachability verdict here; every other
    self-hosted lane reports label supply, and `--mode=static` (its own ctest)
    adds the workflow-name match.
    """
    repo = rtc.DEFAULT_REPO
    kind = rtc.classify_target(lane.expect, contract)
    if kind == "sentinel":
        text = f"SENTINEL `{lane.expect}`"
    elif kind == "github-hosted":
        text = "HOSTED"
    elif kind != "self-hosted":
        text = "UNSERVED (neither self-hosted nor in the hosted allowlist)"
    elif (lane.supervisor or contract.static_default_supervisor) \
            not in contract.static_covered_supervisors:
        text = f"UNKNOWN (supervisor `{lane.supervisor}` is outside the snapshot)"
    else:
        projected, error = st._projections(lane, contract)
        if error:
            text = f"UNKNOWN ({error})"
        elif projected is not None:
            verdicts = {event: st.reach(labels, workflow, repo, snapshot)
                        for event, labels, workflow in projected}
            if all(v == st.REACHABLE for v, _ in verdicts.values()):
                hosts = _hosts([reg for _e, labels, _w in projected
                                for reg in st.label_carriers(labels, repo, snapshot)])
                text = f"REACHABLE on {', '.join(sorted(verdicts))} by {hosts}"
            else:
                bad = sorted(e for e, (v, _) in verdicts.items() if v != st.REACHABLE)
                text = f"UNSERVED on {', '.join(bad)}"
        else:
            carriers = st.label_carriers(lane.expect, repo, snapshot)
            text = (f"labels advertised by {_hosts(carriers)}" if carriers
                    else "UNSERVED (no registration advertises every label)")
    fallback = lane.unset_fallback
    if (rtc.classify_target(fallback, contract) == "self-hosted"
            and not st.label_carriers(fallback, repo, snapshot)):
        text += "; its unset fallback's labels are advertised by no registration"
    if lane.override_id:
        text += f"; override `{lane.override_id}`"
    return f"- `{lane.variable}`: {text}"


def render(contract: rtc.Contract, snapshot: st.Snapshot) -> str:
    commit = (snapshot.generated_from.get("commit") or "unknown")[:12]
    lines = [
        "Required routing lanes on DECLARED supply "
        f"(`tools/scripts/fleet_advertised_labels.json` @ `{commit}`), never live "
        "service. Workflow-name matching, advisory lanes, and undeclared selector "
        "variables: `runner_topology_check.py --mode=static`; live: `--mode=report`.",
        "",
    ]
    lines += [_lane_line(lane, contract, snapshot)
              for lane in contract.lanes if lane.severity == "required"]
    lines += ["", "Active routing overrides (`runner_topology.json` `overrides`; "
                  "an expired one fails every mode):"]
    overrides = contract.raw.get("overrides", []) or []
    if overrides:
        for o in overrides:
            lines.append(f"- `{o.get('id')}`: `{o.get('subject')}` = `{o.get('value')}`, "
                         f"owner {o.get('owner')}, since {o.get('since')}, "
                         f"expires {o.get('expires')}. Revert when: "
                         f"{o.get('revert_condition')}")
    else:
        lines.append("- none")
    lines += [
        "",
        "Landing API traps (verify with "
        "`decisions_contract.py --mode probe --live`, manual only, Shipyard >= 0.208.0):",
        "- REST `pulls/<n>.auto_merge` (and GraphQL `autoMergeRequest`) read null "
        "for a PR the merge queue already holds; read GraphQL `mergeQueueEntry`.",
        "- Classic branch protection omits a ruleset-based merge queue; query "
        "`repos/<o>/<r>/rulesets` before concluding there is no queue.",
        "",
        "GENERATED by `tools/scripts/ci_routing_digest.py --write`. Do not edit by hand.",
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="regenerate the block")
    mode.add_argument("--check", action="store_true", help="fail when stale (default)")
    ap.add_argument("--doc", type=Path, default=DOC)
    ap.add_argument("--contract", type=Path, default=rtc.DEFAULT_CONTRACT)
    args = ap.parse_args(argv)

    contract = rtc.load_contract(args.contract)
    snapshot = st.load_snapshot(args.contract.resolve().parent / contract.static_snapshot)
    block = render(contract, snapshot)
    text = args.doc.read_text(encoding="utf-8")
    spliced = _splice(text, block, BLOCK_ID)
    if spliced is None:
        print(f"ERROR: {args.doc.name} is missing <!-- generated:start id={BLOCK_ID} --> / "
              f"<!-- generated:end id={BLOCK_ID} -->", file=sys.stderr)
        return 1
    if args.write:
        if spliced != text:
            args.doc.write_text(spliced, encoding="utf-8")
            print(f"wrote the {BLOCK_ID} block in {args.doc.name}")
        else:
            print(f"{BLOCK_ID} block already up to date")
        return 0
    if spliced != text:
        print(f"ERROR: the {BLOCK_ID} block in {args.doc.name} is stale or hand-edited. "
              "Regenerate it: python3 tools/scripts/ci_routing_digest.py --write",
              file=sys.stderr)
        return 1
    print(f"{BLOCK_ID} block in sync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
