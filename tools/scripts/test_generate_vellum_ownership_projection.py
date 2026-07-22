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


if __name__ == "__main__":
    unittest.main()
