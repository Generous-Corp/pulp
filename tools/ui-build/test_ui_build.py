#!/usr/bin/env python3
"""Positive and planted-negative contract tests for ``pulp ui build/check``."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "ui_build.py"


class UiBuildContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="pulp-ui-build-")
        self.root = Path(self.tmp.name)
        self.source = self.root / "native-ui" / "src"
        self.out = self.root / "build" / "native-ui"
        self.source.mkdir(parents=True)
        (self.source / "Editor.tsx").write_text(
            "export function Editor({value}: {value: number}) {\n"
            "  return <button data-pulp-action=\"value\">{value}</button>;\n"
            "}\n", encoding="utf-8")
        (self.source / "tokens.css").write_text(":root { --accent: #fff; }\n", encoding="utf-8")
        self.addCleanup(self.tmp.cleanup)

    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return self.run_cli_in(self.root, *args)

    def run_cli_in(self, root: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args], cwd=root,
            text=True, capture_output=True, check=False)

    def test_build_then_check_is_deterministic(self):
        built = self.run_cli("build", "--source", "native-ui/src", "--out", "build/native-ui", "--json")
        self.assertEqual(built.returncode, 0, built.stderr)
        first = json.loads(built.stdout)
        checked = self.run_cli("check", "--source", "native-ui/src", "--out", "build/native-ui", "--json")
        self.assertEqual(checked.returncode, 0, checked.stderr)
        checked_receipt = json.loads(checked.stdout)
        self.assertEqual(first["source_digest"], checked_receipt["source_digest"])
        self.assertEqual(first["files"], checked_receipt["files"])
        manifest = (self.out / "ui-build-manifest.json").read_bytes()
        rebuilt = self.run_cli("build", "--source", "native-ui/src", "--out", "build/native-ui", "--json")
        self.assertEqual(rebuilt.returncode, 0, rebuilt.stderr)
        self.assertEqual(first, json.loads(rebuilt.stdout))
        self.assertEqual(manifest, (self.out / "ui-build-manifest.json").read_bytes())
        self.assertEqual((self.out / "Editor.tsx").read_bytes(), (self.source / "Editor.tsx").read_bytes())

    def test_manifest_is_independent_of_checkout_root(self):
        first = self.run_cli("build", "--source", "native-ui/src", "--out", "build/native-ui", "--json")
        self.assertEqual(first.returncode, 0, first.stderr)
        first_manifest = (self.out / "ui-build-manifest.json").read_bytes()

        other_root = self.root / "other-checkout"
        other_source = other_root / "native-ui" / "src"
        other_source.mkdir(parents=True)
        for source_file in self.source.iterdir():
            (other_source / source_file.name).write_bytes(source_file.read_bytes())
        second = self.run_cli_in(
            other_root, "build", "--source", "native-ui/src", "--out", "build/native-ui", "--json")
        self.assertEqual(second.returncode, 0, second.stderr)
        second_manifest = (other_root / "build" / "native-ui" / "ui-build-manifest.json").read_bytes()

        self.assertEqual(first_manifest, second_manifest)
        self.assertNotIn(str(self.root).encode(), first_manifest)
        manifest = json.loads(first_manifest)
        self.assertEqual(manifest["source_root"], ".")

    def test_check_rejects_source_drift(self):
        built = self.run_cli("build", "--source", "native-ui/src", "--out", "build/native-ui")
        self.assertEqual(built.returncode, 0, built.stderr)
        (self.source / "Editor.tsx").write_text(
            (self.source / "Editor.tsx").read_text(encoding="utf-8") + "// changed\n",
            encoding="utf-8")
        checked = self.run_cli("check", "--source", "native-ui/src", "--out", "build/native-ui")
        self.assertNotEqual(checked.returncode, 0)
        self.assertIn("source tree digest differs", checked.stderr)

    def test_check_rejects_tampered_output_and_extra_file(self):
        built = self.run_cli("build", "--source", "native-ui/src", "--out", "build/native-ui")
        self.assertEqual(built.returncode, 0, built.stderr)
        (self.out / "Editor.tsx").write_text("tampered\n", encoding="utf-8")
        (self.out / "stale.js").write_text("stale\n", encoding="utf-8")
        checked = self.run_cli("check", "--source", "native-ui/src", "--out", "build/native-ui")
        self.assertNotEqual(checked.returncode, 0)
        self.assertIn("built output bytes differ for Editor.tsx", checked.stderr)
        self.assertIn("untracked file stale.js", checked.stderr)

    def test_missing_or_empty_source_fails_closed(self):
        missing = self.run_cli("build", "--source", "missing", "--out", "build/native-ui")
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("source root does not exist", missing.stderr)
        empty = self.root / "empty"
        empty.mkdir()
        result = self.run_cli("build", "--source", "empty", "--out", "build/empty")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("contains no supported source files", result.stderr)

    def test_symlink_source_is_rejected(self):
        (self.source / "link.tsx").symlink_to(self.source / "Editor.tsx")
        result = self.run_cli("build", "--source", "native-ui/src", "--out", "build/native-ui")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("contains a symlink", result.stderr)


if __name__ == "__main__":
    unittest.main()
