#!/usr/bin/env python3
"""Fail-closed unit tests for the A5 clean-agent evidence validator."""

from __future__ import annotations

import math
import pathlib
import tempfile
import unittest

import gpu_clean_agent_journey as journey


def valid_pass() -> dict:
    return {
        "sequence": 0,
        "name": "oracle",
        "verdict": "pass",
        "work_completed": True,
        "expected": 0.0,
        "observed": 0.0,
        "absolute_error": 0.0,
        "code": "cpu_oracle_match",
    }


def valid_result() -> dict:
    return {
        "schema": journey.RESULT_SCHEMA,
        "version": 1,
        "gpu_evidence_id": "a" * 32,
        "recipe_id": "gpu-compute.magnitude.v1",
        "source_digest": "b" * 64,
        "signature_digest": "c" * 64,
        "dimensions": {"width": 1, "height": 1, "work_items": 1},
        "seed": 0,
        "clock": "fixed-step",
        "input_format": "complex-f32",
        "output_format": "f32",
        "encoding": "little-endian-ieee754",
        "tolerance": {"absolute": 0.0, "relative": 0.0},
        "adapter_policy": "hardware-required",
        "adapter": {
            "status": "authentic", "class": "hardware", "backend": "Metal",
            "name": "Apple M3 Ultra", "vendor": "apple",
            "architecture": "metal-3", "device": "vendor=0x106b,device=0x0",
        },
        "numeric_sample_count": 1,
        "mutation": None,
        "verdict": "pass",
        "passes": [valid_pass()],
        "artifacts": [],
        "recommendations": [],
    }


