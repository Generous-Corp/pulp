"""Tests for visual snapshot fixture discovery and coverage counts."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.visual import runner  # noqa: E402


class RunnerTests(unittest.TestCase):
    def test_resolve_fixtures_accepts_surface_prefixed_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture_dir = root / "tools" / "harness" / "visual" / "fixtures" / "yoga"
            fixture_dir.mkdir(parents=True)
            fixture = fixture_dir / "box-sizing.json"
            fixture.write_text("{}", encoding="utf-8")

            self.assertEqual(
                runner.resolve_fixtures(root, "yoga", ["yoga/box-sizing"]),
                [fixture],
            )

    def test_visual_pass_counts_uses_compat_total_and_checked_in_goldens(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "compat.json").write_text(
                json.dumps({"yoga": {"a": {}, "b": {}, "c": {}}}),
                encoding="utf-8",
            )
            golden_dir = root / "tools" / "harness" / "visual" / "goldens" / "yoga"
            golden_dir.mkdir(parents=True)
            (golden_dir / "a.json").write_text("{}", encoding="utf-8")
            (golden_dir / "b.json").write_text("{}", encoding="utf-8")

            counts = runner.visual_pass_counts(root, ["yoga"])

            self.assertEqual(counts["yoga"]["pass"], 2)
            self.assertEqual(counts["yoga"]["total"], 3)
            self.assertEqual(counts["yoga"]["label"], "2/3")


if __name__ == "__main__":
    unittest.main()
