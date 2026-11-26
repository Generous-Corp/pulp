#!/usr/bin/env python3
"""Tests for tools/scripts/examples_manifest_guard.py."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import examples_manifest_guard as guard  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

GOOD = {
    "plain": {"class": "audio-only", "platform": "cross", "evidence": "no view link"},
    "ui": {"class": "moves-to-vellum", "platform": "cross", "evidence": "links pulp::view"},
    "phone": {"class": "quarantined", "platform": "ios", "evidence": "iOS only",
              "quarantine": "C4b-ios"},
    "gone": {"class": "deleted", "platform": "macos", "evidence": "dead", "reason": "never configures"},
}


def make_repo(entries: dict, files: dict[str, str], dirs: list[str] | None = None):
    """Build a throwaway git repo with a manifest and tracked example files."""
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    names = dirs if dirs is not None else list(GOOD)
    for name in names:
        (root / "examples" / name).mkdir(parents=True)
        (root / "examples" / name / "CMakeLists.txt").write_text("", encoding="utf-8")
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    manifest = root / "docs" / "status" / "examples-manifest.json"
    manifest.parent.mkdir(parents=True)
    manifest.write_text(json.dumps({"examples": entries}), encoding="utf-8")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    return tmp


def run(entries: dict, files: dict[str, str] | None = None, dirs: list[str] | None = None) -> list[str]:
    with make_repo(entries, files or {}, dirs) as tmp:
        return guard.check(Path(tmp))


class ExamplesManifestGuardTest(unittest.TestCase):
    def test_complete_manifest_passes(self):
        self.assertEqual(run(GOOD), [])

    def test_unclassified_directory_fails(self):
        problems = run(GOOD, dirs=list(GOOD) + ["extra"])
        self.assertEqual(len(problems), 1)
        self.assertIn("examples/extra is not classified", problems[0])

    def test_entry_without_directory_fails(self):
        problems = run(GOOD, dirs=["plain", "ui", "phone"])
        self.assertEqual(len(problems), 1)
        self.assertIn("'gone'", problems[0])

    def test_unknown_class_fails(self):
        entries = {**GOOD, "plain": {**GOOD["plain"], "class": "retained"}}
        problems = run(entries)
        self.assertEqual(len(problems), 1)
        self.assertIn("plain: class must be one of", problems[0])

    def test_deleted_needs_reason(self):
        entries = {**GOOD, "gone": {"class": "deleted", "platform": "macos", "evidence": "dead"}}
        self.assertTrue(any("deleted entries need a reason" in p for p in run(entries)))

    def test_quarantined_needs_phase(self):
        entries = {**GOOD, "phone": {"class": "quarantined", "platform": "ios", "evidence": "x"}}
        self.assertTrue(any("quarantined entries need" in p for p in run(entries)))

    def test_empty_evidence_fails(self):
        entries = {**GOOD, "ui": {"class": "moves-to-vellum", "platform": "cross", "evidence": " "}}
        self.assertTrue(any("evidence is empty" in p for p in run(entries)))

    def test_audio_only_with_view_include_fails(self):
        files = {"examples/plain/main.cpp": '#include <pulp/view/view.hpp>\nint main() {}\n'}
        problems = run(GOOD, files)
        self.assertEqual(len(problems), 1)
        self.assertIn("plain: classified audio-only but references the UI layer", problems[0])

    def test_audio_only_with_view_link_fails(self):
        files = {"examples/plain/CMakeLists.txt": "target_link_libraries(x PRIVATE pulp::view)\n"}
        self.assertTrue(any("pulp::view" in p for p in run(GOOD, files)))

    def test_view_include_is_fine_when_it_moves(self):
        files = {"examples/ui/main.cpp": '#include <pulp/view/view.hpp>\n'}
        self.assertEqual(run(GOOD, files), [])

    def test_audio_only_may_include_non_ui_headers(self):
        files = {"examples/plain/main.cpp": '#include <pulp/signal/biquad.hpp>\n#include <pulp/inspect/x.hpp>\n'}
        self.assertEqual(run(GOOD, files), [])

    def test_real_repository_manifest_is_clean(self):
        self.assertEqual(guard.check(REPO_ROOT), [])


if __name__ == "__main__":
    unittest.main()
