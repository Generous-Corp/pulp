#!/usr/bin/env python3
"""Unit tests for diff_against_reference_regions.py — schema validation and
--strict flag semantics.

Pins the Codex P2 contract on PR #1871:
  - load_regions() validates region type/coord values upfront and exits
    2 with a clear message before any scoring runs.
  - --strict flag is actually consulted; without it region misses are
    informational and the script exits 0.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
SCRIPT = THIS_DIR / "diff_against_reference_regions.py"


def _run(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def _make_png(path: Path, color: tuple[int, int, int]) -> None:
    """Tiny PNG via PIL — keeps tests self-contained."""
    from PIL import Image  # type: ignore

    img = Image.new("RGB", (32, 32), color)
    img.save(path)


try:
    import PIL  # noqa: F401

    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


class TestSchemaValidation(unittest.TestCase):
    """load_regions() upfront schema validation."""

    def _write_regions(self, tmp: Path, data: object) -> Path:
        p = tmp / "regions.json"
        p.write_text(json.dumps(data))
        return p

    def test_non_dict_region_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(tmp, {"r1": "not a dict"})
            ref = tmp / "ref.png"
            cand = tmp / "cand.png"
            ref.write_text("placeholder")
            cand.write_text("placeholder")
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertIn("r1", result.stderr)
            self.assertIn("object", result.stderr)

    def test_non_numeric_coord_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(
                tmp,
                {"r1": {"x": "0.1", "y": 0.0, "w": 0.5, "h": 0.5}},
            )
            ref = tmp / "ref.png"
            cand = tmp / "cand.png"
            ref.write_text("placeholder")
            cand.write_text("placeholder")
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertIn("non-numeric", result.stderr)

    def test_bool_coord_rejected(self) -> None:
        # bool is an int subclass; remote fix rejects it explicitly so
        # `"x": true` doesn't silently become 1.
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(
                tmp,
                {"r1": {"x": True, "y": 0.0, "w": 0.5, "h": 0.5}},
            )
            ref = tmp / "ref.png"
            cand = tmp / "cand.png"
            ref.write_text("placeholder")
            cand.write_text("placeholder")
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertIn("non-numeric", result.stderr)

    def test_invalid_threshold_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(
                tmp,
                {
                    "r1": {
                        "x": 0.0,
                        "y": 0.0,
                        "w": 0.5,
                        "h": 0.5,
                        "threshold": "tight",
                    }
                },
            )
            ref = tmp / "ref.png"
            cand = tmp / "cand.png"
            ref.write_text("placeholder")
            cand.write_text("placeholder")
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertIn("threshold", result.stderr)

    def test_valid_regions_pass_schema_check(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            regions = self._write_regions(
                tmp,
                {"r1": {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0, "threshold": 0.5}},
            )
            # Missing PNGs still cause exit 2 (file-not-found), but the
            # error message comes AFTER schema validation. Asserting the
            # *absence* of a schema-error substring locks the ordering.
            ref = tmp / "missing-ref.png"
            cand = tmp / "missing-cand.png"
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("non-numeric", result.stderr)
            self.assertNotIn("must be an object", result.stderr)


@unittest.skipUnless(HAVE_PIL, "PIL/Pillow required for image-based tests")
class TestStrictFlag(unittest.TestCase):
    """--strict flag changes exit code semantics."""

    def _setup_mismatched(self, tmp: Path) -> tuple[Path, Path, Path]:
        ref = tmp / "ref.png"
        cand = tmp / "cand.png"
        _make_png(ref, color=(255, 255, 255))
        _make_png(cand, color=(0, 0, 0))
        regions = tmp / "regions.json"
        regions.write_text(
            json.dumps(
                {
                    "full": {
                        "x": 0.0,
                        "y": 0.0,
                        "w": 1.0,
                        "h": 1.0,
                        "threshold": 0.99,
                    }
                }
            )
        )
        return ref, cand, regions

    def test_without_strict_exits_zero_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            ref, cand, regions = self._setup_mismatched(tmp)
            result = _run([str(ref), str(cand), "--regions", str(regions)])
            self.assertEqual(
                result.returncode,
                0,
                f"expected exit 0 without --strict; got {result.returncode}\n"
                f"stdout={result.stdout}\nstderr={result.stderr}",
            )
            # Failure still reported on stdout.
            self.assertIn("FAIL", result.stdout)

    def test_with_strict_exits_one_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tdir:
            tmp = Path(tdir)
            ref, cand, regions = self._setup_mismatched(tmp)
            result = _run([str(ref), str(cand), "--regions", str(regions), "--strict"])
            self.assertEqual(
                result.returncode,
                1,
                f"expected exit 1 with --strict; got {result.returncode}\n"
                f"stdout={result.stdout}\nstderr={result.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