class CleanAgentEvidenceContract(unittest.TestCase):
    def test_workspace_inventory_is_exact_and_rejects_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = pathlib.Path(temporary)
            for relative in (
                "artifacts/reference",
                "artifacts/seeded-failure",
                "artifacts/repaired",
            ):
                (workspace / relative).mkdir(parents=True, exist_ok=True)
            for name in (
                "README.md", "gpu-recipe.json", "reference-result.json",
                "seeded-failure-result.json", "repaired-result.json",
                "clean-agent-journey.json",
            ):
                (workspace / name).write_bytes(b"bounded")
            hashes = {}
            journey._validate_workspace_inventory(workspace, hashes, hashes, hashes)
            (workspace / "undeclared").symlink_to(workspace / "README.md")
            with self.assertRaises(journey.JourneyError):
                journey._validate_workspace_inventory(workspace, hashes, hashes, hashes)

    def test_result_schema_shape_is_closed_and_bounded(self) -> None:
        result = valid_result()
        journey._validate_result_schema_shape(result)
        mutations = []
        missing = dict(result)
        missing.pop("clock")
        mutations.append(missing)
        extra = dict(result)
        extra["extension"] = True
        mutations.append(extra)
        bad_dimensions = dict(result)
        bad_dimensions["dimensions"] = {"width": 0, "height": 1, "work_items": 1}
        mutations.append(bad_dimensions)
        bad_tolerance = dict(result)
        bad_tolerance["tolerance"] = {"absolute": math.inf, "relative": 0.0}
        mutations.append(bad_tolerance)
        bad_recommendations = dict(result)
        bad_recommendations["recommendations"] = [""]
        mutations.append(bad_recommendations)
        for malformed in mutations:
            with self.subTest(malformed=malformed):
                with self.assertRaises(journey.JourneyError):
                    journey._validate_result_schema_shape(malformed)

    def test_workspace_rejects_symlinked_parent(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            real_parent = root / "real"
            real_parent.mkdir()
            alias = root / "alias"
            alias.symlink_to(real_parent, target_is_directory=True)
            with self.assertRaises(journey.JourneyError):
                journey._require_absolute_new_workspace(alias / "journey")

    def test_artifact_verification_rejects_symlinked_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            artifact_dir = root / "artifacts"
            artifact_dir.mkdir()
            external = root / "external.bin"
            external.write_bytes(b"bounded")
            (artifact_dir / "observed.bin").symlink_to(external)
            evidence = {
                "artifacts": [
                    {
                        "name": "observed.bin",
                        "kind": "numeric-samples",
                        "mime": "application/octet-stream",
                        "bytes": len(b"bounded"),
                        "sha256": journey._sha256(external),
                    }
                ]
            }
            with self.assertRaises(journey.JourneyError):
                journey._verified_artifacts(evidence, artifact_dir, root)

    def test_artifact_verification_rejects_undeclared_entries(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            artifact_dir = root / "artifacts"
            artifact_dir.mkdir()
            declared = artifact_dir / "declared.bin"
            declared.write_bytes(b"declared")
            (artifact_dir / "undeclared.bin").write_bytes(b"undeclared")
            evidence = {
                "artifacts": [{
                    "name": declared.name, "kind": "numeric-samples",
                    "mime": "application/octet-stream", "bytes": 8,
                    "sha256": journey._sha256(declared),
                }]
            }
            with self.assertRaises(journey.JourneyError):
                journey._verified_artifacts(evidence, artifact_dir, root)

    def test_artifact_verification_enforces_v1_name_bound(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for length, accepted in ((240, True), (241, False)):
                artifact_dir = root / f"artifacts-{length}"
                artifact_dir.mkdir()
                artifact = artifact_dir / ("x" * length)
                artifact.write_bytes(b"bounded")
                evidence = {
                    "artifacts": [{
                        "name": artifact.name, "kind": "numeric-samples",
                        "mime": "application/octet-stream", "bytes": 7,
                        "sha256": journey._sha256(artifact),
                    }]
                }
                if accepted:
                    journey._verified_artifacts(evidence, artifact_dir, root)
                else:
                    with self.assertRaises(journey.JourneyError):
                        journey._verified_artifacts(evidence, artifact_dir, root)

    def test_artifact_verification_rejects_symlinked_ancestor_escape(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            workspace = root / "workspace"
            workspace.mkdir()
            external = root / "external"
            artifact_dir = external / "reference"
            artifact_dir.mkdir(parents=True)
            (workspace / "artifacts").symlink_to(external, target_is_directory=True)
            artifact = artifact_dir / "declared.bin"
            artifact.write_bytes(b"declared")
            evidence = {
                "artifacts": [{
                    "name": artifact.name, "kind": "numeric-samples",
                    "mime": "application/octet-stream", "bytes": 8,
                    "sha256": journey._sha256(artifact),
                }]
            }
            with self.assertRaises(journey.JourneyError):
                journey._verified_artifacts(
                    evidence, workspace / "artifacts" / "reference", workspace
                )

    def test_authentic_adapter_requires_complete_concrete_identity(self) -> None:
        adapter = {
            "status": "authentic",
            "class": "hardware",
            "backend": "Metal",
            "name": "Apple M3 Ultra",
            "vendor": "apple",
            "architecture": "metal-3",
            "device": "vendor=0x106b,device=0x0",
        }
        evidence = {"adapter_policy": "hardware-required", "adapter": adapter}
        self.assertEqual(journey._authentic_adapter(evidence), adapter)
        for field in adapter:
            with self.subTest(field=field):
                incomplete = dict(adapter)
                incomplete.pop(field)
                with self.assertRaises(journey.JourneyError):
                    journey._authentic_adapter(
                        {"adapter_policy": "hardware-required", "adapter": incomplete}
                    )
        for field, placeholder in (
            ("name", "unknown"),
            ("name", "Apple unknown"),
            ("vendor", "generic"),
            ("architecture", "n/a"),
            ("device", "vendor=0x0,device=0x0"),
            ("device", "opaque-device"),
        ):
            with self.subTest(field=field, placeholder=placeholder):
                malformed = dict(adapter)
                malformed[field] = placeholder
                with self.assertRaises(journey.JourneyError):
                    journey._authentic_adapter(
                        {"adapter_policy": "hardware-required", "adapter": malformed}
                    )
        unsupported = dict(adapter)
        unsupported.update(
            backend="Vulkan",
            name="NVIDIA GeForce RTX 4090",
            vendor="nvidia",
            architecture="ada",
            device="vendor=0x10de,device=0x2684",
        )
        with self.assertRaises(journey.JourneyUnavailable):
            journey._authentic_adapter(
                {"adapter_policy": "hardware-required", "adapter": unsupported}
            )

    def test_selection_receipt_must_bind_the_exact_catalog_row_and_revision(self) -> None:
        recipe = {"id": "gpu-compute.magnitude.v1", "summary": "bounded"}
        selection = {
            "schema": journey.SELECTION_SCHEMA,
            "catalog_schema": journey.CATALOG_SCHEMA,
            "catalog_revision": 3,
            "recipe": recipe,
        }
        journey._validate_selection_binding(selection, recipe, 3)
        for field, value in (
            ("catalog_schema", "pulp.gpu-recipes.v0"),
            ("catalog_revision", 2),
            ("recipe", {"id": "other"}),
        ):
            with self.subTest(field=field):
                mutated = dict(selection)
                mutated[field] = value
                with self.assertRaises(journey.JourneyError):
                    journey._validate_selection_binding(mutated, recipe, 3)

    def test_typed_pass_sequence_accepts_the_complete_contract(self) -> None:
        evidence = {"passes": [valid_pass()]}
        self.assertEqual(journey._typed_passes(evidence), evidence["passes"])

    def test_typed_pass_sequence_rejects_non_object_and_incomplete_rows(self) -> None:
        for malformed in (None, "pass", 1, [], {"verdict": "pass"}):
            with self.subTest(malformed=malformed):
                with self.assertRaises(journey.JourneyError):
                    journey._typed_passes({"passes": [valid_pass(), malformed]})

    def test_typed_pass_sequence_rejects_invalid_field_types_and_order(self) -> None:
        mutations = {
            "sequence": 4,
            "name": "",
            "verdict": "passed",
            "work_completed": 1,
            "expected": "zero",
            "observed": math.nan,
            "code": "",
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                item = valid_pass()
                item[field] = value
                with self.assertRaises(journey.JourneyError):
                    journey._typed_passes({"passes": [item]})

        extra = valid_pass()
        extra["untyped"] = True
        with self.assertRaises(journey.JourneyError):
            journey._typed_passes({"passes": [extra]})

        too_long_name = valid_pass()
        too_long_name["name"] = "x" * 65
        with self.assertRaises(journey.JourneyError):
            journey._typed_passes({"passes": [too_long_name]})

        too_many = []
        for index in range(17):
            item = valid_pass()
            item["sequence"] = index
            too_many.append(item)
        with self.assertRaises(journey.JourneyError):
            journey._typed_passes({"passes": too_many})

    def test_typed_pass_sequence_enforces_semantic_numeric_invariants(self) -> None:
        mutations = []

        incomplete_pass = valid_pass()
        incomplete_pass["work_completed"] = False
        mutations.append(incomplete_pass)

        missing_observed = valid_pass()
        missing_observed["observed"] = None
        mutations.append(missing_observed)

        missing_error = valid_pass()
        missing_error["absolute_error"] = None
        mutations.append(missing_error)

        negative_error = valid_pass()
        negative_error["absolute_error"] = -1.0
        mutations.append(negative_error)

        incoherent_error = valid_pass()
        incoherent_error["observed"] = 2.0
        incoherent_error["absolute_error"] = 1.0
        mutations.append(incoherent_error)

        for malformed in mutations:
            with self.subTest(malformed=malformed):
                with self.assertRaises(journey.JourneyError):
                    journey._typed_passes({"passes": [malformed]})

    def test_artifact_partition_names_every_change_without_filename_roles(self) -> None:
        reference = {
            "opaque-a.bin": "a" * 64,
            "opaque-b.bin": "b" * 64,
            "opaque-c.bin": "c" * 64,
        }
        seeded = dict(reference)
        seeded["opaque-b.bin"] = "d" * 64
        self.assertEqual(
            journey._partition_artifact_changes(reference, seeded),
            (["opaque-a.bin", "opaque-c.bin"], ["opaque-b.bin"]),
        )

        seeded["opaque-a.bin"] = "e" * 64
        self.assertEqual(
            journey._partition_artifact_changes(reference, seeded),
            (["opaque-c.bin"], ["opaque-a.bin", "opaque-b.bin"]),
        )

    def test_artifact_partition_requires_exact_keys_and_both_partitions(self) -> None:
        reference = {"a": "a" * 64, "b": "b" * 64}
        invalid_seeded = (
            {"a": "a" * 64},
            dict(reference),
            {"a": "c" * 64, "b": "d" * 64},
        )
        for seeded in invalid_seeded:
            with self.subTest(seeded=seeded):
                with self.assertRaises(journey.JourneyError):
                    journey._partition_artifact_changes(reference, seeded)

    def test_seeded_failure_rejects_unavailable_or_unverified_rows(self) -> None:
        for verdict in ("unavailable", "unverified"):
            with self.subTest(verdict=verdict):
                row = valid_pass()
                row["verdict"] = verdict
                with self.assertRaises(journey.JourneyUnavailable):
                    journey._seeded_failing_passes({"passes": [row]})

    def test_recipe_contract_rejects_pass_or_artifact_substitution(self) -> None:
        artifact = {
            "name": "input.complex-f32",
            "kind": "numeric-samples",
            "mime": "application/octet-stream",
            "bytes": 8,
            "sha256": "a" * 64,
        }
        reference = {"passes": [valid_pass()], "artifacts": [artifact]}
        pass_contract = journey._pass_contract(reference)
        artifact_contract = journey._artifact_contract(reference)

        renamed_pass = valid_pass()
        renamed_pass["name"] = "decoy"
        with self.assertRaises(journey.JourneyError):
            journey._require_recipe_contract(
                {"passes": [renamed_pass], "artifacts": [artifact]},
                pass_contract,
                artifact_contract,
            )

        renamed_artifact = dict(artifact)
        renamed_artifact["name"] = "input-marker.json"
        with self.assertRaises(journey.JourneyError):
            journey._require_recipe_contract(
                {"passes": [valid_pass()], "artifacts": [renamed_artifact]},
                pass_contract,
                artifact_contract,
            )

    def test_execution_contract_rejects_any_typed_identity_change(self) -> None:
        reference = valid_result()
        contract = journey._execution_contract(reference)
        journey._require_execution_contract(reference, contract)
        for field, value in (
            ("seed", 1),
            ("clock", "other-clock"),
            ("numeric_sample_count", 2),
            ("dimensions", {"width": 2, "height": 1, "work_items": 2}),
            ("tolerance", {"absolute": 1.0, "relative": 0.0}),
        ):
            with self.subTest(field=field):
                changed = dict(reference)
                changed[field] = value
                with self.assertRaises(journey.JourneyError):
                    journey._require_execution_contract(changed, contract)

    def test_signature_isolation_rejects_stale_seeded_signature(self) -> None:
        reference = {"signature_digest": "a" * 64}
        repaired = {"signature_digest": "a" * 64}
        seeded = {"signature_digest": "b" * 64}
        journey._require_signature_isolation(reference, seeded, repaired)
        seeded["signature_digest"] = reference["signature_digest"]
        with self.assertRaises(journey.JourneyError):
            journey._require_signature_isolation(reference, seeded, repaired)


if __name__ == "__main__":
    unittest.main()
