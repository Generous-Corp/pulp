from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.harness.differential.contract import (
    MANIFEST_SCHEMA,
    SCHEMA,
    load_fixture_manifest,
    normalize_report,
    observations_from_lab_reports,
    validate_report,
)


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "test/fixtures/import-differential/canvas-svg-manifest.json"


class DifferentialContractTests(unittest.TestCase):
    def test_bounded_manifest_loads_only_canvas_and_svg(self) -> None:
        raw, fixtures = load_fixture_manifest(MANIFEST)
        self.assertEqual(raw["schema"], MANIFEST_SCHEMA)
        self.assertEqual([f.id for f in fixtures], [
            "canvas-primitives", "canvas-state", "svg-shapes", "svg-invalid-path"
        ])
        self.assertEqual({f.surface for f in fixtures}, {"canvas", "svg"})

    def test_report_is_sorted_and_reproducible(self) -> None:
        observations = {
            "svg-invalid-path": {"status": "fail", "findings": [{"kind": "unsupported-behavior", "message": "malformed path", "path": "svg/path[0]"}]},
            "canvas-primitives": {"status": "pass", "evidence": ["browser.png"]},
        }
        first = normalize_report(MANIFEST, browser=observations, native=observations).to_json()
        second = normalize_report(MANIFEST, browser=observations, native=observations).to_json()
        self.assertEqual(first, second)
        report = json.loads(first)
        self.assertEqual(report["schema"], SCHEMA)
        self.assertEqual([row["id"] for row in report["fixtures"]], sorted(row["id"] for row in report["fixtures"]))
        self.assertEqual(report["reference"]["backend"], "chromium")
        invalid = next(row for row in report["fixtures"] if row["id"] == "svg-invalid-path")
        self.assertEqual(invalid["native"]["findings"][0]["kind"], "unsupported-behavior")

    def test_rejects_non_chromium_authority(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            raw = json.loads(MANIFEST.read_text())
            raw["reference"]["backend"] = "webkit"
            path.write_text(json.dumps(raw))
            for fixture in json.loads(MANIFEST.read_text())["fixtures"]:
                (Path(directory) / fixture["source"]).write_text("<html></html>")
            with self.assertRaisesRegex(ValueError, "chromium"):
                normalize_report(path)

    def test_validate_rejects_null_rows_strings_and_unbound_pass(self) -> None:
        report = json.loads(normalize_report(MANIFEST).to_json())
        report["fixtures"] = [None]
        with self.assertRaisesRegex(ValueError, "objects"):
            validate_report(report)
        report = json.loads(normalize_report(MANIFEST).to_json())
        report["fixtures"][0]["features"] = "fillRect"
        with self.assertRaisesRegex(ValueError, "string array"):
            validate_report(report)
        report = json.loads(normalize_report(MANIFEST).to_json())
        report["fixtures"][0]["browser"] = {"status": "pass", "findings": [], "evidence": []}
        with self.assertRaisesRegex(ValueError, "lacks evidence"):
            validate_report(report)

    def test_deliberate_native_pixel_mismatch_is_preserved(self) -> None:
        observations = {"canvas-primitives": {"status": "fail", "findings": [{"kind": "wrong-pixels", "message": "deliberate control"}], "evidence": ["comparison/report.json"]}}
        report = json.loads(normalize_report(MANIFEST, native=observations).to_json())
        row = next(item for item in report["fixtures"] if item["id"] == "canvas-primitives")
        self.assertEqual(row["native"]["status"], "fail")
        self.assertEqual(row["native"]["findings"][0]["kind"], "wrong-pixels")

    def test_lab_observation_mapping_rejects_missing_or_ambiguous_ids(self) -> None:
        with self.assertRaisesRegex(ValueError, "lacks a fixture id"):
            observations_from_lab_reports([{"classifications": []}])
        report = {"fixture": {"id": "canvas-primitives"}, "classifications": []}
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            observations_from_lab_reports([report, report])

    def test_validate_rejects_unsorted_rows_and_wrong_native(self) -> None:
        report = json.loads(normalize_report(MANIFEST).to_json())
        report["fixtures"].reverse()
        with self.assertRaisesRegex(ValueError, "sorted"):
            validate_report(report)
        report["fixtures"].reverse()
        report["native"]["backend"] = "skia"
        with self.assertRaisesRegex(ValueError, "native backend"):
            validate_report(report)


if __name__ == "__main__":
    unittest.main()
