#!/usr/bin/env python3
"""Fixture suite for screenshot_sync_check.py.

Spins up a throwaway git repo (shared `GateFixtureTestCase`), drops a
`.pulp/screenshots.toml` manifest into it, stages scenarios via real
commits, and asserts the gate's exit code + stdout.

Run directly:
    python3 tools/scripts/test_screenshot_sync_check.py
"""

from __future__ import annotations

import sys
import unittest

from gate_test_support import GateFixtureTestCase, REPO_ROOT, _run, _git

SSCS = REPO_ROOT / "tools" / "scripts" / "screenshot_sync_check.py"

MANIFEST = """\
schema_version = 1

[trigger]
paths = [
  "ui/**/*.js",
  "src/**/*_editor.cpp",
  "assets/design-system/**/*.css",
]

[[target]]
id = "mono-synth-editor"
route = "native"
consumes = [
  { kind = "readme", path = "docs/img/mono-synth.png" },
]

[[target]]
id = "mono-synth-web"
route = "web"
consumes = [
  { kind = "og",      path = "web/mono-synth/og.png" },
  { kind = "og_meta", html = "web/mono-synth/index.html", rel = "./og.png" },
]
"""


class ScreenshotSyncTests(GateFixtureTestCase):
    def _run_gate(self, extra=None):
        return _run(
            ["python3", str(SSCS), "--base", "origin/main", "--mode=report",
             "--repo-root", str(self.f.root), *(extra or [])],
            cwd=self.f.root,
        )

    def _opt_in(self):
        self.f.write(".pulp/screenshots.toml", MANIFEST)
        # Seed the consumed PNGs + a UI file so later diffs are meaningful.
        self.f.write("ui/mono_synth.js", "// v1\n")
        self.f.write("docs/img/mono-synth.png", "PNGv1\n")
        self.f.write("web/mono-synth/og.png", "PNGv1\n")
        self.f.commit("opt in to screenshot-sync")
        # Advance origin/main so the diff base is this commit — otherwise the
        # seed PNG writes would appear inside the origin/main...HEAD range and
        # look like a fresh re-shoot.
        _git(self.f.root, "update-ref", "refs/remotes/origin/main", "HEAD")

    # ── scenarios ─────────────────────────────────────────────────────

    def test_no_manifest_is_noop(self):
        code, out = self._run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("not opted in", out)

    def test_no_trigger_touched(self):
        self._opt_in()
        self.f.write("README.md", "docs only\n")
        self.f.commit("docs change")
        code, out = self._run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("nothing to verify", out)

    def test_ux_change_without_reshoot_fails(self):
        self._opt_in()
        self.f.write("ui/mono_synth.js", "// v2 — new layout\n")
        self.f.commit("change mono-synth UI")
        code, out = self._run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("mono-synth-editor", out)
        self.assertIn("NOT refreshed", out)

    def test_ux_change_with_reshoot_passes(self):
        self._opt_in()
        self.f.write("ui/mono_synth.js", "// v2\n")
        self.f.write("docs/img/mono-synth.png", "PNGv2\n")
        self.f.write("web/mono-synth/og.png", "PNGv2\n")
        self.f.commit("change UI + reshoot")
        code, out = self._run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("refreshed", out)

    def test_bypass_trailer_specific_target(self):
        self._opt_in()
        self.f.write("ui/mono_synth.js", "// v2\n")
        self.f.write("web/mono-synth/og.png", "PNGv2\n")  # web reshot; editor not
        self.f.commit(
            'change UI, skip native\n\n'
            'Screenshot-Sync: skip target=mono-synth-editor reason="invisible refactor"'
        )
        code, out = self._run_gate()
        self.assertEqual(code, 0, out)
        self.assertIn("bypassed", out)

    def test_bypass_trailer_all(self):
        self._opt_in()
        self.f.write("assets/design-system/tokens.css", ":root{--x:2}\n")
        self.f.commit(
            'reskin\n\nScreenshot-Sync: skip target=all reason="batch reshoot follows"'
        )
        code, out = self._run_gate()
        self.assertEqual(code, 0, out)

    def test_token_change_invalidates_all_targets(self):
        self._opt_in()
        self.f.write("assets/design-system/tokens.css", ":root{--x:2}\n")
        self.f.commit("reskin all widgets")
        code, out = self._run_gate()
        self.assertEqual(code, 1, out)
        self.assertIn("mono-synth-editor", out)
        self.assertIn("mono-synth-web", out)

    def test_invalid_manifest_exits_2(self):
        self.f.write(".pulp/screenshots.toml", '[[target]]\nroute = "native"\n')  # no id
        self.f.commit("bad manifest")
        code, out = self._run_gate()
        self.assertEqual(code, 2, out)
        self.assertIn("invalid manifest", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
