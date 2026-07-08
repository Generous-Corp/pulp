#!/usr/bin/env python3
"""Tests for release_checksum_manifest.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).parent / "release_checksum_manifest.py"
spec = importlib.util.spec_from_file_location("release_checksum_manifest", SCRIPT)
assert spec and spec.loader
rcm = importlib.util.module_from_spec(spec)
sys.modules["release_checksum_manifest"] = rcm
spec.loader.exec_module(rcm)


class ReleaseChecksumManifestTests(unittest.TestCase):
    def test_generate_writes_sorted_manifest_and_excludes_existing_checksum(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            (root / "z.bin").write_text("z", encoding="utf-8")
            (root / "a.bin").write_text("a", encoding="utf-8")
            (root / "SHA256SUMS").write_text("old", encoding="utf-8")

            out = root / "SHA256SUMS"
            lines = rcm.generate_manifest(
                root,
                out,
                {"a.bin", "z.bin"},
                exact_required=True,
                excludes=rcm.DEFAULT_EXCLUDES,
            )

            self.assertEqual([line.rsplit("  ", 1)[1] for line in lines], ["a.bin", "z.bin"])
            self.assertEqual(out.read_text(encoding="utf-8"), "\n".join(lines) + "\n")
            self.assertEqual(rcm.verify_manifest(root, out), ["a.bin", "z.bin"])

    def test_generate_fails_when_required_asset_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            (root / "a.bin").write_text("a", encoding="utf-8")

            with self.assertRaises(rcm.ManifestError) as cm:
                rcm.generate_manifest(
                    root,
                    root / "SHA256SUMS",
                    {"a.bin", "missing.bin"},
                    exact_required=True,
                    excludes=rcm.DEFAULT_EXCLUDES,
                )

            self.assertIn("missing required release asset(s): missing.bin", str(cm.exception))

    def test_generate_fails_when_exact_required_sees_unexpected_asset(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            (root / "a.bin").write_text("a", encoding="utf-8")
            (root / "surprise.bin").write_text("surprise", encoding="utf-8")

            with self.assertRaises(rcm.ManifestError) as cm:
                rcm.generate_manifest(
                    root,
                    root / "SHA256SUMS",
                    {"a.bin"},
                    exact_required=True,
                    excludes=rcm.DEFAULT_EXCLUDES,
                )

            self.assertIn("unexpected release asset(s): surprise.bin", str(cm.exception))

    def test_verify_fails_on_tampered_asset(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            asset = root / "a.bin"
            asset.write_text("a", encoding="utf-8")
            manifest = root / "SHA256SUMS"
            rcm.generate_manifest(
                root,
                manifest,
                {"a.bin"},
                exact_required=True,
                excludes=rcm.DEFAULT_EXCLUDES,
            )

            asset.write_text("tampered", encoding="utf-8")

            with self.assertRaises(rcm.ManifestError) as cm:
                rcm.verify_manifest(root, manifest)

            self.assertIn("checksum mismatch for a.bin", str(cm.exception))

    def test_main_returns_nonzero_on_missing_required_asset(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            out = io.StringIO()
            err = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = rcm.main(
                    [
                        "generate",
                        str(root),
                        "--output",
                        str(root / "SHA256SUMS"),
                        "--required-name",
                        "missing.bin",
                        "--exact-required",
                    ]
                )

            self.assertEqual(rc, 1)
            self.assertEqual(out.getvalue(), "")
            self.assertIn("missing required release asset(s): missing.bin", err.getvalue())


if __name__ == "__main__":
    unittest.main()
