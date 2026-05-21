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

import contextlib
import io
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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

    def test_rejects_bad_suffix_missing_key_and_bad_separator(self) -> None:
        with self.assertRaisesRegex(ValueError, "does not end"):
            ca.slice_aggregate(SAMPLE_AGGREGATE[:-1])

        without_imports = SAMPLE_AGGREGATE.replace(
            ',\n  "imports": {\n    "figma": {}\n  }',
            "",
        )
        with self.assertRaisesRegex(ValueError, "top-level key 'imports' not found"):
            ca.slice_aggregate(without_imports)

        bad_separator = SAMPLE_AGGREGATE.replace(
            ',\n  "_comment":',
            '\n  "_comment":',
            1,
        )
        with self.assertRaisesRegex(ValueError, "expected ',\\\\n'"):
            ca.slice_aggregate(bad_separator)

    def test_rejects_unexpected_top_level_key_order(self) -> None:
        blocks = ca.slice_aggregate(SAMPLE_AGGREGATE)
        swapped = "{\n" + ",\n".join(
            blocks[k]
            for k in [
                "compat-schema-version", "_comment", "_audit", "rn", "css",
                "yoga", "react", "html", "canvas2d", "imports",
            ]
        ) + "\n}"

        with self.assertRaisesRegex(ValueError, "unexpected top-level key order"):
            ca.slice_aggregate(swapped)

    def test_assemble_and_part_validation_errors(self) -> None:
        blocks = ca.slice_aggregate(SAMPLE_AGGREGATE)
        blocks.pop("imports")
        with self.assertRaisesRegex(ValueError, "missing key block"):
            ca.assemble_aggregate(blocks)

        with self.assertRaisesRegex(ValueError, "not a"):
            ca.blocks_from_part('{"css": {}}\n', ["css"])

        with self.assertRaisesRegex(ValueError, "key 'css' not found"):
            ca.blocks_from_part('{\n  "rn": {}\n}\n', ["css"])

    def test_aggregate_from_parts_reports_missing_surface_and_meta(self) -> None:
        blocks = ca.slice_aggregate(SAMPLE_AGGREGATE)
        with tempfile.TemporaryDirectory() as tmp:
            parts_dir = Path(tmp)

            with self.assertRaisesRegex(FileNotFoundError, "missing part"):
                ca._aggregate_from_parts(parts_dir)

            for key in ca.SURFACE_KEYS:
                (parts_dir / f"{key}.json").write_text(
                    ca.part_text_for([key], blocks), encoding="utf-8",
                )

            with self.assertRaisesRegex(FileNotFoundError, "missing part"):
                ca._aggregate_from_parts(parts_dir)

    def test_slicers_tolerate_first_key_found_without_line_prefix(self) -> None:
        malformed_but_recoverable = '{\nxx  "css": {}\n}'
        with mock.patch.object(ca, "ALL_KEYS_ORDER", ["css"]):
            self.assertEqual(
                ca.slice_aggregate(malformed_but_recoverable),
                {"css": '  "css": {}'},
            )

        self.assertEqual(
            ca.blocks_from_part(malformed_but_recoverable + "\n", ["css"]),
            {"css": '  "css": {}'},
        )

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

    def test_build_missing_parts_dir_and_check_missing_aggregate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(self._run("build", root).returncode, 1)
            self.assertEqual(self._run("check", root).returncode, 2)

    def test_check_reports_structural_part_errors(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "compat.json").write_text(
                SAMPLE_AGGREGATE, encoding="utf-8")
            self.assertEqual(self._run("split", root).returncode, 0)
            (root / "compat" / "css.json").write_text("not json-shaped\n",
                                                       encoding="utf-8")

            res = self._run("check", root)

            self.assertEqual(res.returncode, 2)
            self.assertIn("cannot rebuild from parts", res.stderr)

    def test_split_reports_non_identical_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "compat.json").write_text(
                SAMPLE_AGGREGATE, encoding="utf-8")
            stderr = io.StringIO()

            with mock.patch.object(ca, "_aggregate_from_parts", return_value="{}\n"), \
                 contextlib.redirect_stderr(stderr):
                rc = ca.cmd_split(root)

            self.assertEqual(rc, 1)
            self.assertIn("non-byte-identical", stderr.getvalue())


class RepoRootTests(unittest.TestCase):
    def test_repo_root_prefers_git_toplevel(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git"], 0, stdout="/tmp/pulp-root\n", stderr="",
        )
        with mock.patch.object(ca.subprocess, "run", return_value=completed):
            self.assertEqual(ca.repo_root(), Path("/tmp/pulp-root"))

    def test_repo_root_falls_back_to_script_parent_with_compat_json(self) -> None:
        with mock.patch.object(ca.subprocess, "run", side_effect=FileNotFoundError):
            self.assertEqual(ca.repo_root(), REPO_ROOT)

    def test_repo_root_exits_when_git_and_parent_walk_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake_script = Path(tmp) / "tools" / "scripts" / "compat_aggregate.py"
            fake_script.parent.mkdir(parents=True)
            fake_script.write_text("", encoding="utf-8")

            with mock.patch.object(ca.subprocess, "run", side_effect=FileNotFoundError), \
                 mock.patch.object(ca, "__file__", str(fake_script)):
                with self.assertRaisesRegex(SystemExit, "cannot locate repo root"):
                    ca.repo_root()


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
