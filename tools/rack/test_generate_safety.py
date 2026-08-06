#!/usr/bin/env python3
"""Destructive-path checks for Forge's generated-module writer."""

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import generate  # noqa: E402


class GeneratedSlugSafety(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.pack = pathlib.Path(self.temp.name)
        (self.pack / "modules").mkdir()
        (self.pack / "src").mkdir()
        self.pack_patch = mock.patch.object(generate, "PACK", str(self.pack))
        self.pack_patch.start()

    def tearDown(self) -> None:
        self.pack_patch.stop()
        self.temp.cleanup()

    def write_manifest(self, filename: str, slug: str) -> pathlib.Path:
        path = self.pack / "modules" / filename
        path.write_text(json.dumps({"modules": [{"slug": slug}]}))
        return path

    def test_existing_manifest_path_is_never_truncated(self) -> None:
        path = self.write_manifest("vco.json", "VCO")
        before = path.read_bytes()

        with self.assertRaisesRegex(generate.ExistingModuleSlug,
                                    "manifest already exists"):
            generate._write_generated_module({"slug": "VCO"}, "replacement")

        self.assertEqual(path.read_bytes(), before)
        self.assertFalse((self.pack / "src" / "VCO.cpp").exists())

    def test_slug_identity_is_refused_across_manifest_filenames_and_case(self) -> None:
        path = self.write_manifest("legacy-name.json", "ExistingVoice")
        before = path.read_bytes()

        with self.assertRaisesRegex(generate.ExistingModuleSlug,
                                    "already declared"):
            generate._write_generated_module(
                {"slug": "existingvoice"}, "replacement")

        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list((self.pack / "src").iterdir()), [])

    def test_existing_source_or_symlink_is_never_followed(self) -> None:
        source = self.pack / "src" / "Orphan.cpp"
        source.write_text("keep me")
        with self.assertRaisesRegex(generate.ExistingModuleSlug,
                                    "source file already exists"):
            generate._write_generated_module({"slug": "Orphan"}, "replace")
        self.assertEqual(source.read_text(), "keep me")

        dangling = self.pack / "modules" / "linked.json"
        os.symlink(self.pack / "missing-target", dangling)
        with self.assertRaisesRegex(generate.ExistingModuleSlug,
                                    "manifest already exists"):
            generate._write_generated_module({"slug": "Linked"}, "body")

    def test_unique_slug_writes_both_new_files(self) -> None:
        generate._write_generated_module(
            {"slug": "FreshVoice", "name": "Fresh Voice"}, "// dsp\n")

        manifest = json.loads(
            (self.pack / "modules" / "freshvoice.json").read_text())
        self.assertTrue(manifest["forge_generated"])
        self.assertEqual(manifest["modules"][0]["slug"], "FreshVoice")
        self.assertEqual((self.pack / "src" / "FreshVoice.cpp").read_text(),
                         "// dsp\n")


if __name__ == "__main__":
    unittest.main()
