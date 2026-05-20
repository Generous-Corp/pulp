#!/usr/bin/env python3
"""Tests for compat_aggregate.py (roadmap P5-NEW-B).

Covers:
    1. Verbatim slice / assemble round-trip (byte-stable).
    2. The real repo-root compat.json round-trips byte-identically
       through split + build.
    3. `check` passes when in sync and fails on drift.
    4. The em-dash / \\uXXXX escape inconsistency that defeats
       json.dumps re-serialization is preserved by verbatim slicing.

Run:
    python3 tools/scripts/test_compat_aggregate.py
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "compat_aggregate.py"
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))

import compat_aggregate as ca  # noqa: E402


# A minimal aggregate with the SAME structural shape as the real
# compat.json: a non-surface metadata block, the six surfaces, and a
# `react` block — in the canonical key order. Deliberately mixes a
# literal em-dash and an escaped — so re-serialization would NOT
# be byte-stable but verbatim slicing is.
SAMPLE_AGGREGATE = (
    "{\n"
    '  "compat-schema-version": "0.3",\n'
    '  "_comment": "literal em-dash — here",\n'
    '  "_audit": {\n'
    '    "generated": "2026-05-04"\n'
    "  },\n"
    '  "css": {\n'
    '    "display": {\n'
    '      "note": "escaped \\u2014 dash"\n'
    "    }\n"
    "  },\n"
    '  "rn": {\n'
    '    "View": {}\n'
    "  },\n"
    '  "yoga": {\n'
    '    "flex": {}\n'
    "  },\n"
    '  "react": {\n'
    '    "_note": "react surface"\n'
    "  },\n"
    '  "html": {\n'
    '    "div": {}\n'
    "  },\n"
    '  "canvas2d": {\n'
    '    "fillRect": {}\n'
    "  },\n"
    '  "imports": {\n'
    '    "figma": {}\n'
    "  }\n"
    "}"
)


class SliceAssembleTests(unittest.TestCase):
    def test_round_trip_is_byte_identical(self) -> None:
        blocks = ca.slice_aggregate(SAMPLE_AGGREGATE)
        self.assertEqual(ca.assemble_aggregate(blocks), SAMPLE_AGGREGATE)

    def test_all_top_level_keys_present(self) -> None:
        blocks = ca.slice_aggregate(SAMPLE_AGGREGATE)
        self.assertEqual(set(blocks), set(ca.ALL_KEYS_ORDER))

    def test_preserves_escape_inconsistency(self) -> None:
        # The literal em-dash and the escaped — must both survive
        # verbatim — this is exactly why we slice instead of json.dumps.
        blocks = ca.slice_aggregate(SAMPLE_AGGREGATE)
        self.assertIn("—", blocks["_comment"])
        self.assertIn("\\u2014", blocks["css"])

    def test_rejects_bad_prefix(self) -> None:
        with self.assertRaises(ValueError):
            ca.slice_aggregate('  "css": {}\n}')  # no '{\n' prefix

    def test_part_round_trip(self) -> None:
        blocks = ca.slice_aggregate(SAMPLE_AGGREGATE)
        for key in ca.SURFACE_KEYS:
            text = ca.part_text_for([key], blocks)
            recovered = ca.blocks_from_part(text, [key])
            self.assertEqual(recovered[key], blocks[key])
        meta_text = ca.part_text_for(ca.META_KEYS, blocks)
        recovered = ca.blocks_from_part(meta_text, ca.META_KEYS)
        for key in ca.META_KEYS:
            self.assertEqual(recovered[key], blocks[key])


class CliRoundTripTests(unittest.TestCase):
    def _run(self, command: str, root: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), command, "--repo-root", str(root)],
            capture_output=True, text=True,
        )

    def test_split_build_check_on_sample(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            agg = root / "compat.json"
            agg.write_text(SAMPLE_AGGREGATE, encoding="utf-8")
            original = agg.read_text(encoding="utf-8")

            self.assertEqual(self._run("split", root).returncode, 0)
            parts = sorted(p.name for p in (root / "compat").iterdir())
            self.assertEqual(parts, [
                "_meta.json", "canvas2d.json", "css.json", "html.json",
                "imports.json", "rn.json", "yoga.json",
            ])

            self.assertEqual(self._run("build", root).returncode, 0)
            self.assertEqual(
                agg.read_text(encoding="utf-8"), original,
                "build must regenerate a byte-identical compat.json",
            )
            self.assertEqual(self._run("check", root).returncode, 0)

    def test_check_detects_drift(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "compat.json").write_text(
                SAMPLE_AGGREGATE, encoding="utf-8")
            self.assertEqual(self._run("split", root).returncode, 0)
            # Mutate a part so it no longer matches the aggregate.
            css = root / "compat" / "css.json"
            css.write_text(
                css.read_text(encoding="utf-8").replace("display", "DISPLAY"),
                encoding="utf-8",
            )
            res = self._run("check", root)
            self.assertEqual(res.returncode, 1, res.stderr)
            self.assertIn("DRIFT", res.stderr)

    def test_check_missing_parts_dir(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "compat.json").write_text(
                SAMPLE_AGGREGATE, encoding="utf-8")
            res = self._run("check", root)
            self.assertEqual(res.returncode, 2, res.stderr)


class RealCompatJsonTests(unittest.TestCase):
    """Round-trip the actual repo compat.json byte-for-byte."""

    def test_real_compat_json_is_byte_stable(self) -> None:
        src = REPO_ROOT / "compat.json"
        if not src.exists():
            self.skipTest("compat.json not present")
        original = src.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shutil.copy(src, root / "compat.json")
            res = subprocess.run(
                [sys.executable, str(SCRIPT), "split",
                 "--repo-root", str(root)],
                capture_output=True, text=True,
            )
            self.assertEqual(res.returncode, 0, res.stderr)
            subprocess.run(
                [sys.executable, str(SCRIPT), "build",
                 "--repo-root", str(root)],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(
                (root / "compat.json").read_text(encoding="utf-8"),
                original,
                "real compat.json must survive split+build byte-identically",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
