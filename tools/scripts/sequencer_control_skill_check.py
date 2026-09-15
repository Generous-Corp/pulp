#!/usr/bin/env python3
"""Keep the timeline skill honest about the live sequencer control surface.

The sequencer reaches agents through four surfaces, and three of them already
have a gate: the capability registry is frozen by a digest, the human catalog
`docs/reference/development-inspector-capabilities.md` is generated and
enforced by `inspector_truth_check.py`, and `pulp control capabilities` prints
the registry offline. The fourth has none. `.agents/skills/timeline/SKILL.md`
is what an agent actually reads before touching sequencer work, and nothing
compelled it to mention that a running host's step grid is reachable at all --
so a sequencer capability could ship, be registered, be documented in the
reference catalog, and still be invisible to every agent that consults the
skill for which surface to use.

This check closes that by deriving the answer instead of restating it: every
operation in the frozen registry whose gating capability is a
`dev.pulp.sequencer/` contract must appear in the skill with its exact
operation id and its registry-declared result kind. Adding a sequencer
operation without telling the skill fails here.

Parsing is delegated to `inspector_truth_check` so the registry has exactly one
reader. Exit 0 when the skill covers the registry, 1 when it does not.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from inspector_truth_check import (  # noqa: E402
    CAPABILITY_DEFINITIONS_PATH,
    CONTROL_MANIFEST_PATH,
    parse_capability_definitions,
    parse_control_operations,
)

SKILL_PATH = ".agents/skills/timeline/SKILL.md"
SEQUENCER_CONTRACT_PREFIX = "dev.pulp.sequencer/"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def sequencer_operations(root: Path):
    """Return the registry operations gated on a sequencer capability.

    The gating capability is the authority, not the operation slug: an
    operation that reads sequencer state through some other contract is that
    contract's business, and a sequencer contract that grew a second operation
    is caught here without editing this list.
    """
    definitions = parse_capability_definitions(
        (root / CAPABILITY_DEFINITIONS_PATH).read_text(encoding="utf-8")
    )
    sequencer_symbols = {
        definition.symbol: definition
        for definition in definitions
        if definition.contract_id.startswith(SEQUENCER_CONTRACT_PREFIX)
    }
    operations = parse_control_operations(
        (root / CONTROL_MANIFEST_PATH).read_text(encoding="utf-8")
    )
    return [
        (operation, sequencer_symbols[operation.capability_symbol])
        for operation in operations
        if operation.capability_symbol in sequencer_symbols
    ], sequencer_symbols


def check(root: Path) -> list[str]:
    errors: list[str] = []
    paired, sequencer_symbols = sequencer_operations(root)

    # A zero here would pass every assertion below while proving nothing, so it
    # is itself the failure: the registry is the control, and an empty one
    # means this check is reading the wrong file rather than that the sequencer
    # has no live surface.
    if not sequencer_symbols:
        errors.append(
            f"no `{SEQUENCER_CONTRACT_PREFIX}` capability found in "
            f"{CAPABILITY_DEFINITIONS_PATH}; this check is reading the wrong "
            "inventory rather than reporting an empty sequencer surface"
        )
        return errors
    if not paired:
        errors.append(
            f"{len(sequencer_symbols)} sequencer capabilities are declared but no "
            f"operation in {CONTROL_MANIFEST_PATH} binds one; a capability with "
            "no operation is unreachable through unified controls"
        )
        return errors

    skill_lines = (root / SKILL_PATH).read_text(encoding="utf-8").splitlines()
    for operation, definition in paired:
        # The result kind must be bound to its own operation, not merely
        # present in the document. A skill that names five operations already
        # contains the words `response` and `receipt` somewhere, so a
        # whole-document search passes no matter which operation the registry
        # moved -- it grades the vocabulary rather than the claim. Requiring
        # both on one line makes the table row the assertion.
        mentions = [line for line in skill_lines if f"`{operation.operation_id}`" in line]
        if not mentions:
            errors.append(
                f"{SKILL_PATH} does not document control operation "
                f"`{operation.operation_id}` (capability `{definition.contract_id}`); "
                "an agent reading the skill cannot discover it"
            )
            continue
        if operation.result_kind == "unknown":
            errors.append(
                f"control operation `{operation.operation_id}` has no readable "
                f"result kind in {CONTROL_MANIFEST_PATH}"
            )
            continue
        if not any(f"`{operation.result_kind}`" in line for line in mentions):
            errors.append(
                f"{SKILL_PATH} names `{operation.operation_id}` but never beside "
                f"its registry result kind `{operation.result_kind}`"
            )
        if not any(f"`{definition.legacy_id}`" in line for line in mentions):
            errors.append(
                f"{SKILL_PATH} names `{operation.operation_id}` but never beside "
                f"its gating capability `{definition.legacy_id}`"
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=None, help="repository root to check")
    arguments = parser.parse_args()
    root = Path(arguments.root).resolve() if arguments.root else repo_root()
    errors = check(root)
    for error in errors:
        print(f"sequencer-control-skill: {error}", file=sys.stderr)
    if errors:
        print(
            "sequencer-control-skill: document each operation in "
            f"{SKILL_PATH} with its exact operation id and result kind",
            file=sys.stderr,
        )
        return 1
    print(
        "sequencer-control-skill: the timeline skill covers every "
        "sequencer-gated control operation"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
