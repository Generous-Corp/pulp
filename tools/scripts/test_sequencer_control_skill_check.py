#!/usr/bin/env python3
"""Calibrated positive and negative controls for sequencer_control_skill_check.

Every control here is a synthetic repository root the checker must reject, plus
one it must accept. A gate with only the accepting case proves that it runs, not
that it can fail -- and this gate's whole job is to fail when the timeline skill
stops describing the live sequencer surface.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sequencer_control_skill_check import SKILL_PATH, check  # noqa: E402

CAPABILITY_HEADER = (
    "// Define PULP_INSPECT_CAPABILITY(symbol, legacy_id, contract_id, risk,\n"
    "// side_effect, executor, evidence, observe, develop, grantable,\n"
    "// publication_bound) before including this file.\n\n"
)
STATE_READ = (
    'PULP_INSPECT_CAPABILITY(SequencerStateRead, "sequencer.state.read", '
    '"dev.pulp.sequencer/state.read@1", Sensitive, None, HostMain, Response, 1, 1, 1, 0)\n'
)
STATE_EDIT = (
    'PULP_INSPECT_CAPABILITY(SequencerStateEdit, "sequencer.state.edit", '
    '"dev.pulp.sequencer/state.edit@1", Control, State, HostMain, Receipt, 0, 1, 1, 0)\n'
)
# The same invocation reflowed after the open paren -- the shape clang-format
# produces for an over-long macro call, and the shape the parser could not see.
STATE_EDIT_WRAPPED = (
    "PULP_INSPECT_CAPABILITY(\n"
    '    SequencerStateEdit, "sequencer.state.edit",\n'
    '    "dev.pulp.sequencer/state.edit@1", Control, State, HostMain, Receipt, 0, 1, 1, 0)\n'
)
# An invocation the field regex cannot read: a named constant where it requires
# a boolean literal. The counter still sees it, which is the whole point.
STATE_EDIT_UNREADABLE = (
    'PULP_INSPECT_CAPABILITY(SequencerStateEdit, "sequencer.state.edit", '
    '"dev.pulp.sequencer/state.edit@1", Control, State, HostMain, Receipt, '
    "kObserveDenied, 1, 1, 0)\n"
)
UNRELATED = (
    'PULP_INSPECT_CAPABILITY(StateRead, "state.read", "dev.pulp.state/read@1", '
    "Sensitive, None, Background, Response, 1, 1, 1, 0)\n"
)

SKILL_ROW_READ = (
    "| `dev.pulp.sequencer/state.read@1` | `sequencer.state.read` | `response` |\n"
)
SKILL_ROW_EDIT = (
    "| `dev.pulp.sequencer/state.edit@1` | `sequencer.state.edit` | `receipt` |\n"
)

_TALLY = {"clean": 0, "calibrated": 0}


def manifest(*operations: str) -> str:
    """Wrap operation invocations in the registry array the parser bounds to."""
    body = "".join(f"        {line}\n" for line in operations)
    return (
        "#define PULP_OPERATION(symbol, slug, input, output, result)\n"
        "constexpr auto kControlOperations = std::array{\n"
        f"{body}"
        "    });\n"
        'constexpr auto kUnrelatedTail = "response");\n'
    )


OP_READ = 'PULP_OPERATION(SequencerStateRead, "sequencer/state.read", E, E, "response"),'
OP_EDIT = 'PULP_RECEIPT_OPERATION(SequencerStateEdit, "sequencer/state.edit", E, E),'


def build_root(base: Path, *, capabilities: str, manifest_text: str, skill: str) -> Path:
    root = Path(tempfile.mkdtemp(dir=base))
    (root / "inspect/include/pulp/inspect").mkdir(parents=True)
    (root / "inspect/include/pulp/inspect/capability_definitions.inc").write_text(
        capabilities, encoding="utf-8"
    )
    (root / "inspect/src").mkdir(parents=True)
    (root / "inspect/src/control_manifest.cpp").write_text(manifest_text, encoding="utf-8")
    skill_path = root / SKILL_PATH
    skill_path.parent.mkdir(parents=True)
    skill_path.write_text(skill, encoding="utf-8")
    return root


def expect_clean(label: str, root: Path) -> None:
    errors = check(root)
    if errors:
        raise AssertionError(f"{label}: expected no errors, got {errors}")
    _TALLY["clean"] += 1


def expect_rejected(label: str, root: Path, needle: str) -> None:
    errors = check(root)
    if not errors:
        raise AssertionError(f"{label}: expected a rejection, got none")
    if not any(needle in error for error in errors):
        raise AssertionError(f"{label}: expected {needle!r} among {errors}")
    _TALLY["calibrated"] += 1


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        good_caps = CAPABILITY_HEADER + UNRELATED + STATE_READ + STATE_EDIT
        good_manifest = manifest(OP_READ, OP_EDIT)
        good_skill = SKILL_ROW_READ + SKILL_ROW_EDIT

        expect_clean(
            "documented surface",
            build_root(
                base,
                capabilities=good_caps,
                manifest_text=good_manifest,
                skill=good_skill,
            ),
        )

        # A capability reflowed after the open paren is the same capability.
        # It used to read as absent, which left one sequencer symbol instead of
        # two -- not zero, so the emptiness guard stayed quiet while the gate
        # stopped requiring the skill to document the missing operation.
        expect_clean(
            "reflowed capability invocation",
            build_root(
                base,
                capabilities=CAPABILITY_HEADER + UNRELATED + STATE_READ + STATE_EDIT_WRAPPED,
                manifest_text=good_manifest,
                skill=good_skill,
            ),
        )

        # The general form of that failure, which no regex can be widened out
        # of: an invocation the field pattern cannot read at all. Comparing the
        # parse against the count the file spells is what turns a silently
        # narrowed inventory into a stated one.
        expect_rejected(
            "capability invocation the parser cannot read",
            build_root(
                base,
                capabilities=CAPABILITY_HEADER
                + UNRELATED
                + STATE_READ
                + STATE_EDIT_UNREADABLE,
                manifest_text=good_manifest,
                skill=good_skill,
            ),
            "the parser read",
        )

        expect_rejected(
            "skill omits an operation",
            build_root(
                base,
                capabilities=good_caps,
                manifest_text=good_manifest,
                skill=SKILL_ROW_READ,
            ),
            "does not document control operation",
        )

        expect_rejected(
            "skill records the wrong result kind",
            build_root(
                base,
                capabilities=good_caps,
                manifest_text=good_manifest,
                skill=SKILL_ROW_READ
                + "| `dev.pulp.sequencer/state.edit@1` | `sequencer.state.edit` | `response` |\n",
            ),
            "never beside",
        )

        expect_rejected(
            "skill names the operation without its gating capability",
            build_root(
                base,
                capabilities=good_caps,
                manifest_text=good_manifest,
                skill=SKILL_ROW_READ
                + "| `dev.pulp.sequencer/state.edit@1` | `receipt` |\n",
            ),
            "gating capability",
        )

        # Both halves of a whole-document search: the words `response` and
        # `receipt` are present somewhere, just never on the operation's row.
        expect_rejected(
            "result kind present elsewhere in the document",
            build_root(
                base,
                capabilities=good_caps,
                manifest_text=good_manifest,
                skill=SKILL_ROW_READ
                + "`receipt` appears in this sentence and nowhere useful.\n"
                + "| `dev.pulp.sequencer/state.edit@1` | `sequencer.state.edit` |\n",
            ),
            "never beside",
        )

        expect_rejected(
            "registry holds no sequencer capability",
            build_root(
                base,
                capabilities=CAPABILITY_HEADER + UNRELATED,
                manifest_text=manifest(OP_READ),
                skill=good_skill,
            ),
            "reading the wrong",
        )

        expect_rejected(
            "sequencer capability that no operation binds",
            build_root(
                base,
                capabilities=good_caps,
                manifest_text=manifest(
                    'PULP_OPERATION(StateRead, "state/read", E, E, "response"),'
                ),
                skill=good_skill,
            ),
            "unreachable through unified controls",
        )

    print(
        "sequencer control skill checker selftest: OK "
        f"({_TALLY['clean']} clean checks, {_TALLY['calibrated']} calibrated failures)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
