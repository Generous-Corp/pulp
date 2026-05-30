#!/usr/bin/env python3
"""Tests for tools/scripts/frontend_ir_codegen_artifacts.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "frontend_ir_codegen_artifacts.py"
spec = importlib.util.spec_from_file_location("frontend_ir_codegen_artifacts", SCRIPT)
assert spec and spec.loader
codegen_artifacts = importlib.util.module_from_spec(spec)
sys.modules["frontend_ir_codegen_artifacts"] = codegen_artifacts
spec.loader.exec_module(codegen_artifacts)


def sample_frontend_ir() -> dict:
    return {
        "schema": "pulp-frontend-ir-v0",
        "fixture_id": "panel",
        "routes": [
            {"node_id": "panel.knob.freq", "chosen_route": "native_cpp"},
            {"node_id": "panel.meter.out", "chosen_route": "native_cpp"},
            {"node_id": "panel.waveform", "chosen_route": "recorded_paint"},
        ],
    }


def sample_binding_manifest() -> dict:
    return {
        "schema": "pulp-native-cpp-binding-manifest-v1",
        "entries": [
            {
                "id": "panel.knob.freq",
                "native_primitive": "knob",
                "route_type": "native_cpp",
            },
            {
                "id": "panel.meter.out.left",
                "native_primitive": "meter",
                "route_type": "native_cpp",
            },
            {
                "id": "panel.meter.out.right",
                "native_primitive": "meter",
                "route_type": "native_cpp",
            },
        ],
    }


class FrontendIrCodegenArtifactTests(unittest.TestCase):
    def _build_report(self, frontend_ir: dict, manifest: dict) -> dict:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            frontend_ir_path = root / "frontend-ir.json"
            source_cpp = root / "generated.cpp"
            header = root / "generated.hpp"
            binding_manifest = root / "generated.bindings.json"
            frontend_ir_path.write_text(json.dumps(frontend_ir), encoding="utf-8")
            source_cpp.write_text("// cpp\n", encoding="utf-8")
            header.write_text("// hpp\n", encoding="utf-8")
            binding_manifest.write_text(json.dumps(manifest), encoding="utf-8")
            return codegen_artifacts.build_codegen_artifact_report(
                frontend_ir,
                manifest,
                frontend_ir_path=frontend_ir_path,
                source_cpp=source_cpp,
                header=header,
                binding_manifest_path=binding_manifest,
                repo_root=root,
            )

    def test_builds_codegen_artifact_report(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            frontend_ir = root / "frontend-ir.json"
            source_cpp = root / "generated.cpp"
            header = root / "generated.hpp"
            binding_manifest = root / "generated.bindings.json"
            frontend_ir.write_text(json.dumps(sample_frontend_ir()), encoding="utf-8")
            source_cpp.write_text("// cpp\n", encoding="utf-8")
            header.write_text("// hpp\n", encoding="utf-8")
            binding_manifest.write_text(json.dumps(sample_binding_manifest()), encoding="utf-8")

            report = codegen_artifacts.build_codegen_artifact_report(
                sample_frontend_ir(),
                sample_binding_manifest(),
                frontend_ir_path=frontend_ir,
                source_cpp=source_cpp,
                header=header,
                binding_manifest_path=binding_manifest,
                repo_root=root,
            )

        self.assertEqual(report["schema"], "pulp-frontend-ir-codegen-artifacts-v0")
        self.assertEqual(report["fixture_id"], "panel")
        self.assertEqual(report["summary"]["native_cpp_routes"], 2)
        self.assertEqual(report["summary"]["binding_entries"], 3)
        self.assertEqual(report["summary"]["directly_bound_native_routes"], 1)
        self.assertEqual(report["summary"]["missing_native_route_bindings"], 1)
        self.assertEqual(report["summary"]["extra_binding_entries"], 2)
        self.assertEqual(report["summary"]["split_binding_candidates"], 1)
        self.assertEqual(report["missing_native_route_bindings"], ["panel.meter.out"])
        self.assertEqual(report["extra_binding_entries"], ["panel.meter.out.left", "panel.meter.out.right"])
        self.assertEqual(report["split_binding_candidates"][0]["route_id"], "panel.meter.out")
        # Default fixture has no source_route_id, so the split is inferred by the
        # historical id-prefix convention.
        self.assertEqual(report["split_binding_candidates"][0]["provenance"], "id_prefix")
        self.assertNotIn("parts", report["split_binding_candidates"][0])
        self.assertEqual(report["summary"]["explicit_split_bindings"], 0)
        self.assertEqual(report["summary"]["prefix_split_bindings"], 1)
        self.assertEqual(report["summary"]["binding_primitives"], {"knob": 1, "meter": 2})

    def test_cli_writes_codegen_artifact_report(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            frontend_ir = root / "frontend-ir.json"
            source_cpp = root / "generated.cpp"
            header = root / "generated.hpp"
            binding_manifest = root / "generated.bindings.json"
            output = root / "codegen-artifacts.json"
            frontend_ir.write_text(json.dumps(sample_frontend_ir()), encoding="utf-8")
            source_cpp.write_text("// cpp\n", encoding="utf-8")
            header.write_text("// hpp\n", encoding="utf-8")
            binding_manifest.write_text(json.dumps(sample_binding_manifest()), encoding="utf-8")

            rc = codegen_artifacts.main([
                "--frontend-ir",
                str(frontend_ir),
                "--source-cpp",
                str(source_cpp),
                "--header",
                str(header),
                "--binding-manifest",
                str(binding_manifest),
                "--output",
                str(output),
                "--repo-root",
                str(root),
            ])

            self.assertEqual(rc, 0)
            written = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(written["frontend_ir"]["path"], "frontend-ir.json")
            self.assertEqual(written["artifacts"]["source_cpp"]["byte_size"], 7)

    def test_explicit_source_route_id_split_is_modeled(self) -> None:
        frontend_ir = sample_frontend_ir()
        manifest = {
            "schema": "pulp-native-cpp-binding-manifest-v1",
            "entries": [
                {
                    "id": "panel.knob.freq",
                    "native_primitive": "knob",
                    "route_type": "native_cpp",
                },
                {
                    "id": "panel.meter.out.left",
                    "native_primitive": "meter",
                    "route_type": "native_cpp",
                    "source_route_id": "panel.meter.out",
                    "channel": "left",
                },
                {
                    "id": "panel.meter.out.right",
                    "native_primitive": "meter",
                    "route_type": "native_cpp",
                    "source_route_id": "panel.meter.out",
                    "channel": "right",
                },
            ],
        }

        report = self._build_report(frontend_ir, manifest)

        candidate = report["split_binding_candidates"][0]
        self.assertEqual(candidate["route_id"], "panel.meter.out")
        self.assertEqual(candidate["provenance"], "explicit_source_route_id")
        self.assertEqual(
            candidate["binding_entry_ids"],
            ["panel.meter.out.left", "panel.meter.out.right"],
        )
        self.assertEqual(
            candidate["parts"],
            [
                {"binding_entry_id": "panel.meter.out.left", "channel": "left"},
                {"binding_entry_id": "panel.meter.out.right", "channel": "right"},
            ],
        )
        self.assertEqual(report["summary"]["explicit_split_bindings"], 1)
        self.assertEqual(report["summary"]["prefix_split_bindings"], 0)
        self.assertEqual(report["summary"]["split_binding_candidates"], 1)

    def test_explicit_xy_axis_split_is_modeled(self) -> None:
        frontend_ir = {
            "schema": "pulp-frontend-ir-v0",
            "fixture_id": "panel",
            "routes": [
                {"node_id": "panel.xy.pan", "chosen_route": "native_cpp"},
            ],
        }
        manifest = {
            "schema": "pulp-native-cpp-binding-manifest-v1",
            "entries": [
                {
                    "id": "panel.xy.pan.x",
                    "native_primitive": "xy_pad",
                    "route_type": "native_cpp",
                    "source_route_id": "panel.xy.pan",
                    "axis": "x",
                },
                {
                    "id": "panel.xy.pan.y",
                    "native_primitive": "xy_pad",
                    "route_type": "native_cpp",
                    "source_route_id": "panel.xy.pan",
                    "axis": "y",
                },
            ],
        }

        report = self._build_report(frontend_ir, manifest)

        candidate = report["split_binding_candidates"][0]
        self.assertEqual(candidate["route_id"], "panel.xy.pan")
        self.assertEqual(candidate["provenance"], "explicit_source_route_id")
        self.assertEqual(
            candidate["parts"],
            [
                {"binding_entry_id": "panel.xy.pan.x", "axis": "x"},
                {"binding_entry_id": "panel.xy.pan.y", "axis": "y"},
            ],
        )
        self.assertEqual(report["summary"]["explicit_split_bindings"], 1)
        self.assertEqual(report["summary"]["prefix_split_bindings"], 0)

    def test_rejects_unexpected_binding_manifest_schema(self) -> None:
        with self.assertRaisesRegex(ValueError, "binding manifest schema"):
            codegen_artifacts.build_codegen_artifact_report(
                sample_frontend_ir(),
                {"schema": "wrong", "entries": []},
                frontend_ir_path=pathlib.Path("frontend-ir.json"),
                source_cpp=pathlib.Path(__file__),
                header=pathlib.Path(__file__),
                binding_manifest_path=pathlib.Path(__file__),
                repo_root=pathlib.Path(__file__).resolve().parent,
            )


if __name__ == "__main__":
    unittest.main()
