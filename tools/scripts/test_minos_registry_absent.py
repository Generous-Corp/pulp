#!/usr/bin/env python3
"""The `pulp minos` tools degrade cleanly when the consumer registry is absent.

`sdk-consumers/consumers.yaml` lives in the private `planning` submodule. A
fresh public clone and every CI runner may legitimately lack it, so the two
`minos` CTest cases that read the registry are marked with a
SKIP_REGULAR_EXPRESSION rather than being allowed to fail.

That skip only works while the scripts keep printing the phrase the CMake
property matches. These tests hold both ends together: the scripts really do
announce a missing registry, and `tools/cli/CMakeLists.txt` really does key its
skip on that announcement.

Run:
    python3 tools/scripts/test_minos_registry_absent.py
"""
from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "tools" / "scripts"
CLI_CMAKE = REPO / "tools" / "cli" / "CMakeLists.txt"

# The single phrase both the scripts and the CTest properties agree on.
PHRASE = "consumers registry not found"


def run(script: str, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPTS / script), *args],
        capture_output=True, text=True, cwd=REPO)


class ScriptsAnnounceAMissingRegistry(unittest.TestCase):
    def test_sweep_dry_run(self):
        r = run("sdk_consumer_sweep.py", "--sdk-prefix", "/nonexistent",
                "--dry-run", "--consumers", "/nonexistent/consumers.yaml")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(PHRASE, r.stdout + r.stderr)

    def test_publish_runbook(self):
        r = run("sdk_consumer_update.py", "publish-runbook", "--to", "0.640.0",
                "--consumers", "/nonexistent/consumers.yaml")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(PHRASE, r.stdout + r.stderr)

    def test_the_message_names_the_remedy(self):
        r = run("sdk_consumer_sweep.py", "--sdk-prefix", "/nonexistent",
                "--dry-run", "--consumers", "/nonexistent/consumers.yaml")
        self.assertIn("git submodule update --init planning", r.stdout + r.stderr)


class CTestSkipsRatherThanFails(unittest.TestCase):
    """Both registry-reading CTest cases must carry the matching skip regex."""

    def setUp(self):
        self.cmake = CLI_CMAKE.read_text()

    def _properties_block(self, test_name: str) -> str:
        m = re.search(
            r"set_tests_properties\(\s*" + re.escape(test_name) + r"\s+PROPERTIES(.*?)\)",
            self.cmake, re.S)
        self.assertIsNotNone(m, f"{test_name} has no set_tests_properties block")
        return m.group(1)

    def test_sweep_dryrun_skips_without_the_registry(self):
        self.assertIn(PHRASE, self._properties_block("cli-minos-sweep-dryrun"))

    def test_publish_runbook_skips_without_the_registry(self):
        self.assertIn(PHRASE, self._properties_block("cli-minos-publish-runbook"))

    def test_the_skip_is_a_skip_regex_not_a_pass_regex(self):
        # A PASS_REGULAR_EXPRESSION on the failure phrase would turn a broken
        # tool into a green test. The property has to be SKIP_REGULAR_EXPRESSION.
        for name in ("cli-minos-sweep-dryrun", "cli-minos-publish-runbook"):
            block = self._properties_block(name)
            skip = re.search(r'SKIP_REGULAR_EXPRESSION\s+"([^"]*)"', block)
            self.assertIsNotNone(skip, f"{name} lacks SKIP_REGULAR_EXPRESSION")
            self.assertIn(PHRASE, skip.group(1))


if __name__ == "__main__":
    unittest.main()
