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


def registry_header(*ids: str) -> str:
    rows = ",\n".join(f'    std::string_view{{"{recipe_id}"}}' for recipe_id in ids)
    return f"inline constexpr std::array kRecipeIds{{\n{rows},\n}};\n"


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
            header = pathlib.Path(directory) / "probe_result.hpp"
            path.write_text(json.dumps(valid_document()), encoding="utf-8")
            header.write_text(registry_header("gpu.renderer3d.cube"), encoding="utf-8")
            self.assertEqual(
                catalog.main(
                    ["--catalog", str(path), "--registry-header", str(header)]
                ),
                0,
            )

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

    def test_checked_in_catalog_matches_native_registry_and_references(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        native = catalog.probe_registry_ids(
            catalog.DEFAULT_REGISTRY_HEADER.read_text(encoding="utf-8")
        )
        self.assertEqual(catalog.validate_registry_projection(document, native), [])
        self.assertEqual(catalog.validate_repository_references(document, catalog.ROOT), [])
        self.assertEqual(catalog.main([]), 0)

    def test_new_native_recipe_fails_closed_until_catalog_is_explicitly_updated(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        native = catalog.probe_registry_ids(
            catalog.DEFAULT_REGISTRY_HEADER.read_text(encoding="utf-8")
        )
        problems = catalog.validate_registry_projection(
            document, [*native, "threejs.multi-pass.v1"]
        )
        self.assertIn("missing=['threejs.multi-pass.v1']", "\n".join(problems))

    def test_catalog_recipe_not_in_native_registry_is_rejected(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        native = catalog.probe_registry_ids(
            catalog.DEFAULT_REGISTRY_HEADER.read_text(encoding="utf-8")
        )
        problems = catalog.validate_registry_projection(document, native[:-1])
        self.assertIn("extra=['gpu-audio.stft.v1']", "\n".join(problems))

    def test_clean_agent_can_select_a_recipe_by_exact_symptom(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        selected = catalog.recipes_for_symptoms(document, ["blank-gpu-output"])
        self.assertEqual(
            [recipe["id"] for recipe in selected], ["renderer3d.hardcoded-cube.v1"]
        )
        self.assertEqual(
            selected[0]["entrypoints"]["cli"]["command"][:5],
            [
                "pulp",
                "gpu",
                "probe",
                "--recipe",
                "renderer3d.hardcoded-cube.v1",
            ],
        )
        self.assertEqual(selected[0]["entrypoints"]["trace"]["question"], "gpu-probe")

    def test_unknown_recipe_and_symptom_use_unavailable_exit_two(self) -> None:
        self.assertEqual(catalog.main(["--show", "missing.recipe"]), 2)
        self.assertEqual(catalog.main(["--symptom", "missing-symptom"]), 2)
        self.assertEqual(
            catalog.main(
                [
                    "--show",
                    "gpu-audio.stft.v1",
                    "--symptom",
                    "blank-gpu-output",
                ]
            ),
            2,
        )

    def test_vellum_handoff_is_closed_and_cannot_authorize_cutover(self) -> None:
        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        self.assertEqual(catalog.validate_handoff(handoff), [])
        self.assertEqual(catalog.validate_handoff_routing(handoff, catalog.ROOT), [])
        handoff["boundary_change_authorized"] = True
        self.assertIn(
            "must not authorize",
            "\n".join(catalog.validate_handoff(handoff)),
        )

    def test_vellum_handoff_rejects_action_and_authority_drift(self) -> None:
        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["entries"][2]["cutover_action"] = "delete-now"
        self.assertIn("unknown cutover action", "\n".join(catalog.validate_handoff(handoff)))

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["route_set_sha256"] = "0" * 64
        self.assertIn(
            "differs from the authoritative projection",
            "\n".join(catalog.validate_handoff_routing(handoff, catalog.ROOT)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["entries"][0]["current_owner"] = "Generous-Corp/vellum"
        problems = [
            *catalog.validate_handoff(handoff),
            *catalog.validate_handoff_routing(handoff, catalog.ROOT),
        ]
        self.assertIn("differs from routed owner", "\n".join(problems))


if __name__ == "__main__":
    unittest.main()
