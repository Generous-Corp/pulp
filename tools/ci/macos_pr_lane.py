#!/usr/bin/env python3
"""Choose a pull request's macOS lane in build.yml: `full` or `contract`.

The `contract` lane configures and runs only the pr-fast and pr-guard tiers;
`full` builds and tests as every pull-request head did before ("macOS on a pull
request waits for approval" in docs/guides/local-ci.md). The rule:

- The repository variable PULP_MACOS_PR_GATING must be exactly `true`, or every
  event takes `full`. It stays off until the arm side refuses a `deferred` green
  durably, so an unset variable is today's behaviour.
- Only `pull_request` events can take `contract`.
- `synchronize` takes `contract`: a new head is unapproved, and the payload
  still lists the label the previous head earned.
- `macos-ci` counts only when TRUSTED_ACTOR applied it. On `labeled` that is
  the event's sender. On any other action it is the actor of the label's
  latest `labeled` event on the issue timeline; a timeline that cannot be read
  counts as untrusted.

Prints one JSON object, {"lane": ..., "reason": ...}, and exits 0. A lane that
cannot be decided is `contract` only when gating is on; otherwise `full`.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from typing import Callable, Optional

LABEL = "macos-ci"
TRUSTED_ACTOR = "shipyard-local[bot]"

# Returns the login that most recently applied LABEL, or None when it cannot
# be read.
ActorLookup = Callable[[], Optional[str]]


def decide(
    *,
    gating: str,
    event_name: str,
    action: str,
    label_name: str,
    sender: str,
    labels: list[str],
    label_actor: ActorLookup,
) -> tuple[str, str]:
    if gating.strip() != "true":
        return "full", "PULP_MACOS_PR_GATING is not `true`"
    if event_name != "pull_request":
        return "full", f"{event_name or 'this'} event is never gated"
    if action == "synchronize":
        return "contract", "a new head needs approval before the full lane"
    if action == "labeled" and label_name == LABEL:
        if sender == TRUSTED_ACTOR:
            return "full", f"{LABEL} applied by {TRUSTED_ACTOR}"
        return "contract", f"{LABEL} applied by {sender or 'an unknown actor'}, not {TRUSTED_ACTOR}"
    if LABEL not in labels:
        return "contract", f"the head does not carry {LABEL}"
    actor = label_actor()
    if actor is None:
        return "contract", f"could not read who applied {LABEL}"
    if actor == TRUSTED_ACTOR:
        return "full", f"{LABEL} applied by {TRUSTED_ACTOR}"
    return "contract", f"{LABEL} applied by {actor}, not {TRUSTED_ACTOR}"


def gh_label_actor(repo: str, number: str) -> Optional[str]:
    """Ask GitHub who last applied LABEL; None when the answer is unreadable."""
    query = f'.[] | select(.event == "labeled" and .label.name == "{LABEL}") | .actor.login'
    try:
        proc = subprocess.run(
            ["gh", "api", "--paginate", f"repos/{repo}/issues/{number}/events?per_page=100",
             "--jq", query],
            capture_output=True, text=True, encoding="utf-8", timeout=30, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    logins = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    return logins[-1] if logins else None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gating", default="")
    parser.add_argument("--event-name", default="")
    parser.add_argument("--action", default="")
    parser.add_argument("--label-name", default="")
    parser.add_argument("--sender", default="")
    parser.add_argument("--labels-json", default="[]",
                        help="JSON array of the pull request's label names")
    parser.add_argument("--repo", default="")
    parser.add_argument("--number", default="")
    args = parser.parse_args(argv)
    try:
        labels = [str(name) for name in json.loads(args.labels_json or "[]")]
    except (json.JSONDecodeError, TypeError):
        labels = []
    lane, reason = decide(
        gating=args.gating, event_name=args.event_name, action=args.action,
        label_name=args.label_name, sender=args.sender, labels=labels,
        label_actor=lambda: gh_label_actor(args.repo, args.number),
    )
    print(json.dumps({"lane": lane, "reason": reason}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
