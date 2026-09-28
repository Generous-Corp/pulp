#!/usr/bin/env python3
"""Tests for check_vendored_runtime.py.

The motivating bundle was banner-less and lacked the scoped re-apply fix while
its app built against an SDK that had it. The stale cases below reproduce that
shape; the current cases prove the checker is not simply reporting everything
stale, and the scan cases prove a directory with no bundle says so instead of
reading as up to date.
"""
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_vendored_runtime as cvr  # noqa: E402

MANIFEST = {
    "schema": 1,
    "revision": 2,
    "fixes": [
        {"revision": 1, "id": "scoped-reapply", "summary": "scoped re-apply",
         "signature": "materializedDirtyIds"},
        {"revision": 2, "id": "unsigned-fix", "summary": "no signature"},
    ],
}
OLD = "(() => { function markMaterializedTreeDirty() {} })();\n"
HAS_FIRST = "(() => { const materializedDirtyIds = new Set(); "\
            "globalThis.__pulpReactDomRegistry__ = 1; })();\n"
CURRENT = "/* @pulp/react runtime revision 2 */\n(() => {})();\n"


def run(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        rc = cvr.main(argv)
    return rc, out.getvalue()


class VendoredRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self.fp = self.root / "fingerprint.json"
        self.fp.write_text(json.dumps(MANIFEST))

    def tearDown(self):
        self.tmp.cleanup()

    def bundle(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def test_banner_less_old_bundle_misses_every_fix(self):
        ids = [f["id"] for f in cvr.missing_fixes(OLD, MANIFEST)]
        self.assertEqual(ids, ["scoped-reapply", "unsigned-fix"])

    def test_signature_proves_a_fix_without_a_banner(self):
        ids = [f["id"] for f in cvr.missing_fixes(HAS_FIRST, MANIFEST)]
        self.assertEqual(ids, ["unsigned-fix"])

    def test_banner_at_sdk_revision_is_current(self):
        self.assertEqual(cvr.missing_fixes(CURRENT, MANIFEST), [])

    def test_older_banner_misses_later_fixes(self):
        text = "/* @pulp/react runtime revision 1 */\n(() => {})();\n"
        ids = [f["id"] for f in cvr.missing_fixes(text, MANIFEST)]
        self.assertEqual(ids, ["unsigned-fix"])

    def test_stale_warns_by_default_and_fails_under_strict(self):
        path = self.bundle("runtime.js", OLD)
        rc, out = run([str(path), "--fingerprint", str(self.fp)])
        self.assertEqual(rc, 0)
        self.assertIn("STALE", out)
        self.assertIn("[scoped-reapply]", out)
        self.assertIn("refresh:", out)
        rc, _ = run([str(path), "--fingerprint", str(self.fp), "--strict"])
        self.assertEqual(rc, 1)

    def test_current_passes_strict(self):
        path = self.bundle("runtime.js", CURRENT)
        rc, out = run([str(path), "--fingerprint", str(self.fp), "--strict"])
        self.assertEqual(rc, 0, out)
        self.assertIn("OK", out)

    def test_directory_scan_finds_bundles_and_skips_node_modules(self):
        self.bundle("app/ui/runtime.js", OLD)
        self.bundle("app/node_modules/x/runtime.js", OLD)
        self.bundle("app/ui/other.js", "console.log('not a bundle');\n")
        found = cvr.find_bundles(self.root / "app")
        self.assertEqual([p.relative_to(self.root).as_posix() for p in found],
                         ["app/ui/runtime.js"])

    def test_directory_without_bundle_says_none(self):
        self.bundle("empty/ui/main.js", "createCol('root');\n")
        rc, out = run([str(self.root / "empty"), "--fingerprint", str(self.fp),
                       "--strict"])
        self.assertEqual(rc, 0)
        self.assertIn("NONE", out)

    def test_named_non_bundle_is_a_usage_error(self):
        path = self.bundle("main.js", "createCol('root');\n")
        rc, _ = run([str(path), "--fingerprint", str(self.fp)])
        self.assertEqual(rc, 2)

    def test_real_sdk_fingerprint_loads(self):
        manifest = cvr.load_fingerprint(cvr.DEFAULT_FINGERPRINT)
        self.assertGreaterEqual(manifest["revision"], 1)
        self.assertTrue(all(f["revision"] <= manifest["revision"]
                            for f in manifest["fixes"]))


if __name__ == "__main__":
    unittest.main()
