#!/usr/bin/env python3
"""Tests for tools/deps/check_patches.py, using a small fixture repository."""

from __future__ import annotations

import difflib
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_patches  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

V0 = "".join(f"line {i:02d}\n" for i in range(1, 41))
V1 = V0.replace("line 05\n", "line 05 patched once\n")
# The second patch rewrites the line the first one changed, so the series only
# reverses when it is undone last to first.
V2 = V1.replace("line 05 patched once\n", "line 05 patched twice\n")


def unified(old: str, new: str, rel: str) -> str:
    return "".join(difflib.unified_diff(
        old.splitlines(True), new.splitlines(True), f"a/{rel}", f"b/{rel}"))


def sha(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


class Fixture:
    """A throwaway repository with one vendored file and a two-patch series."""

    REL = "external/lib/file.c"

    def __init__(self, tree_text: str = V2):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "external/lib").mkdir(parents=True)
        (self.root / self.REL).write_text(tree_text, encoding="utf-8")
        pdir = self.root / "tools/deps/patches/lib"
        pdir.mkdir(parents=True)
        (pdir / "0001-first.patch").write_text(unified(V0, V1, self.REL), encoding="utf-8")
        (pdir / "0002-second.patch").write_text(unified(V1, V2, self.REL), encoding="utf-8")
        self.manifest = {
            "schema_version": 1,
            "dependencies": [{
                "name": "lib", "in_tree": True,
                "upstream_repo": "https://example.invalid/lib.git",
                "upstream_ref": "v1",
                "files": [{"path": self.REL, "upstream_path": "file.c",
                           "upstream_sha256": sha(V0)}],
                "patches": ["tools/deps/patches/lib/0001-first.patch",
                            "tools/deps/patches/lib/0002-second.patch"],
            }],
        }
        self.write_manifest()

    def write_manifest(self):
        self.path = self.root / "tools/deps/patches.json"
        self.path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def run(self):
        manifest = check_patches.load_manifest(self.path)
        problems = check_patches.check_orphans(manifest, self.root)
        for dep in manifest["dependencies"]:
            problems += check_patches.check_dependency(dep, self.root)
        return problems


class CheckPatchesTest(unittest.TestCase):
    def test_clean_series_reverses_to_upstream(self):
        self.assertEqual(Fixture().run(), [])

    def test_untracked_in_tree_edit_fails(self):
        # An edit on a line no patch touches reverses cleanly but no longer
        # matches upstream.
        fx = Fixture(V2.replace("line 30\n", "line 30 edited\n"))
        problems = fx.run()
        self.assertEqual(len(problems), 1)
        self.assertIn("does not match upstream", problems[0])

    def test_edit_inside_a_patched_hunk_names_the_failing_patch(self):
        fx = Fixture(V2.replace("line 05 patched twice\n", "line 05 hand edited\n"))
        problems = fx.run()
        self.assertEqual(len(problems), 1)
        # Patches reverse last to first, so the last patch is the first to fail.
        self.assertIn("0002-second.patch", problems[0])
        self.assertNotIn("0001-first.patch", problems[0])

    def test_reports_only_the_first_failing_patch(self):
        # Break both hunks: only the last patch (the first one tried) is named.
        fx = Fixture(V2.replace("line 05 patched twice\n", "line 05 other\n"))
        problems = fx.run()
        self.assertEqual(len(problems), 1)
        self.assertIn("0002-second.patch", problems[0])

    def test_wrong_order_is_rejected(self):
        fx = Fixture()
        fx.manifest["dependencies"][0]["patches"].reverse()
        fx.write_manifest()
        self.assertTrue(fx.run())

    def test_missing_patch_file_fails(self):
        fx = Fixture()
        (fx.root / "tools/deps/patches/lib/0002-second.patch").unlink()
        problems = fx.run()
        self.assertTrue(any("does not exist" in p for p in problems))

    def test_unlisted_patch_file_fails(self):
        fx = Fixture()
        (fx.root / "tools/deps/patches/lib/0003-extra.patch").write_text("", encoding="utf-8")
        self.assertTrue(any("not listed" in p for p in fx.run()))

    def test_patch_touching_unlisted_file_fails(self):
        fx = Fixture()
        other = "external/lib/other.c"
        (fx.root / "tools/deps/patches/lib/0001-first.patch").write_text(unified(V0, V1, other), encoding="utf-8")
        self.assertTrue(any("not listed" in p for p in fx.run()))

    def test_unpatched_dependency_must_equal_upstream(self):
        fx = Fixture(V0)
        fx.manifest["dependencies"][0]["patches"] = []
        fx.write_manifest()
        for p in (fx.root / "tools/deps/patches/lib").glob("*.patch"):
            p.unlink()
        self.assertEqual(fx.run(), [])
        (fx.root / fx.REL).write_text(V1, encoding="utf-8")
        self.assertTrue(fx.run())

    def test_crlf_checkout_still_verifies(self):
        fx = Fixture()
        path = fx.root / fx.REL
        path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(fx.run(), [])

    def test_scratch_inside_another_repository(self):
        # The verifier must work when the temporary directory sits inside some
        # other repository, as it does when TMPDIR points into a checkout.
        outer = tempfile.TemporaryDirectory()
        subprocess.run(["git", "init", "-q"], cwd=outer.name, check=True)
        previous = tempfile.tempdir
        tempfile.tempdir = outer.name
        try:
            self.assertEqual(Fixture().run(), [])
        finally:
            tempfile.tempdir = previous
            outer.cleanup()

    def test_missing_vendored_file_fails(self):
        fx = Fixture()
        (fx.root / fx.REL).unlink()
        self.assertTrue(any("missing" in p for p in fx.run()))

    def test_fetched_fork_requires_provenance(self):
        dep = {"name": "forked", "in_tree": False, "files": [], "patches": []}
        self.assertTrue(check_patches.check_dependency(dep, REPO_ROOT))

    def test_configure_time_patch_functions_must_exist(self):
        fx = Fixture()
        (fx.root / "patch.cmake").write_text("function(pulp_patch_x dir)\nendfunction()\n", encoding="utf-8")
        dep = {"name": "forked", "in_tree": False, "upstream_repo": "u", "upstream_ref": "r",
               "fork_repo": "f", "fork_ref": "t", "fork_commit": "c",
               "carried_commits": ["abc one"], "files": [], "patches": [],
               "configure_time_patches": {"file": "patch.cmake", "functions": ["pulp_patch_x"]}}
        self.assertEqual(check_patches.check_dependency(dep, fx.root), [])
        dep["configure_time_patches"]["functions"].append("pulp_patch_missing")
        self.assertTrue(any("pulp_patch_missing" in p for p in check_patches.check_dependency(dep, fx.root)))

    def test_duplicate_names_rejected(self):
        fx = Fixture()
        fx.manifest["dependencies"].append(dict(fx.manifest["dependencies"][0]))
        fx.write_manifest()
        with self.assertRaises(check_patches.ManifestError):
            check_patches.load_manifest(fx.path)

    def test_repository_manifest_verifies(self):
        self.assertEqual(
            check_patches.run(check_patches.DEFAULT_MANIFEST, REPO_ROOT, False), 0)


if __name__ == "__main__":
    unittest.main()
