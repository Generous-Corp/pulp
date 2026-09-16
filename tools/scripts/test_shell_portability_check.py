#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path

import shell_portability_check as check


class ShellPortabilityTests(unittest.TestCase):
    def assert_findings(self, text: str, expected: bool) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.sh"
            path.write_text(text, encoding="utf-8")
            self.assertEqual(bool(check.check_file(path)), expected)

    def test_braced_colon_is_safe(self) -> None:
        self.assert_findings('#!/bin/zsh\nprint -- "${root}:test/file.cpp"\n', False)

    def test_unbraced_colon_is_rejected(self) -> None:
        self.assert_findings('#!/bin/zsh\nprint -- "$root:test/file.cpp"\n', True)

    def test_pipestatus_requires_bash_boundary(self) -> None:
        self.assert_findings('#!/bin/zsh\nprint -- "${PIPESTATUS[1]}"\n', True)

    def test_bash_pipestatus_is_allowed(self) -> None:
        self.assert_findings('#!/usr/bin/env bash\nprint -- "${PIPESTATUS[0]}"\n', False)

    def test_bare_pipestatus_is_rejected_in_zsh(self) -> None:
        self.assert_findings('#!/bin/zsh\nprint -- "$PIPESTATUS"\n', True)

    def test_manifest_is_valid(self) -> None:
        import json
        manifest = json.loads((Path(__file__).with_name("shell_portability_rules.json")).read_text())
        self.assertEqual(manifest["schema_version"], 1)
        self.assertTrue(all(rule["owner"] and rule["review_after"] for rule in manifest["rules"]))


if __name__ == "__main__":
    unittest.main()
