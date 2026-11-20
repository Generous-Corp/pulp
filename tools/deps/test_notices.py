#!/usr/bin/env python3
"""Tests for tools/deps/notices.py, the per-SDK notices manifest generator."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "deps"))
import notices  # noqa: E402

NOTICE = """# Third-Party Notices

---

## Alpha

Copyright (c) 2020 Alpha Authors
All rights reserved.

MIT License

Permission is hereby granted.

---

## Beta

COPYRIGHT AND PERMISSION NOTICE

Copyright © 2016 Beta, Inc.

Beta license text.
"""


def dep(name: str, **extra: object) -> dict:
    entry = {"name": name, "license": "MIT", "version": "1.0",
             "repository": f"https://example.invalid/{name.lower()}.git"}
    entry.update(extra)
    return entry


class BuildManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.sections = notices.notice_sections(NOTICE)

    def test_only_linked_entries_are_emitted(self) -> None:
        manifest, texts, errors = notices.build_manifest(
            [dep("Alpha", link_targets=["alpha"]), dep("Beta")], self.sections, "9.9")
        self.assertEqual(errors, [])
        self.assertEqual([e["name"] for e in manifest["entries"]], ["Alpha"])
        self.assertEqual(manifest["schema"], "pulp.notices.v1")
        self.assertEqual(manifest["sdk_version"], "9.9")
        entry = manifest["entries"][0]
        self.assertEqual(entry["required_by"], ["alpha"])
        self.assertEqual(entry["source"], "https://example.invalid/alpha")
        self.assertEqual(entry["copyright"],
                         "Copyright (c) 2020 Alpha Authors\nAll rights reserved.")
        self.assertIn("Permission is hereby granted.", texts[entry["license_text"]])
        self.assertNotIn("---", texts[entry["license_text"]])

    def test_copyright_skips_an_all_caps_heading(self) -> None:
        manifest, _, _ = notices.build_manifest(
            [dep("Beta", link_targets=["beta"])], self.sections, "")
        self.assertEqual(manifest["entries"][0]["copyright"], "Copyright © 2016 Beta, Inc.")

    def test_linked_entry_without_notice_section_is_an_error(self) -> None:
        _, _, errors = notices.build_manifest(
            [dep("Gamma", link_targets=["gamma"])], self.sections, "")
        self.assertEqual(len(errors), 1)
        self.assertIn("Gamma", errors[0])

    def test_license_ids(self) -> None:
        self.assertEqual(notices.spdx_id(dep("A", license="MIT OR Apache-2.0")),
                         "MIT OR Apache-2.0")
        self.assertEqual(notices.spdx_id(dep("A", license="SIL OFL 1.1", spdx="OFL-1.1")),
                         "OFL-1.1")
        self.assertEqual(notices.spdx_id(dep("A", license="Public domain")),
                         "LicenseRef-public-domain")

    def test_feature_is_carried(self) -> None:
        manifest, _, _ = notices.build_manifest(
            [dep("Alpha", link_targets=["alpha"], link_feature="HAS_ALPHA")],
            self.sections, "")
        self.assertEqual(manifest["entries"][0]["feature"], "HAS_ALPHA")


class RepositoryTests(unittest.TestCase):
    def test_pulp_manifest_generates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(notices.main(["--out", tmp]), 0)
            manifest = json.loads((Path(tmp) / "notices.json").read_text())
            names = {e["name"] for e in manifest["entries"]}
            # Controls: dependencies every plug-in or every GPU editor links.
            for name in ("CLAP", "Mbed TLS", "Skia", "Dawn", "Yoga"):
                self.assertIn(name, names)
            for entry in manifest["entries"]:
                text = Path(tmp) / entry["license_text"]
                self.assertTrue(text.read_text().strip(), entry["name"])

    def test_unchanged_output_is_not_rewritten(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            notices.main(["--out", tmp])
            path = Path(tmp) / "notices.json"
            before = path.stat().st_mtime_ns
            notices.main(["--out", tmp])
            self.assertEqual(path.stat().st_mtime_ns, before)


if __name__ == "__main__":
    unittest.main()
