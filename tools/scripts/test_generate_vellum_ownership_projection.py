#!/usr/bin/env python3
"""Tests for generate_vellum_ownership_projection.py."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("generate_vellum_ownership_projection.py")
SPEC = importlib.util.spec_from_file_location("generate_vellum_ownership_projection", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
projection_tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(projection_tool)


def entry(path: str, classification: str, source: str = "derived:test") -> dict[str, object]:
    return {
        "source_path": path,
        "classification": classification,
        "classification_source": source,
        "git_blob_sha": "a" * 40,
        "git_mode": "100644",
        "selected_by": [path],
    }


def manifest(entries: list[dict[str, object]]) -> dict[str, object]:
    return {
        "schema": "pulp.vellum.initial-cut-manifest.v1",
        "source_commit": "b" * 40,
        "entry_count": len(entries),
        "entries": entries,
    }


def activation_event(*, slices: list[str] | None = None) -> dict[str, object]:
    return {
        "schema_version": 1,
        "event_id": "20260723-authority-activation",
        "kind": "authority-transition",
        "created_at": "2026-07-23T22:00:00Z",
        "slices": slices or ["canvas-kernel"],
        "rationale": "Activate Vellum source authority after independent validation.",
        "tests": ["Vellum freeze", "Vellum trusted freeze"],
        "transition": "activate",
        "vellum_authority_commit": "c" * 40,
        "approved_by": "@danielraffel",
        "counterpart": (
            "provenance/authority/records/native-design-kernel-v1.json"
        ),
    }


class OwnershipProjectionTests(unittest.TestCase):
    def test_candidate_paths_are_exact_and_forbidden_classes_are_deferred(self) -> None:
        value = manifest(
            [
                entry("core/canvas/src/skia_canvas.cpp", "framework-core"),
                entry("core/canvas/src/scene.cpp", "optional"),
                entry("core/render/src/sdl3_surface.cpp", "excluded"),
                entry("core/view/src/view.cpp", "unresolved"),
                entry(
                    "tools/figma-plugin/schema/figma-plugin-export-v1.json",
                    "pulp-specific",
                ),
            ]
        )
        projection = projection_tool.build_projection(value)
        slices = {item["id"]: item for item in projection["slices"]}

        self.assertEqual(
            slices["canvas-kernel"]["paths"],
            ["core/canvas/src/skia_canvas.cpp"],
        )
        self.assertEqual(slices["canvas-kernel"]["state"], "pulp-authoritative-untransferred")
        self.assertEqual(slices["canvas-kernel-deferred"]["state"], "excluded")
        self.assertEqual(slices["render-skia-dawn-deferred"]["state"], "excluded")
        self.assertEqual(slices["retained-ui-kernel"]["state"], "pulp-authoritative-untransferred")
        self.assertEqual(slices["legacy-figma-schema"]["state"], "pulp-only")
        for item in slices.values():
            if item["state"] == "pulp-authoritative-untransferred":
                self.assertTrue(all(not path.endswith("/") for path in item["paths"]))

    def test_forbidden_class_in_candidate_slice_fails_closed(self) -> None:
        value = manifest([entry("core/view/src/view.cpp", "optional")])
        with self.assertRaisesRegex(projection_tool.ProjectionError, "forbidden optional"):
            projection_tool.build_projection(value)

    def test_all_rows_have_one_owner_and_candidate_paths_do_not_overlap(self) -> None:
        value = manifest(
            [
                entry("LICENSE.md", "framework-core"),
                entry("core/view/src/view.cpp", "unresolved"),
                entry("external/fonts/Inter-Regular.ttf", "framework-core"),
            ]
        )
        projection = projection_tool.build_projection(value)
        with tempfile.TemporaryDirectory() as temporary:
            projection_tool.verify_projection(
                repo=Path(temporary), manifest=value, projection=projection
            )

        retained = next(
            item for item in projection["slices"] if item["id"] == "retained-ui-kernel"
        )
        retained["paths"].append("LICENSE.md")
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(projection_tool.ProjectionError, "stale"):
                projection_tool.verify_projection(
                    repo=Path(temporary), manifest=value, projection=projection
                )

    def test_neutrality_scan_reports_transferable_audio_plugin_identifiers(self) -> None:
        value = manifest([entry("core/canvas/src/example.cpp", "framework-core")])
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            source = repo / "core/canvas/src/example.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(
                "// audio plugin prose is ignored\nint plugin_id = 0;\n",
                encoding="utf-8",
            )
            self.assertEqual(
                projection_tool.neutrality_findings(repo, value),
                [("core/canvas/src/example.cpp", "plugin_id")],
            )

    def test_neutrality_scan_ignores_unresolved_and_figma_plugin_identifier(self) -> None:
        value = manifest(
            [
                entry("core/view/src/theme.cpp", "unresolved"),
                entry("core/view/src/anchor_strategy.cpp", "framework-core"),
            ]
        )
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            for path, content in {
                "core/view/src/theme.cpp": "int pro_audio = 0;\n",
                "core/view/src/anchor_strategy.cpp": "int figma_plugin = 0;\n",
            }.items():
                target = repo / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content, encoding="utf-8")
            self.assertEqual(projection_tool.neutrality_findings(repo, value), [])

    def test_repository_projection_covers_all_235_rows_exactly_once(self) -> None:
        repo = SCRIPT.parents[2]
        value = projection_tool.load_manifest(
            repo / "docs/contracts/vellum-initial-cut-manifest.json"
        )
        projection = projection_tool.build_projection(value)
        projection_tool.verify_projection(repo=repo, manifest=value, projection=projection)
        covered = [
            path
            for item in projection["slices"]
            for path in item["paths"]
            if path in {entry["source_path"] for entry in value["entries"]}
        ]
        self.assertEqual(len(value["entries"]), 235)
        self.assertEqual(len(covered), 235)
        self.assertEqual(len(set(covered)), 235)

    def test_serialization_is_deterministic(self) -> None:
        value = manifest([entry("LICENSE.md", "framework-core")])
        first = projection_tool.serialize(projection_tool.build_projection(value))
        second = projection_tool.serialize(projection_tool.build_projection(value))
        self.assertEqual(first, second)
        self.assertEqual(json.loads(first)["activation"]["state"], "prepared")

    def test_prepared_v1_projection_remains_verifiable_during_bootstrap(self) -> None:
        value = manifest([entry("LICENSE.md", "framework-core")])
        legacy = projection_tool.build_projection(value)
        legacy["schema_version"] = 1
        legacy["activation"].pop("authority_record_path")
        legacy["activation"].pop("initial_transition_event")
        for item in legacy["slices"]:
            item.pop("authority")

        with tempfile.TemporaryDirectory() as temporary:
            projection_tool.verify_projection(
                repo=Path(temporary),
                manifest=value,
                projection=legacy,
            )

        legacy["activation"]["state"] = "active"
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(projection_tool.ProjectionError, "only the prepared"):
                projection_tool.verify_projection(
                    repo=Path(temporary),
                    manifest=value,
                    projection=legacy,
                )

    def test_activation_is_derived_from_one_event(self) -> None:
        value = manifest([entry("core/canvas/src/skia_canvas.cpp", "framework-core")])
        projection = projection_tool.activate_projection(
            projection_tool.build_projection(value),
            activation_event(),
        )
        activation = projection["activation"]
        self.assertEqual(activation["state"], "active")
        self.assertEqual(
            activation["initial_transition_event"],
            "20260723-authority-activation",
        )
        canvas = next(
            item for item in projection["slices"] if item["id"] == "canvas-kernel"
        )
        self.assertEqual(canvas["state"], "framework-authoritative-transferred")
        self.assertEqual(
            canvas["authority"],
            {
                "event_id": "20260723-authority-activation",
                "vellum_commit": "c" * 40,
                "counterpart": (
                    "provenance/authority/records/native-design-kernel-v1.json"
                ),
                "accepted_by": "@danielraffel",
                "accepted_at": "2026-07-23T22:00:00Z",
            },
        )

    def test_activation_rejects_unknown_or_non_candidate_slice(self) -> None:
        value = manifest([entry("core/canvas/src/skia_canvas.cpp", "framework-core")])
        with self.assertRaisesRegex(projection_tool.ProjectionError, "unknown"):
            projection_tool.activate_projection(
                projection_tool.build_projection(value),
                activation_event(slices=["missing"]),
            )
        projection = projection_tool.build_projection(value)
        canvas = next(
            item for item in projection["slices"] if item["id"] == "canvas-kernel"
        )
        canvas["state"] = "excluded"
        with self.assertRaisesRegex(projection_tool.ProjectionError, "not a prepared"):
            projection_tool.activate_projection(projection, activation_event())

    def test_verify_active_projection_replays_committed_event(self) -> None:
        value = manifest([entry("core/canvas/src/skia_canvas.cpp", "framework-core")])
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            event_path = (
                repo
                / ".github/vellum-change-events/20260723-authority-activation.json"
            )
            event_path.parent.mkdir(parents=True)
            event_path.write_text(
                json.dumps(activation_event(), indent=2) + "\n",
                encoding="utf-8",
            )
            source = repo / "core/canvas/src/skia_canvas.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("int render_surface = 0;\n", encoding="utf-8")
            projection = projection_tool.activate_projection(
                projection_tool.build_projection(value),
                activation_event(),
            )
            projection_tool.verify_projection(
                repo=repo,
                manifest=value,
                projection=projection,
            )
            projection["activation"]["accepted_at"] = "2026-07-23T22:00:01Z"
            with self.assertRaisesRegex(projection_tool.ProjectionError, "stale"):
                projection_tool.verify_projection(
                    repo=repo,
                    manifest=value,
                    projection=projection,
                )

    def test_cli_verify_does_not_ignore_explicit_activation_event(self) -> None:
        value = manifest([entry("core/canvas/src/skia_canvas.cpp", "framework-core")])
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            manifest_path = repo / "manifest.json"
            manifest_path.write_text(
                json.dumps(value, indent=2) + "\n", encoding="utf-8"
            )
            source = repo / "core/canvas/src/skia_canvas.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("int render_surface = 0;\n", encoding="utf-8")
            event_a = activation_event()
            event_b = activation_event()
            event_b["event_id"] = "20260723-other-activation"
            event_b["created_at"] = "2026-07-23T22:00:01Z"
            path_a = repo / "event-a" / f"{event_a['event_id']}.json"
            path_b = repo / "event-b" / f"{event_b['event_id']}.json"
            path_a.parent.mkdir()
            path_b.parent.mkdir()
            path_a.write_text(json.dumps(event_a) + "\n", encoding="utf-8")
            path_b.write_text(json.dumps(event_b) + "\n", encoding="utf-8")
            output = repo / "ownership.json"
            output.write_bytes(
                projection_tool.serialize(
                    projection_tool.activate_projection(
                        projection_tool.build_projection(value),
                        projection_tool.load_activation_event(path_b),
                    )
                )
            )
            self.assertEqual(
                projection_tool.main(
                    [
                        "--repo",
                        str(repo),
                        "--manifest",
                        str(manifest_path),
                        "--output",
                        str(output),
                        "--activation-event",
                        str(path_a),
                        "--verify",
                    ]
                ),
                1,
            )

    def test_event_loader_matches_freeze_gate_basics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for field, value, message in (
                ("created_at", "not-a-timeZ", "valid timestamp"),
                ("rationale", "  ", "rationale"),
                ("tests", [], "tests"),
            ):
                event = activation_event()
                event[field] = value
                path = root / f"{event['event_id']}.json"
                path.write_text(json.dumps(event) + "\n", encoding="utf-8")
                with self.assertRaisesRegex(projection_tool.ProjectionError, message):
                    projection_tool.load_activation_event(path)


if __name__ == "__main__":
    unittest.main()
