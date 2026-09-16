#!/usr/bin/env python3
"""Keep the timeline skill honest about the durable command vocabulary.

`core/timeline/schema/timeline_cli_verbs.json` is the manifest-derived
definition of every Timeline verb, and its `Command` domain is the exact set of
durable document mutations a caller may author -- through `pulp seq apply`, the
`pulp_timeline_command_apply` MCP tool, or `DocumentSession` directly. Three
gates already hold that artifact still: `timeline-schema-cli-drift` byte-checks
it against the manifest, `timeline-schema-cli-selftest` proves the emitter, and
the decoder refuses a wire type the manifest does not carry.

None of them require anyone to be told a command exists. `skill_sync_check.py`
asks only that `.agents/skills/timeline/SKILL.md` *changed* when timeline paths
did, which a note about something else satisfies -- so a command could be
declared, emitted, decodable, reachable from two user-facing surfaces, and
named on no page an agent reads.

This check closes that by deriving the answer instead of restating it: every
`Command` verb in the manifest-derived artifact must appear in the skill beside
the exact wire `type` a caller writes. Naming the C++ spelling alone is not
enough, because the spelling is what an agent already guesses and the wire type
is what a document actually carries. Adding a command without telling the skill
fails here.

Exit 0 when the skill covers the artifact, 1 when it does not.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

SKILL_PATH = ".agents/skills/timeline/SKILL.md"
VERBS_PATH = "core/timeline/schema/timeline_cli_verbs.json"
COMMAND_HEADER_PATH = "core/timeline/include/pulp/timeline/command.hpp"
COMMAND_DOMAIN = "Command"
COMMAND_TYPE_PREFIX = "pulp.timeline.command."


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def pascal_case(snake: str) -> str:
    return "".join(part.capitalize() for part in snake.split("_"))


def command_verbs(root: Path) -> list[tuple[str, str]]:
    """Return `(declared name, wire type)` for every Command-domain verb.

    The declared name is derived from the wire type rather than read from a
    second list, so the two cannot drift apart: a hand-maintained table of
    spellings is one more thing to forget alongside the docs this check exists
    to defend.
    """
    artifact = json.loads((root / VERBS_PATH).read_text(encoding="utf-8"))
    pairs: list[tuple[str, str]] = []
    for verb in artifact.get("verbs", []):
        if verb.get("domain") != COMMAND_DOMAIN:
            continue
        schema_type = verb.get("schema_type", "")
        if not schema_type.startswith(COMMAND_TYPE_PREFIX):
            continue
        pairs.append((pascal_case(schema_type[len(COMMAND_TYPE_PREFIX):]), schema_type))
    return sorted(pairs)


def declared_structs(root: Path) -> set[str]:
    header = (root / COMMAND_HEADER_PATH).read_text(encoding="utf-8")
    return set(re.findall(r"^struct (\w+)", header, re.MULTILINE))


def check(root: Path) -> list[str]:
    errors: list[str] = []
    pairs = command_verbs(root)

    # A zero here would pass every assertion below while proving nothing, so it
    # is itself the failure: the artifact is the control, and an empty one means
    # this check is reading the wrong file rather than that the document model
    # has no mutation vocabulary.
    if not pairs:
        errors.append(
            f"no `{COMMAND_DOMAIN}` verb found in {VERBS_PATH}; this check is "
            "reading the wrong artifact rather than reporting an empty command "
            "vocabulary"
        )
        return errors

    # The derivation is checked against the header rather than trusted. A
    # spelling rule that silently stopped matching the real type names would
    # otherwise turn this gate into a search for words that exist nowhere,
    # which fails loudly for the wrong reason and sends the reader to the docs.
    structs = declared_structs(root)
    if not structs:
        errors.append(
            f"no `struct` declaration parsed from {COMMAND_HEADER_PATH}; the "
            "name derivation has no control to check against"
        )
        return errors
    undeclared = [name for name, _ in pairs if name not in structs]
    if undeclared:
        errors.append(
            "derived command names are absent from "
            f"{COMMAND_HEADER_PATH}: {', '.join(undeclared)}; the wire-type to "
            "type-name spelling rule in this check no longer matches the model"
        )
        return errors

    skill_lines = (root / SKILL_PATH).read_text(encoding="utf-8").splitlines()
    for name, schema_type in pairs:
        # Both tokens must sit on one line. A whole-document search would pass
        # on a skill that names five commands and carries the prefix
        # `pulp.timeline.command.` somewhere else entirely -- it grades the
        # vocabulary rather than the claim. Requiring the pair on one line
        # makes the catalog row the assertion.
        mentions = [line for line in skill_lines if f"`{name}`" in line]
        if not mentions:
            errors.append(
                f"{SKILL_PATH} does not document command `{name}` "
                f"(wire type `{schema_type}`); an agent reading the skill "
                "cannot discover it"
            )
            continue
        if not any(f"`{schema_type}`" in line for line in mentions):
            errors.append(
                f"{SKILL_PATH} names `{name}` but never beside its wire type "
                f"`{schema_type}`, which is what a caller writes into a command "
                "document"
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=None, help="repository root to check")
    arguments = parser.parse_args()
    root = Path(arguments.root).resolve() if arguments.root else repo_root()
    errors = check(root)
    for error in errors:
        print(f"timeline-command-doc: {error}", file=sys.stderr)
    if errors:
        print(
            "timeline-command-doc: document each command in "
            f"{SKILL_PATH} with its exact name and wire type",
            file=sys.stderr,
        )
        return 1
    print(
        "timeline-command-doc: the timeline skill covers every "
        f"{COMMAND_DOMAIN}-domain verb in {VERBS_PATH}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
