#!/usr/bin/env python3
"""Model B (post_merge_assignment) tests: PRs touch NO version files; the
version-at-land bot infers the level and assigns it on merge. Gate must PASS a
fix/feat touching a versioned surface (no bump/no trailer), HARD-FAIL a fix/feat
touching NO versioned surface (Z2), and make --mode=apply a verified no-op."""
from __future__ import annotations
import json, subprocess, unittest
from gate_test_support import GateFixtureTestCase, VBC, _run, _git


class ModelBGateTests(GateFixtureTestCase):
    def setUp(self):
        super().setUp()
        # Turn ON Model B in the fixture's versioning.json (it ships without it).
        cfgp = self.tmp / "tools/scripts/versioning.json"
        cfg = json.loads(cfgp.read_text())
        cfg["post_merge_assignment"] = True
        cfgp.write_text(json.dumps(cfg, indent=2) + "\n")
        _git(self.f.root, "add", "--", "tools/scripts/versioning.json")
        _git(self.f.root, "commit", "-q", "-m", "test: enable Model B")
        self._cfgp = str(cfgp)

    def _report(self, extra=None):
        return _run(["python3", str(VBC), "--base", "origin/main",
                     "--config", self._cfgp, "--mode=report", *(extra or [])],
                    cwd=self.tmp)

    def _commit(self, path, content, subject):
        self.f.write(path, content)
        _git(self.f.root, "add", "--", path)
        _git(self.f.root, "commit", "-q", "-m", subject)

    def test_touched_surface_no_bump_no_trailer_passes(self):
        # public-API header → sdk scores minor; NO version files, NO trailer.
        self._commit("core/runtime/include/pulp/foo.hpp",
                     "#pragma once\nint foo();\n", "feat(core): new API")
        rc, out = self._report()
        self.assertEqual(rc, 0, out)
        self.assertIn("assigned post-merge", out)

    def test_fixfeat_touching_no_surface_fails(self):
        self._commit("notes/scratch.txt", "notes\n", "fix(x): non-surface change")
        rc, out = self._report()
        self.assertEqual(rc, 1, out)
        self.assertIn("touches NO versioned surface", out)

    def test_nonfixfeat_touching_no_surface_passes(self):
        self._commit("notes/scratch.txt", "notes\n", "docs: a note")
        rc, out = self._report()
        self.assertEqual(rc, 0, out)

    def test_skip_trailer_rescues_no_surface_fixfeat(self):
        self._commit("notes/scratch.txt", "notes\n",
                     'fix(x): non-surface\n\nVersion-Bump: skip reason="no surface"')
        rc, out = self._report()
        self.assertEqual(rc, 0, out)

    def test_apply_is_a_noop(self):
        self._commit("core/runtime/include/pulp/foo.hpp",
                     "#pragma once\nint foo();\n", "feat(core): new API")
        rc, out = _run(["python3", str(VBC), "--base", "origin/main",
                        "--config", self._cfgp, "--mode=apply"], cwd=self.tmp)
        self.assertEqual(rc, 0, out)
        self.assertIn("version files not written", out)
        dirty = subprocess.run(["git","-C",str(self.tmp),"status","--porcelain"],
                               capture_output=True, text=True).stdout.strip()
        self.assertEqual(dirty, "", f"apply wrote files: {dirty}")


if __name__ == "__main__":
    unittest.main()
