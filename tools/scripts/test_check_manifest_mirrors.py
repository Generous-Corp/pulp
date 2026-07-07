"""Tests for the manifest-mirror drift lint (check_manifest_mirrors.py)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.scripts import check_manifest_mirrors as cmm  # noqa: E402


def _manifest(skia_assets: dict, versions: dict) -> dict:
    return {
        "dependencies": [
            {"name": "Skia", "version": versions["Skia"],
             "determinism": {"release_assets": {
                 p: {"url": f"https://x/{p}.zip", "sha256": s}
                 for p, s in skia_assets.items()}}},
            {"name": "Dawn", "version": versions["Dawn"]},
            {"name": "V8", "version": versions["V8"]},
        ]
    }


_SHAS = {
    "linux-arm64": "a" * 64,
    "ios-simulator-arm64-x86_64": "b" * 64,  # underscore-bearing platform token
}
_VERSIONS = {"Skia": "chrome/m151", "Dawn": "chrome/m151 deps",
             "V8": "v8-15.2.24-lkgr-97440bd4f523"}


class ManifestMirrorTests(unittest.TestCase):
    def setUp(self) -> None:
        import tempfile
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    # --- repo-state guard: the live repo must currently be in sync ----------

    def test_live_repo_is_in_sync(self) -> None:
        self.assertEqual(cmm.main([]), 0)

    # --- VERSION.md parser: underscore platform tokens (x86_64) -------------

    def test_version_md_parses_underscore_platform(self) -> None:
        md = self.root / "VERSION.md"
        md.write_text(
            "| Asset | SHA-256 |\n|--|--|\n"
            "| `skia-build-ios-simulator-arm64-x86_64-gpu-release.zip` | `%s` |\n"
            "| `skia-build-mac-arm64-gpu-release.zip` | `%s` |\n"
            % ("b" * 64, "c" * 64),
            encoding="utf-8",
        )
        got = cmm.version_md_asset_shas(md)
        self.assertEqual(got["ios-simulator-arm64-x86_64"], "b" * 64)
        self.assertEqual(got["mac-arm64"], "c" * 64)

    def test_version_md_without_table_raises(self) -> None:
        md = self.root / "VERSION.md"
        md.write_text("# no digest table here\n", encoding="utf-8")
        with self.assertRaises(cmm.CheckError):
            cmm.version_md_asset_shas(md)

    # --- DEPENDENCIES.md parser --------------------------------------------

    def test_dependencies_md_versions(self) -> None:
        dep = self.root / "DEPENDENCIES.md"
        dep.write_text(
            "| Name | Version | License |\n|--|--|--|\n"
            "| Skia | chrome/m151 | BSD-3-Clause |\n"
            "| V8 | v8-15.2.24-lkgr-97440bd4f523 | BSD-3-Clause |\n",
            encoding="utf-8",
        )
        got = cmm.dependencies_md_versions(dep)
        self.assertEqual(got["Skia"], "chrome/m151")
        self.assertEqual(got["V8"], "v8-15.2.24-lkgr-97440bd4f523")

    # --- compare(): in-sync and each drift class ---------------------------

    def _patch(self, md_shas: dict, dep_versions: dict):
        return (
            mock.patch.object(cmm, "version_md_asset_shas", return_value=md_shas),
            mock.patch.object(cmm, "dependencies_md_versions", return_value=dep_versions),
        )

    def test_compare_in_sync(self) -> None:
        man = _manifest(_SHAS, _VERSIONS)
        p1, p2 = self._patch(dict(_SHAS), dict(_VERSIONS))
        with p1, p2:
            self.assertEqual(cmm.compare(man), [])

    def test_compare_reports_sha_drift(self) -> None:
        man = _manifest(_SHAS, _VERSIONS)
        drifted = dict(_SHAS)
        drifted["linux-arm64"] = "9" * 64
        p1, p2 = self._patch(drifted, dict(_VERSIONS))
        with p1, p2:
            drift = cmm.compare(man)
        self.assertTrue(any("linux-arm64" in d and "SHA-256" in d for d in drift))

    def test_compare_reports_missing_version_md_row(self) -> None:
        man = _manifest(_SHAS, _VERSIONS)
        partial = {"linux-arm64": _SHAS["linux-arm64"]}  # drop the x86_64 row
        p1, p2 = self._patch(partial, dict(_VERSIONS))
        with p1, p2:
            drift = cmm.compare(man)
        self.assertTrue(any("ios-simulator-arm64-x86_64" in d for d in drift))

    def test_compare_reports_version_drift(self) -> None:
        man = _manifest(_SHAS, _VERSIONS)
        bad = dict(_VERSIONS)
        bad["V8"] = "v8-15.1.27"  # stale
        p1, p2 = self._patch(dict(_SHAS), bad)
        with p1, p2:
            drift = cmm.compare(man)
        self.assertTrue(any(d.strip().startswith("V8 version drift") for d in drift))

    # --- error handling -----------------------------------------------------

    def test_main_missing_manifest_returns_2(self) -> None:
        with mock.patch.object(cmm, "_load_manifest",
                               side_effect=cmm.CheckError("nope")):
            self.assertEqual(cmm.main([]), 2)


if __name__ == "__main__":
    unittest.main()
