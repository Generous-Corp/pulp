#!/usr/bin/env python3
"""Contract tests for the manifest-driven uninstaller.

An uninstaller has two ways to be wrong and only one of them is loud. Removing
too little leaves a plugin the host keeps loading, which the user notices.
Removing too much deletes somebody else's work, which they may not notice until
much later. Both are covered here.

HOME is redirected at every invocation so no test can touch the real plugin
folders, and the system-level root is exercised only through paths that do not
exist, so nothing here needs (or gets) administrator rights.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "scripts" / "pulp_uninstall.sh"


class UninstallerTest(unittest.TestCase):
    def _run(self, manifest: Path, home: Path, *extra: str):
        env = dict(os.environ)
        env["HOME"] = str(home)
        return subprocess.run(
            ["/bin/bash", str(SCRIPT), "--manifest", str(manifest), *extra],
            env=env, text=True, capture_output=True, check=False)

    def _fixture(self, tmp: Path, manifest_paths: list[str]) -> Path:
        manifest = tmp / "uninstall-manifest.txt"
        body = "product: Fixture\nversion: 1.0.0\n"
        body += "".join(f"path: {p}\n" for p in manifest_paths)
        manifest.write_text(body)
        return manifest

    def test_removes_a_same_named_copy_the_manifest_never_listed(self) -> None:
        # The installer writes to /Library; a development build writes to
        # ~/Library. Leaving the second behind means the host still sees a
        # plugin with that bundle id, so "uninstalled" does not mean gone.
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw); home = tmp / "home"
            user_copy = home / "Library/Audio/Plug-Ins/Components/Acme FX.component"
            user_copy.mkdir(parents=True)
            manifest = self._fixture(
                tmp, ["/Library/Audio/Plug-Ins/Components/Acme FX.component"])

            out = self._run(manifest, home, "--yes")
            self.assertEqual(out.returncode, 0, msg=out.stderr)
            self.assertFalse(user_copy.exists(),
                             "the user-level copy of the same bundle survived")

    def test_leaves_a_differently_named_plugin_alone(self) -> None:
        # The nightmare: two vendors whose products share a word. Matching is
        # by exact bundle name, never by prefix or glob.
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw); home = tmp / "home"
            components = home / "Library/Audio/Plug-Ins/Components"
            (components / "Acme FX.component").mkdir(parents=True)
            other = components / "Acme FX Pro.component"
            other.mkdir(parents=True)
            manifest = self._fixture(
                tmp, ["/Library/Audio/Plug-Ins/Components/Acme FX.component"])

            self._run(manifest, home, "--yes")
            self.assertTrue(other.exists(),
                            "a different vendor's plugin was deleted")

    def test_refuses_a_path_outside_an_installable_root(self) -> None:
        # A corrupted or hand-edited manifest must not be able to reach
        # anywhere an installer could not have written.
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw); home = tmp / "home"
            home.mkdir(parents=True)
            victim = tmp / "not-ours"
            victim.mkdir()
            manifest = self._fixture(tmp, [str(victim)])

            out = self._run(manifest, home, "--yes")
            self.assertTrue(victim.exists(), "removed a path outside the allowlist")
            self.assertIn("refusing path", out.stderr)

    def test_dry_run_removes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw); home = tmp / "home"
            user_copy = home / "Library/Audio/Plug-Ins/CLAP/Acme FX.clap"
            user_copy.mkdir(parents=True)
            manifest = self._fixture(
                tmp, ["/Library/Audio/Plug-Ins/CLAP/Acme FX.clap"])

            out = self._run(manifest, home, "--dry-run")
            self.assertEqual(out.returncode, 0, msg=out.stderr)
            self.assertTrue(user_copy.exists(), "dry run deleted something")
            self.assertIn("dry run", out.stdout)

    def test_running_twice_is_not_an_error(self) -> None:
        # Someone will run it again to check it worked. That must read as
        # "nothing to remove" rather than as a failure.
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw); home = tmp / "home"
            (home / "Library/Audio/Plug-Ins/VST3/Acme FX.vst3").mkdir(parents=True)
            manifest = self._fixture(
                tmp, ["/Library/Audio/Plug-Ins/VST3/Acme FX.vst3"])

            self.assertEqual(self._run(manifest, home, "--yes").returncode, 0)
            second = self._run(manifest, home, "--yes")
            self.assertEqual(second.returncode, 0, msg=second.stderr)
            self.assertIn("already gone", second.stdout)

    def test_a_missing_manifest_fails_loudly(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            out = self._run(tmp / "nope.txt", tmp, "--yes")
            self.assertNotEqual(out.returncode, 0)
            self.assertIn("no uninstall manifest", out.stderr)


if __name__ == "__main__":
    unittest.main()
