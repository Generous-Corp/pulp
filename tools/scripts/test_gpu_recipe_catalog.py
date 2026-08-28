#!/usr/bin/env python3
"""Self-tests for the closed GPU recipe catalog contract."""

from __future__ import annotations

import copy
import json
import pathlib
import tempfile
import unittest

import gpu_recipe_catalog as catalog


SCHEMA = json.loads(catalog.DEFAULT_SCHEMA.read_text(encoding="utf-8"))


def valid_document() -> dict:
    return {
        "$schema": "gpu-recipes.schema.json",
        "schema": "pulp.gpu-recipes.v1",
        "catalog_revision": 1,
        "recipes": [
            {
                "id": "gpu.renderer3d.cube",
                "version": 1,
                "title": "Renderer3D cube",
                "summary": "Prove bounded render and readback work.",
                "symptoms": ["blank-output", "readback-mismatch"],
                "kind": "offline",
                "entrypoints": {
                    "cli": {
                        "command": [
                            "pulp",
                            "gpu",
                            "probe",
                            "--recipe",
                            "gpu.renderer3d.cube",
                            "--json",
                        ]
                    },
                    "mcp": {"tool": "pulp_gpu_probe", "recipe_id": "gpu.renderer3d.cube"},
                    "control": None,
                    "trace": {"question": "gpu-probe"},
                },
                "evidence_schema": "pulp.gpu-probe-result.v1",
                "docs": ["docs/guides/gpu-validation-checklist.md"],
                "skills": ["screenshot", "skia-gpu-build"],
                "scaffold_template": "tools/gpu-recipes/renderer3d-cube",
            }
        ],
    }


class CatalogContract(unittest.TestCase):
    def test_valid_fixture_passes_schema_and_semantics(self) -> None:
        self.assertEqual(catalog.validate_document(valid_document(), SCHEMA), [])

    def test_cli_accepts_json_compatible_yaml(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "gpu-recipes.yaml"
            path.write_text(json.dumps(valid_document()), encoding="utf-8")
            self.assertEqual(catalog.main(["--catalog", str(path)]), 0)

    def test_negative_mutations_fail_for_the_intended_reason(self) -> None:
        cases = []

        duplicate = valid_document()
        duplicate["recipes"].append(copy.deepcopy(duplicate["recipes"][0]))
        cases.append((duplicate, "sorted by unique id"))

        mismatched_mcp = valid_document()
        mismatched_mcp["recipes"][0]["entrypoints"]["mcp"]["recipe_id"] = "gpu.other"
        cases.append((mismatched_mcp, "must equal the catalog recipe id"))

        unsafe_doc = valid_document()
        unsafe_doc["recipes"][0]["docs"] = ["../private-plan.md"]
        cases.append((unsafe_doc, "unsafe path"))

        retired_live_path = valid_document()
        retired_live_path["recipes"][0]["entrypoints"]["cli"]["command"] = [
            "pulp",
            "inspect",
            "--host",
            "localhost",
            "--json",
        ]
        cases.append((retired_live_path, "retired live selector"))

        offline_control = valid_document()
        offline_control["recipes"][0]["entrypoints"]["control"] = {
            "operation_id": "dev.pulp.gpu/health.read@1"
        }
        cases.append((offline_control, "offline and must not declare"))

        live_without_control = valid_document()
        live_without_control["recipes"][0]["kind"] = "live"
        cases.append((live_without_control, "live and must declare"))

        for mutated, expected in cases:
            with self.subTest(expected=expected):
                self.assertIn(expected, "\n".join(catalog.validate_document(mutated, SCHEMA)))

    def test_non_json_yaml_is_rejected_instead_of_partially_parsed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "gpu-recipes.yaml"
            path.write_text("schema: pulp.gpu-recipes.v1\n", encoding="utf-8")
            self.assertEqual(catalog.main(["--catalog", str(path)]), 1)


if __name__ == "__main__":
    unittest.main()
