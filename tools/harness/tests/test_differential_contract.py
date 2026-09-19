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
            "canvas-primitives": {"status": "pass"},
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
