#!/usr/bin/env python3
"""Focused tests for the friction-report exposure contract."""

from __future__ import annotations

import unittest

from friction_report_exposure_check import validate_text


def report(exposure: str, mechanism: str, proof: str, role: str) -> str:
    return f"""# Incident

## Orchestrator independence
**Public exposure:** {exposure}
**Evidence:** exact direct-path reproduction or exact orchestrator-only boundary
**Public mechanism:** {mechanism}
**Public proof:** {proof}
**Orchestrator role:** {role}

## Routing
owner
"""


class FrictionReportExposureTests(unittest.TestCase):
    def test_exposed_requires_public_mechanism_and_proof(self) -> None:
        self.assertEqual(
            validate_text(report(
                "EXPOSED",
                "core/process/browser_lifecycle.cpp",
                "test_browser_owner_death",
                "optional defense-in-depth for governed cancellation",
            )),
            [],
        )
        errors = validate_text(report(
            "EXPOSED", "N/A — Shipyard cleans it", "N/A — covered by Shipyard",
            "primary Shipyard fix",
        ))
        self.assertTrue(any("public Pulp/Forge mechanism" in e for e in errors), errors)
        self.assertTrue(any("public-path proof" in e for e in errors), errors)
        self.assertTrue(any("defense-in-depth" in e for e in errors), errors)

    def test_not_exposed_requires_evidenced_na(self) -> None:
        self.assertEqual(
            validate_text(report(
                "NOT EXPOSED",
                "N/A — queue state is not installed with Pulp",
                "N/A — reproduced only against Shipyard queue records",
                "primary orchestrator-only queue fix",
            )),
            [],
        )
        errors = validate_text(report(
            "NOT EXPOSED", "N/A", "N/A", "primary orchestrator-only queue fix",
        ))
        self.assertEqual(len(errors), 2, errors)

    def test_missing_or_unknown_classification_fails(self) -> None:
        self.assertTrue(validate_text("# no contract\n"))
        errors = validate_text(report(
            "UNKNOWN", "TBD", "TBD", "TBD",
        ))
        self.assertTrue(errors)

    def test_duplicate_fields_and_missing_evidence_fail(self) -> None:
        text = report(
            "NOT EXPOSED",
            "N/A — queue state is not installed with Pulp",
            "N/A — reproduced only against queue records",
            "primary orchestrator-only queue fix",
        ).replace(
            "**Evidence:** exact direct-path reproduction or exact orchestrator-only boundary",
            "**Evidence:** N/A\n**Evidence:** N/A",
        )
        errors = validate_text(text)
        self.assertTrue(any("duplicate field: Evidence" in e for e in errors), errors)
        self.assertTrue(any("lowest independently reproducing layer" in e for e in errors), errors)


if __name__ == "__main__":
    unittest.main(verbosity=2)
