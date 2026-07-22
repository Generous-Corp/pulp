#!/usr/bin/env python3
"""Tests for generate_vellum_cut_manifest.py."""

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("generate_vellum_cut_manifest.py")
SPEC = importlib.util.spec_from_file_location("generate_vellum_cut_manifest", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
manifest_tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(manifest_tool)


class CutManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.repo = Path(self.temporary.name)
        self._git("init", "-q")
        self._git("config", "user.name", "Manifest Test")
        self._git("config", "user.email", "manifest@example.invalid")
        self._write("core/canvas/z.cpp", "z\n")
        self._write("core/canvas/a.hpp", "a\n")
        self._write("core/view/platform/mac/host.mm", "host\n")
        self._write("misc/unknown.txt", "unknown\n")
        self._git("add", ".")
        self._git("commit", "-qm", "fixture")
        self.commit = self._git("rev-parse", "HEAD").strip()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _git(self, *args: str) -> str:
        return subprocess.check_output(
            ["git", "-C", str(self.repo), *args], text=True
        )

    def _write(self, relative: str, content: str) -> Path:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def _selection(self, content: str) -> Path:
        return self._write("selection.txt", content)

    def test_expands_directories_records_git_identity_and_sorts(self) -> None:
        selection = self._selection(
            "core/canvas\ncore/view/platform/mac/host.mm\n"
        )
        manifest = manifest_tool.generate_manifest(
            repo=self.repo,
            source_commit=self.commit,
            selection_path=selection,
        )

        entries = manifest["entries"]
        self.assertEqual(
            [entry["source_path"] for entry in entries],
            [
                "core/canvas/a.hpp",
                "core/canvas/z.cpp",
                "core/view/platform/mac/host.mm",
            ],
        )
        self.assertEqual(entries[0]["classification"], "framework-core")
        self.assertEqual(entries[2]["classification"], "platform-adapter")
        self.assertRegex(entries[0]["git_blob_sha"], r"^[0-9a-f]{40}$")
        self.assertEqual(entries[0]["git_mode"], "100644")

    def test_overlapping_selections_are_deduplicated_with_provenance(self) -> None:
        selection = self._selection("core/canvas\ncore/canvas/a.hpp\n")
        manifest = manifest_tool.generate_manifest(
            repo=self.repo,
            source_commit=self.commit,
            selection_path=selection,
        )

        self.assertEqual(manifest["entry_count"], 2)
        self.assertEqual(
            manifest["entries"][0]["selected_by"],
            ["core/canvas", "core/canvas/a.hpp"],
        )

    def test_missing_selection_is_rejected(self) -> None:
        selection = self._selection("core/canvas\ndoes/not/exist\n")
        with self.assertRaisesRegex(manifest_tool.ManifestError, "missing"):
            manifest_tool.generate_manifest(
                repo=self.repo,
                source_commit=self.commit,
                selection_path=selection,
            )

    def test_unclassified_blob_is_rejected(self) -> None:
        selection = self._selection("misc/unknown.txt\n")
        with self.assertRaisesRegex(manifest_tool.ManifestError, "no classification"):
            manifest_tool.generate_manifest(
                repo=self.repo,
                source_commit=self.commit,
                selection_path=selection,
            )

    def test_declared_classification_handles_deliberate_exception(self) -> None:
        selection = self._selection("excluded\tmisc/unknown.txt\n")
        manifest = manifest_tool.generate_manifest(
            repo=self.repo,
            source_commit=self.commit,
            selection_path=selection,
        )
        entry = manifest["entries"][0]
        self.assertEqual(entry["classification"], "excluded")
        self.assertEqual(
            entry["classification_source"], "declared:misc/unknown.txt"
        )

    def test_specific_cut_rules_precede_broad_subsystem_rules(self) -> None:
        cases = {
            "core/canvas/CMakeLists.txt": "unresolved",
            "core/canvas/include/pulp/canvas/lottie_animation.hpp": "optional",
            "core/canvas/include/pulp/canvas/scene/scene.hpp": "optional",
            "core/canvas/src/image_codecs_gif.cpp": "optional",
            "core/canvas/platform/mac/cg_canvas.mm": "optional",
            "core/canvas/src/skia_canvas.cpp": "framework-core",
            "core/canvas/include/pulp/canvas/font_scope.hpp": "unresolved",
            "core/canvas/src/font_scope.cpp": "unresolved",
            "core/render/include/pulp/render/renderer3d.hpp": "optional",
            "core/render/include/pulp/render/gpu_compute.hpp": "optional",
            "core/render/src/sdl3_surface.cpp": "excluded",
            "core/render/src/metal_surface_ios.mm": "excluded",
            "core/render/platform/android/gpu_surface_android.cpp": "excluded",
            "core/render/src/gpu_surface_dawn.cpp": "framework-core",
            "core/render/include/pulp/render/bench/perf_counters.hpp": "unresolved",
            "core/view/include/pulp/view/design_ir.hpp": "unresolved",
            "core/view/src/view.cpp": "unresolved",
            "core/view/src/widgets/label.cpp": "unresolved",
            "core/view/src/design_import_native_common.cpp": "unresolved",
            "core/view/include/pulp/view/theme.hpp": "unresolved",
            "core/view/src/theme.cpp": "unresolved",
            "core/view/platform/mac/window_host_mac.mm": "unresolved",
            "core/view/platform/mac/screenshot_mac.mm": "platform-adapter",
            "packages/pulp-import-ir/src/anchors.ts": "authoring-only",
            "packages/pulp-import-ir/src/types.ts": "unresolved",
            "tools/figma-plugin/schema/figma-plugin-export-v1.json": "pulp-specific",
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                result = manifest_tool.derive_classification(path)
                self.assertIsNotNone(result)
                self.assertEqual(result[0], expected)

    def test_serialization_is_byte_deterministic(self) -> None:
        selection = self._selection("core/canvas\n")
        first = manifest_tool.generate_manifest(
            repo=self.repo,
            source_commit=self.commit,
            selection_path=selection,
        )
        second = manifest_tool.generate_manifest(
            repo=self.repo,
            source_commit=self.commit,
            selection_path=selection,
        )
        self.assertEqual(
            manifest_tool.serialize_manifest(first),
            manifest_tool.serialize_manifest(second),
        )
        decoded = json.loads(manifest_tool.serialize_manifest(first))
        self.assertNotIn("generated_at", decoded)

    def test_verify_rejects_a_stale_committed_manifest(self) -> None:
        selection = self._selection("core/canvas\n")
        output = self.repo / "manifest.json"
        common_arguments = [
            "--repo",
            str(self.repo),
            "--source-commit",
            self.commit,
            "--selection",
            str(selection),
            "--output",
            str(output),
        ]
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(manifest_tool.main(common_arguments), 0)
            self.assertEqual(manifest_tool.main([*common_arguments, "--verify"]), 0)

        output.write_text("{}\n", encoding="utf-8")
        with redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(manifest_tool.main([*common_arguments, "--verify"]), 1)
        self.assertIn("manifest is stale", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
