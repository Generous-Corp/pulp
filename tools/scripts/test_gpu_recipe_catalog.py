#!/usr/bin/env python3
"""Self-tests for the closed GPU recipe catalog contract."""

from __future__ import annotations

import copy
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

import connected_git_history
import gpu_recipe_catalog as catalog


SCHEMA = json.loads(catalog.DEFAULT_SCHEMA.read_text(encoding="utf-8"))


def registry_header(*ids: str) -> str:
    rows = ",\n".join(f'    std::string_view{{"{recipe_id}"}}' for recipe_id in ids)
    return f"inline constexpr std::array kRecipeIds{{\n{rows},\n}};\n"


def conditional_registry_header(default: list[str], enabled: list[str]) -> str:
    return (
        "#if PULP_GPU_PROBE_THREEJS_CALLABLE\n"
        + registry_header(*enabled)
        + "#else\n"
        + registry_header(*default)
        + "#endif\n"
    )


def valid_document() -> dict:
    return {
        "$schema": "gpu-recipes.schema.json",
        "schema": "pulp.gpu-recipes.v1",
        "catalog_revision": 1,
        "recipes": [
            {
                "id": "gpu.renderer3d.cube",
                "native_registry_index": 0,
                "availability": {
                    "required_features": [],
                    "unavailable_exit_code": 2,
                },
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
        self.assertEqual(
            native,
            [
                "renderer3d.hardcoded-cube.v1",
                "gpu-compute.magnitude.v1",
                "gpu-audio.stft.v1",
                "threejs.multi-pass.v1",
            ],
        )
        self.assertEqual(catalog.validate_registry_projection(document, native), [])
        self.assertEqual(catalog.validate_repository_references(document, catalog.ROOT), [])
        self.assertEqual(catalog.main([]), 0)

    def test_a2_recipe_skill_mappings_are_semantically_discoverable(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        recipes = {recipe["id"]: recipe for recipe in document["recipes"]}
        required_pairs = {
            "gpu-audio.stft.v1": "audio-harness",
            "renderer3d.hardcoded-cube.v1": "screenshot",
        }
        for recipe_id, skill in required_pairs.items():
            with self.subTest(recipe_id=recipe_id, skill=skill):
                self.assertIn(skill, recipes[recipe_id]["skills"])
                skill_text = (
                    catalog.ROOT / ".agents/skills" / skill / "SKILL.md"
                ).read_text(encoding="utf-8")
                self.assertIn(recipe_id, skill_text)

    def test_cpu_only_model_embedding_reads_authoritative_bytes_without_copied_ids(self) -> None:
        cmake = (
            catalog.ROOT / "tools/cli/gpu_probe/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        template = (
            catalog.ROOT / "tools/cli/gpu_recipe_catalog_data.h.in"
        ).read_text(encoding="utf-8")
        self.assertIn('docs/status/gpu-recipes.yaml"', cmake)
        self.assertIn("PULP_GPU_RECIPE_CATALOG_JSON", cmake)
        self.assertEqual(template.count("@PULP_GPU_RECIPE_CATALOG_JSON@"), 1)
        for recipe in catalog.load_and_validate(catalog.DEFAULT_CATALOG)["recipes"]:
            self.assertNotIn(recipe["id"], template)

    def test_new_native_recipe_fails_closed_until_catalog_is_explicitly_updated(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        native = catalog.probe_registry_ids(
            catalog.DEFAULT_REGISTRY_HEADER.read_text(encoding="utf-8")
        )
        problems = catalog.validate_registry_projection(
            document, [*native, "not-yet-cataloged.v1"]
        )
        self.assertIn("missing=['not-yet-cataloged.v1']", "\n".join(problems))

    def test_native_registry_reorder_is_rejected(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        native = catalog.probe_registry_ids(
            catalog.DEFAULT_REGISTRY_HEADER.read_text(encoding="utf-8")
        )
        problems = catalog.validate_registry_projection(document, list(reversed(native)))
        self.assertTrue(problems)

    def test_conditional_registry_is_rejected(self) -> None:
        native = catalog.probe_registry_ids(
            catalog.DEFAULT_REGISTRY_HEADER.read_text(encoding="utf-8")
        )
        with self.assertRaisesRegex(ValueError, "not found exactly once"):
            catalog.probe_registry_variants(
                conditional_registry_header(native, [*native, "threejs.multi-pass.v1"])
            )

    def test_catalog_discovery_does_not_hide_unavailable_recipes(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        native = catalog.probe_registry_ids(
            catalog.DEFAULT_REGISTRY_HEADER.read_text(encoding="utf-8")
        )
        self.assertEqual(
            catalog.catalog_probe_ids(document),
            native,
        )

    def test_catalog_recipe_not_in_native_registry_is_rejected(self) -> None:
        document = catalog.load_and_validate(catalog.DEFAULT_CATALOG)
        native = catalog.probe_registry_ids(
            catalog.DEFAULT_REGISTRY_HEADER.read_text(encoding="utf-8")
        )
        problems = catalog.validate_registry_projection(document, native[:-1])
        self.assertIn("extra=['threejs.multi-pass.v1']", "\n".join(problems))

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
        threejs = catalog.recipes_for_symptoms(document, ["threejs-runtime-unavailable"])
        self.assertEqual([recipe["id"] for recipe in threejs], ["threejs.multi-pass.v1"])
        self.assertEqual(
            threejs[0]["availability"]["required_features"],
            ["pinned-threejs-runtime", "v8"],
        )

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

    def test_vellum_handoff_v2_is_closed_and_default_denies_deletion(self) -> None:
        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        self.assertEqual(catalog.validate_handoff(handoff), [])
        self.assertEqual(catalog.validate_handoff_routing(handoff, catalog.ROOT), [])
        self.assertEqual(handoff["schema"], "pulp.gpu-vellum-handoff.v2")
        self.assertEqual(
            [entry["id"] for entry in handoff["entries"]],
            list(catalog.HANDOFF_PACKAGE_IDS),
        )
        self.assertIn("b3-vellum-gpu-capture", handoff["required_package_ids"])
        self.assertIn("b0-adoption-authority-freeze", handoff["required_package_ids"])
        self.assertIn("b4-vellum-observability-executor", handoff["required_package_ids"])
        self.assertIn("b4-vellum-signature-prewarm", handoff["required_package_ids"])
        self.assertEqual(
            handoff["authorities"]["plan"]["revision"],
            "bec7fb85c6f8886d32fac0a34be491eb6821a741",
        )
        self.assertEqual(handoff["upstream"]["current_comment_id"], 5464003821)
        comments = {
            comment["id"]: comment
            for comment in handoff["upstream"]["comments"]
        }
        self.assertEqual(comments[5464003821]["status"], "current")
        self.assertEqual(
            comments[5464003821]["supersedes"],
            [5461645539, 5462069172, 5462803929],
        )
        for superseded in (5461645539, 5462069172, 5462803929):
            self.assertEqual(comments[superseded]["status"], "superseded")
            self.assertEqual(comments[superseded]["superseded_by"], 5464003821)
        self.assertIn(
            "foundational Vellum cutover is merged on protected Pulp main",
            handoff["cutover_trigger"],
        )
        host = next(
            entry
            for entry in handoff["entries"]
            if entry["id"] == "a3-pulp-input-to-present-host"
        )
        self.assertEqual(len(host["trace_contract"]["events"]), 20)
        self.assertIn("core/view/platform/mac/window_host_mac.mm", host["retained_paths"])
        b0 = next(
            entry for entry in handoff["entries"]
            if entry["id"] == "b0-adoption-authority-freeze"
        )
        self.assertEqual(tuple(b0["depends_on"]), catalog.HANDOFF_B0_DEPENDENCIES)
        self.assertEqual(b0["delete_paths"], [])
        b4 = next(
            entry
            for entry in handoff["entries"]
            if entry["id"] == "b4-vellum-observability-executor"
        )
        self.assertEqual(b4["accepted_dispositions"], ["pass"])
        self.assertIn(
            "regardless of the A3 optimization disposition", b4["trigger"]
        )
        a4 = next(
            entry for entry in handoff["entries"]
            if entry["id"] == "a4-pulp-dpr-experiment"
        )
        self.assertEqual(
            a4["output_receipt"]["id"], "a4-pulp-dpr-experiment.terminal.v2"
        )
        self.assertEqual(
            a4["output_receipt"]["schema"], "pulp.gpu-vellum-package-terminal.v1"
        )
        self.assertIn("a2t-pulp-trace-analysis.terminal.v1", a4["input_receipts"])
        self.assertIn("a3-pulp-dpr-product-policy.terminal.v2", a4["input_receipts"])
        followup = next(
            entry
            for entry in handoff["entries"]
            if entry["id"] == "pulp-v8-threejs-release-followup"
        )
        self.assertIn("experimental/pulp-rs/src/install.rs", followup["retained_paths"])
        self.assertIn("tools/scripts/release_artifact_contents.py", followup["retained_paths"])
        for entry in handoff["entries"]:
            with self.subTest(package=entry["id"]):
                inventory = {row["path"] for row in entry["pulp_paths"]}
                disposition = set(entry["retained_paths"]) | set(entry["delete_paths"])
                self.assertEqual(inventory, disposition)
                self.assertEqual(entry["delete_paths"], [])
                self.assertTrue(entry["accepted_dispositions"])
                self.assertTrue(entry["terminal_evidence"]["required"])
                self.assertTrue(entry["terminal_evidence"]["planted"])
                expected_inputs = [
                    catalog.HANDOFF_OUTPUT_RECEIPT_OVERRIDES.get(
                        dependency, f"{dependency}.terminal.v1"
                    )
                    for dependency in entry["depends_on"]
                ] + catalog.HANDOFF_SUPPLEMENTAL_INPUT_RECEIPTS.get(entry["id"], [])
                self.assertEqual(entry["input_receipts"], sorted(expected_inputs))
                for row in entry["pulp_paths"]:
                    self.assertRegex(row["revision"], r"^[0-9a-f]{40}$")
                    self.assertRegex(row["object_id"], r"^[0-9a-f]{40}$")
                    self.assertIn(row["object_type"], {"blob", "tree"})
                self.assertEqual(
                    entry["deletion_gate"],
                    {"authorized": False, "authority": "none", "required_evidence": []},
                )
        handoff["boundary_change_authorized"] = True
        self.assertIn(
            "must not authorize",
            "\n".join(catalog.validate_handoff(handoff)),
        )

    def test_vellum_handoff_rejects_missing_packages_targets_and_contracts(self) -> None:
        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["entries"] = [
            entry for entry in handoff["entries"] if entry["id"] != "b3-vellum-gpu-capture"
        ]
        self.assertIn(
            "closed A2-B6 package set",
            "\n".join(catalog.validate_handoff(handoff)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        b3 = next(entry for entry in handoff["entries"] if entry["id"] == "b3-vellum-gpu-capture")
        b3["vellum_paths"] = []
        self.assertIn("empty applicable Vellum target", "\n".join(catalog.validate_handoff(handoff)))

        for field, expected in (
            ("api_contract", "api_contract is incomplete"),
            ("ownership_lifetime_rt", "ownership_lifetime_rt is incomplete"),
            ("tests", "tests has the wrong closed shape"),
        ):
            with self.subTest(missing_contract=field):
                handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
                b3 = next(
                    entry for entry in handoff["entries"] if entry["id"] == "b3-vellum-gpu-capture"
                )
                b3[field] = {}
                self.assertIn(expected, "\n".join(catalog.validate_handoff(handoff)))

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        b6 = next(
            entry for entry in handoff["entries"] if entry["id"] == "b6-pulp-vellum-adoption-skills"
        )
        b6["vellum_paths"][0]["path"] = ".agents/skills/unknown/SKILL.md"
        self.assertIn(
            "claims an unknown existing Vellum path",
            "\n".join(catalog.validate_handoff(handoff)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        b1 = next(entry for entry in handoff["entries"] if entry["id"] == "b1-vellum-gpu-runtime")
        b1["vellum_paths"] = [
            row for row in b1["vellum_paths"] if row["path"] != "graphics/CMakeLists.txt"
        ]
        self.assertIn(
            "omits Vellum source/test/install/export wiring",
            "\n".join(catalog.validate_handoff(handoff)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        b6 = next(
            entry for entry in handoff["entries"] if entry["id"] == "b6-pulp-vellum-adoption-skills"
        )
        b6["terminal_evidence"]["required"].remove(
            "old generic Pulp symbols and targets are absent for every separately authorized delete path"
        )
        self.assertIn(
            "omits B6 absence/corpus/overhead terminal proof",
            "\n".join(catalog.validate_handoff(handoff)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["entries"][1]["input_receipts"] = []
        self.assertIn(
            "input_receipts must exactly bind dependency outputs",
            "\n".join(catalog.validate_handoff(handoff)),
        )

    def test_vellum_handoff_rejects_unsafe_deletion_dispositions(self) -> None:
        def deletion_gate() -> dict:
            return {
                "authorized": False,
                "authority": "future B6 exact-path review",
                "required_evidence": ["installed replacement proof"],
            }

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        a2 = next(entry for entry in handoff["entries"] if entry["id"] == "a2-pulp-product-probes")
        broad = "tools/cli/gpu_probe"
        next(row for row in a2["pulp_paths"] if row["path"] == broad)["state"] = (
            "pulp-owned-delete-candidate"
        )
        a2["retained_paths"].remove(broad)
        a2["delete_paths"] = [broad]
        a2["deletion_gate"] = deletion_gate()
        self.assertIn(
            "contains broad directory",
            "\n".join(catalog.validate_handoff_routing(handoff, catalog.ROOT)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        a2 = next(entry for entry in handoff["entries"] if entry["id"] == "a2-pulp-product-probes")
        a2["delete_paths"] = ["tools/scripts/not-in-pulp-inventory.py"]
        a2["deletion_gate"] = deletion_gate()
        self.assertIn(
            "delete candidate is absent from the Pulp inventory",
            "\n".join(catalog.validate_handoff(handoff)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        a2 = next(entry for entry in handoff["entries"] if entry["id"] == "a2-pulp-product-probes")
        overlap = "tools/mcp/mcp_gpu_tools.cpp"
        next(row for row in a2["pulp_paths"] if row["path"] == overlap)["state"] = (
            "pulp-owned-delete-candidate"
        )
        a2["delete_paths"] = [overlap]
        a2["deletion_gate"] = deletion_gate()
        self.assertIn("retained/deleted paths overlap", "\n".join(catalog.validate_handoff(handoff)))

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        a2 = next(entry for entry in handoff["entries"] if entry["id"] == "a2-pulp-product-probes")
        a2["deletion_gate"] = deletion_gate()
        self.assertIn(
            "grants prose-only deletion without delete_paths",
            "\n".join(catalog.validate_handoff(handoff)),
        )

    def test_vellum_handoff_rejects_authority_and_routing_drift(self) -> None:
        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["authorities"]["plan"]["revision"] = "0" * 40
        self.assertIn(
            "must pin the reviewed plan/Pulp/Vellum revisions",
            "\n".join(catalog.validate_handoff(handoff)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["upstream"]["current_comment_id"] = 5462069172
        self.assertIn(
            "upstream issue/comment IDs differ from the reviewed authority",
            "\n".join(catalog.validate_handoff(handoff)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["upstream"]["comments"][-1]["status"] = "superseded"
        self.assertIn(
            "upstream issue/comment IDs differ from the reviewed authority",
            "\n".join(catalog.validate_handoff(handoff)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["route_set_sha256"] = "0" * 64
        self.assertIn(
            "differs from the authoritative projection",
            "\n".join(catalog.validate_handoff_routing(handoff, catalog.ROOT)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        b4 = next(
            entry
            for entry in handoff["entries"]
            if entry["id"] == "b4-vellum-observability-executor"
        )
        next(
            row for row in b4["pulp_paths"] if row["path"] == "core/render/src/skia_surface.cpp"
        )["state"] = "pulp-owned-retained"
        self.assertIn(
            "expects 'Generous-Corp/pulp' but routing returned 'Generous-Corp/vellum'",
            "\n".join(catalog.validate_handoff_routing(handoff, catalog.ROOT)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["entries"][0]["pulp_paths"][0]["path"] = "tools/cli/gpu_prove"
        self.assertIn(
            "path does not exist inside the repository",
            "\n".join(catalog.validate_handoff_routing(handoff, catalog.ROOT)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        row = next(
            row
            for entry in handoff["entries"]
            for row in entry["pulp_paths"]
            if row["path"] == "tools/scripts/gpu_recipe_catalog.py"
        )
        # Use a real, durable Pulp revision that contains this path. The
        # reviewed handoff-authority commit predates the file and is not a
        # valid fixture for a planted blob-drift test.
        path_history = subprocess.check_output(
            ["git", "log", "--format=%H", "HEAD", "--", row["path"]],
            cwd=catalog.ROOT, text=True,
        ).splitlines()
        self.assertGreaterEqual(len(path_history), 2)
        row["revision"] = path_history[1]
        row["object_id"] = subprocess.check_output(
            ["git", "rev-parse", f"{row['revision']}:{row['path']}"],
            cwd=catalog.ROOT, text=True,
        ).strip()
        self.assertIn(
            "stale revision/blob/tree identity",
            "\n".join(catalog.validate_handoff_routing(handoff, catalog.ROOT)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["entries"][0]["pulp_paths"][0]["object_id"] = "0" * 40
        self.assertIn(
            "stale revision/blob/tree identity",
            "\n".join(catalog.validate_handoff_routing(handoff, catalog.ROOT)),
        )

        handoff = json.loads(catalog.DEFAULT_HANDOFF.read_text(encoding="utf-8"))
        handoff["ownership_projection"] = "docs/status/alternate-ownership.json"
        self.assertIn(
            "must be the authoritative",
            "\n".join(catalog.validate_handoff_routing(handoff, catalog.ROOT)),
        )



def _git(root, *arguments):
    subprocess.run(
        ["git", "-c", "user.email=history@example.invalid", "-c", "user.name=History",
         *arguments],
        cwd=root, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE, check=True,
    )


class TruncatedCheckoutAttribution(unittest.TestCase):
    """A short history must be named, not misreported as a missing path.

    ``git log -1 --format=%H <commit> -- <path>`` does not fail on a truncated
    checkout; it answers with the shallow graft boundary, or cannot answer at all
    because the pinned revision is absent. Either way the pinned row fails for a
    reason the ledger cannot fix, and the pre-existing "is absent from its pinned
    Pulp revision" message sends the reader to repair the wrong thing.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls._workspace = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls._workspace.cleanup)
        base = pathlib.Path(cls._workspace.name)

        # --no-local is load-bearing. Without it Git hardlinks the source object
        # store and silently ignores --depth, yielding a complete clone that
        # would make every assertion here vacuous.
        cls.truncated = base / "truncated"
        _git(base, "clone", "--quiet", "--depth=1", "--no-local",
             f"file://{catalog.ROOT}", str(cls.truncated))

        # A genuinely unshallow checkout, built by re-rooting a second clone's
        # working tree onto a fresh single-commit history. A real full clone is
        # not available: this host's only Pulp object store is itself shallow, so
        # `git clone --no-local` from it dies with "early EOF". The fixture keeps
        # the property under test -- no graft boundary at all -- and keeps the
        # pinned revisions absent, which is what makes it prove that the new
        # guard does not steal the absent-path message.
        cls.complete = base / "complete"
        _git(base, "clone", "--quiet", "--depth=1", "--no-local",
             f"file://{catalog.ROOT}", str(cls.complete))
        shutil.rmtree(cls.complete / ".git")
        _git(cls.complete, "init", "--quiet", "--initial-branch=main")
        _git(cls.complete, "add", ".")
        _git(cls.complete, "commit", "--quiet", "-m", "complete fixture")

        cls.handoff = json.loads(
            (cls.truncated / "docs/status/gpu-vellum-handoff.yaml").read_text(
                encoding="utf-8"
            )
        )

    def test_truncated_checkout_is_named_not_reported_as_a_missing_path(self) -> None:
        problems = catalog.validate_handoff_routing(
            copy.deepcopy(self.handoff), self.truncated
        )
        shallow = [problem for problem in problems if "history is truncated" in problem]
        self.assertEqual(len(shallow), 1, problems)
        self.assertIn("git fetch --unshallow", shallow[0])
        self.assertIn("hydrate_gpu_provenance_commits.py", shallow[0])
        self.assertFalse(
            [
                problem
                for problem in problems
                if "is absent from its pinned Pulp revision" in problem
            ],
            problems,
        )

    def test_truncated_checkout_still_reports_history_free_defects(self) -> None:
        """The negative control against an early return.

        The routing-owner and delete_paths checks need no history at all. If the
        shallow branch ever returns early instead of falling through, this test
        goes red -- which is the only thing standing between a truncated checkout
        and a fresh vacuous green.
        """

        document = copy.deepcopy(self.handoff)
        document["entries"][0]["delete_paths"] = ["core"]
        problems = catalog.validate_handoff_routing(document, self.truncated)
        self.assertTrue(
            [problem for problem in problems if "history is truncated" in problem],
            problems,
        )
        self.assertIn(
            "handoff entries[0].delete_paths contains broad directory 'core'",
            problems,
        )

    def test_full_checkout_reports_no_shallow_problem(self) -> None:
        """With no graft boundary the original messages survive verbatim."""

        problems = catalog.validate_handoff_routing(
            copy.deepcopy(self.handoff), self.complete
        )
        self.assertFalse(
            [problem for problem in problems if "history is truncated" in problem],
            problems,
        )
        self.assertTrue(
            [
                problem
                for problem in problems
                if "is absent from its pinned Pulp revision" in problem
            ],
            problems,
        )

    def test_shallow_marked_but_deep_checkout_reports_no_shallow_problem(self) -> None:
        """The regression test for the falsified ``--is-shallow-repository`` guard.

        This worktree reports ``true`` for ``git rev-parse
        --is-shallow-repository`` and still resolves all 102 pinned rows to real
        owning commits. A guard keyed on that flag would fail it, and would fail
        every CI runner checkout in the same state -- including the ones that
        produce the required ``macos`` gate.

        The state cannot be synthesised: being shallow-marked requires Git to
        have written a ``shallow`` file during a truncated fetch, while resolving
        past it requires the real deep history. So this asserts against the live
        worktree and states the two facts it rests on rather than assuming them.
        If this checkout is ever not shallow-marked, the falsification has
        nothing to say here, but the assertion that matters -- no shallow problem
        -- still holds and is still made.
        """

        marked = subprocess.run(
            ["git", "rev-parse", "--is-shallow-repository"],
            cwd=catalog.ROOT, capture_output=True, text=True, check=False,
        ).stdout.strip()
        boundaries = connected_git_history.shallow_boundaries(catalog.ROOT)
        self.assertEqual(marked == "true", bool(boundaries))

        rows = [
            row
            for entry in self.handoff["entries"]
            for row in entry["pulp_paths"]
            if isinstance(row, dict)
        ]
        self.assertGreater(len(rows), 0)
        on_boundary = [
            row["path"]
            for row in rows
            if connected_git_history.resolves_to_boundary(
                catalog.ROOT, str(row["revision"]), row["path"], boundaries
            )
        ]
        self.assertEqual(on_boundary, [])

        problems = catalog.validate_handoff_routing(
            copy.deepcopy(self.handoff), catalog.ROOT
        )
        self.assertFalse(
            [problem for problem in problems if "history is truncated" in problem],
            problems,
        )


if __name__ == "__main__":
    unittest.main()
