#!/usr/bin/env python3
"""Dependency-free checks that the FrontendIR schema tracks emitted artifacts."""

from __future__ import annotations

import json
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA_PATH = ROOT / "planning" / "schemas" / "pulp-frontend-ir-v0.schema.json"
ARTIFACTS = [
    ROOT / "planning" / "artifacts" / "native-ui" / "nv0" / "reports" / "chainer-frontend-ir.json",
    ROOT / "planning" / "artifacts" / "native-ui" / "nv0" / "reports" / "compressor-strip-frontend-ir.json",
    ROOT / "planning" / "artifacts" / "native-ui" / "nv0" / "reports" / "static-html-control-panel-frontend-ir.json",
]


def load_json(path: pathlib.Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise AssertionError(f"{path} did not contain a JSON object")
    return data


# The schema and its artifacts live in the private planning/ submodule, which an
# external clone (and CI checkouts that do not fetch it) does not have.
@unittest.skipUnless(
    SCHEMA_PATH.is_file(),
    f"planning/ submodule not checked out: {SCHEMA_PATH.relative_to(ROOT)} is missing",
)
class FrontendIrSchemaContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.schema = load_json(SCHEMA_PATH)
        self.validation_schema = self.schema["$defs"]["validation"]
        self.validation_props = self.validation_schema["properties"]

    def test_schema_covers_current_validation_fields(self) -> None:
        for path in ARTIFACTS:
            with self.subTest(path=path.name):
                report = load_json(path)
                validation = report.get("validation", {})
                self.assertIsInstance(validation, dict)
                self.assertFalse(
                    set(validation) - set(self.validation_props),
                    f"{path.name} emits validation fields not covered by schema",
                )

    def test_schema_covers_current_compile_proof_fields(self) -> None:
        compile_props = self.validation_props["compile"]["properties"]
        binary_props = self.validation_props["binary_dependencies"]["properties"]
        proof_props = self.validation_props["proofs"]["items"]["properties"]
        compile_status_enum = set(compile_props["status"]["enum"])
        proof_status_enum = set(proof_props["status"]["enum"])

        for path in ARTIFACTS:
            with self.subTest(path=path.name):
                validation = load_json(path).get("validation", {})
                compile_status = validation.get("compile", {})
                binary_status = validation.get("binary_dependencies", {})
                proofs = validation.get("proofs", [])

                if isinstance(compile_status, dict):
                    self.assertFalse(set(compile_status) - set(compile_props))
                    status = compile_status.get("status")
                    if status is not None:
                        self.assertIn(status, compile_status_enum)

                if isinstance(binary_status, dict):
                    self.assertFalse(set(binary_status) - set(binary_props))

                if isinstance(proofs, list):
                    for proof in proofs:
                        self.assertIsInstance(proof, dict)
                        self.assertFalse(set(proof) - set(proof_props))
                        self.assertIn(proof.get("status"), proof_status_enum)


if __name__ == "__main__":
    unittest.main()
