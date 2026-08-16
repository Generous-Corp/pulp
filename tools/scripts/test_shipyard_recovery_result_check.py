#!/usr/bin/env python3
"""Contract tests for the fenced recovery-result validator."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import shipyard_recovery_result_check as checker  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
TRIAGE_SCHEMA = json.loads(
    (REPO_ROOT / "tools/shipyard/recovery-result.schema.json").read_text()
)
REPAIR_SCHEMA = json.loads(
    (REPO_ROOT / "tools/shipyard/repair-result.schema.json").read_text()
)

VALID_TRIAGE = {
    "classification": "needs_sol_fix",
    "summary": "one blocker",
    "evidence": ["compile error"],
    "next_action": "remove the assertion",
}
VALID_REPAIR = {"outcome": "fixed", "summary": "removed it", "tests": ["ctest"]}


def _write(payload: object) -> Path:
    handle = tempfile.NamedTemporaryFile(
        "w", suffix=".json", delete=False, encoding="utf-8"
    )
    json.dump(payload, handle)
    handle.close()
    return Path(handle.name)


class ResultCheckTests(unittest.TestCase):
    def test_accepts_the_bare_structured_object(self) -> None:
        checker.validate(VALID_TRIAGE, TRIAGE_SCHEMA)
        checker.validate(VALID_REPAIR, REPAIR_SCHEMA)

    def test_extracts_the_claude_envelope_structured_output(self) -> None:
        path = _write(
            {
                "type": "result",
                "usage": {"input_tokens": 1},
                "result": json.dumps(VALID_TRIAGE),
                "structured_output": VALID_TRIAGE,
            }
        )
        self.assertEqual(checker.load_structured_output(path), VALID_TRIAGE)

    def test_falls_back_to_the_encoded_result_string(self) -> None:
        path = _write(
            {
                "type": "result",
                "usage": {"input_tokens": 1},
                "result": json.dumps(VALID_REPAIR),
            }
        )
        self.assertEqual(checker.load_structured_output(path), VALID_REPAIR)

    def test_rejects_a_refused_or_empty_structured_output(self) -> None:
        path = _write({"type": "result", "usage": {}, "structured_output": None})
        with self.assertRaises(checker.ResultError):
            checker.load_structured_output(path)

    def test_rejects_an_out_of_enum_classification(self) -> None:
        payload = dict(VALID_TRIAGE, classification="please_merge")
        with self.assertRaises(checker.ResultError):
            checker.validate(payload, TRIAGE_SCHEMA)

    def test_rejects_a_missing_required_field(self) -> None:
        payload = dict(VALID_TRIAGE)
        del payload["next_action"]
        with self.assertRaises(checker.ResultError):
            checker.validate(payload, TRIAGE_SCHEMA)

    def test_rejects_smuggled_additional_fields(self) -> None:
        payload = dict(VALID_TRIAGE, push_to_main=True)
        with self.assertRaises(checker.ResultError):
            checker.validate(payload, TRIAGE_SCHEMA)

    def test_enforces_declared_bounds(self) -> None:
        with self.assertRaises(checker.ResultError):
            checker.validate(dict(VALID_TRIAGE, summary=""), TRIAGE_SCHEMA)
        with self.assertRaises(checker.ResultError):
            checker.validate(dict(VALID_TRIAGE, summary="x" * 1201), TRIAGE_SCHEMA)
        with self.assertRaises(checker.ResultError):
            checker.validate(dict(VALID_TRIAGE, evidence=["e"] * 9), TRIAGE_SCHEMA)
        with self.assertRaises(checker.ResultError):
            checker.validate(dict(VALID_REPAIR, tests=["t"] * 13), REPAIR_SCHEMA)

    def test_rejects_wrong_json_types(self) -> None:
        with self.assertRaises(checker.ResultError):
            checker.validate(dict(VALID_TRIAGE, evidence="not-a-list"), TRIAGE_SCHEMA)
        with self.assertRaises(checker.ResultError):
            checker.validate(dict(VALID_TRIAGE, summary=7), TRIAGE_SCHEMA)
        with self.assertRaises(checker.ResultError):
            checker.validate([], TRIAGE_SCHEMA)

    def test_cli_normalizes_the_envelope_in_place(self) -> None:
        source = _write(
            {
                "type": "result",
                "usage": {"input_tokens": 1},
                "result": json.dumps(VALID_TRIAGE),
                "structured_output": VALID_TRIAGE,
            }
        )
        destination = source.with_suffix(".out.json")
        exit_code = checker.main(
            [
                "--schema",
                str(REPO_ROOT / "tools/shipyard/recovery-result.schema.json"),
                "--result",
                str(source),
                "--output",
                str(destination),
            ]
        )
        self.assertEqual(exit_code, 0)
        self.assertEqual(json.loads(destination.read_text()), VALID_TRIAGE)

    def test_a_rejected_envelope_explains_itself(self) -> None:
        """A bare "not valid JSON" is undiagnosable after the fact, because the
        raw envelope is excluded from the published artifact by design. The
        rejection must carry the envelope's own status fields and a bounded
        excerpt — this cost a full canary cycle on 2026-08-16."""
        path = _write(
            {
                "type": "result",
                "usage": {"input_tokens": 1},
                "subtype": "success",
                "stop_reason": "end_turn",
                "result": "I cannot complete this request.",
            }
        )
        with self.assertRaises(checker.ResultError) as caught:
            checker.load_structured_output(path)
        message = str(caught.exception)
        self.assertIn("stop_reason", message)
        self.assertIn("I cannot complete this request.", message)
        self.assertIn("keys=", message)

    def test_an_error_envelope_is_named_as_such(self) -> None:
        path = _write(
            {
                "type": "result",
                "usage": {},
                "is_error": True,
                "subtype": "error_during_execution",
                "result": "upstream refused",
            }
        )
        with self.assertRaises(checker.ResultError) as caught:
            checker.load_structured_output(path)
        self.assertIn("error envelope", str(caught.exception))
        self.assertIn("error_during_execution", str(caught.exception))

    def test_the_digest_never_carries_the_whole_result(self) -> None:
        path = _write(
            {
                "type": "result",
                "usage": {},
                "result": "x" * 5000,
            }
        )
        with self.assertRaises(checker.ResultError) as caught:
            checker.load_structured_output(path)
        self.assertLess(len(str(caught.exception)), 900)

    def test_the_validator_is_inside_the_recovery_fence(self) -> None:
        # A validator outside the fence could be weakened by the very model it
        # constrains, so its own path must match a forbidden prefix.
        import shipyard_recovery_repair

        relative = "tools/scripts/shipyard_recovery_result_check.py"
        self.assertTrue(
            any(
                relative.startswith(prefix)
                for prefix in shipyard_recovery_repair.FORBIDDEN_PREFIXES
            )
        )


if __name__ == "__main__":
    unittest.main()
