#!/usr/bin/env python3
"""Tests for release_please_config.py.

Load-bearing behavior: each surface's release-please `exclude-paths` is the
set of shallowest directories holding no trigger file, so a commit counts for
a surface at directory granularity exactly when versioning.json's trigger paths
say it should. The check fails only on drift that loses a release (an excluded
directory gaining a trigger file) and on a manifest that disagrees with its
version files, because release-please would then propose a version below
main's; a directory the derivation would add only over-releases, and passes.
"""
from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import release_please_config as rpc  # noqa: E402

REPO = HERE.parent.parent


class DeriveExcludePaths(unittest.TestCase):
    FILES = [
        "README.md",
        "core/a/src/x.cpp",
        "core/a/test/t.cpp",
        "core/b/docs/d.md",
        "docs/guide.md",
        "tools/cli/main.cpp",
        "tools/ci/run.py",
        "tools/scripts/sdk_provenance.py",
        "tools/scripts/lint.py",
    ]
    TRIGGERS = ["core/**/src/**/*.cpp", "tools/cli/**/*.cpp", "tools/scripts/sdk_provenance.py"]

    def test_shallowest_directories_without_a_trigger_file(self) -> None:
        self.assertEqual(
            rpc.derive_exclude_paths(self.TRIGGERS, self.FILES),
            ["core/a/test", "core/b", "docs", "tools/ci"],
        )

    def test_a_directory_holding_any_trigger_file_is_never_excluded(self) -> None:
        excluded = rpc.derive_exclude_paths(self.TRIGGERS, self.FILES)
        for kept in ("core", "core/a", "core/a/src", "tools", "tools/cli", "tools/scripts"):
            self.assertNotIn(kept, excluded)

    def test_no_trigger_excludes_every_top_level_directory(self) -> None:
        self.assertEqual(rpc.derive_exclude_paths([], self.FILES), ["core", "docs", "tools"])


class CheckFixture(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.write("core/src/a.cpp", "x\n")
        self.write("docs/d.md", "x\n")
        self.write("CMakeLists.txt", "project(Pulp\n    VERSION 1.2.3 # x-release-please-version\n)\n")
        self.write("versioning.json", json.dumps({
            "schema_version": 1,
            "surfaces": {"sdk": {
                "label": "SDK",
                "version_files": [{"path": "CMakeLists.txt", "kind": "cmake_project_version"}],
                "trigger_paths": ["core/**"],
                "release_please": {"config": "rp/sdk.config.json", "manifest": "rp/sdk.manifest.json"},
            }},
            "skills": {"skills_dir": ".agents/skills", "path_map": "map.json"},
        }))
        self.write("rp/sdk.config.json", json.dumps({"packages": {".": {"exclude-paths": []}}}))
        self.write("rp/sdk.manifest.json", json.dumps({".": "1.2.3"}))
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def check(self, write: bool = False) -> list[str]:
        return rpc.check(self.root, self.root / "versioning.json", write=write)

    def test_write_derives_the_exclude_paths(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(self.check(write=True), [])
        config = json.loads((self.root / "rp/sdk.config.json").read_text(encoding="utf-8"))
        self.assertEqual(config["packages"]["."]["exclude-paths"], ["docs", "rp"])

    def test_a_missing_exclude_only_over_releases_and_passes(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            self.assertEqual(self.check(), [])
        self.assertIn("could also exclude ['docs', 'rp']", stderr.getvalue())

    def test_an_excluded_directory_gaining_a_trigger_file_fails(self) -> None:
        self.write("rp/sdk.config.json", json.dumps({"packages": {".": {"exclude-paths": ["core/new"]}}}))
        self.write("core/new/b.cpp", "x\n")
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        with contextlib.redirect_stderr(io.StringIO()):
            problems = self.check()
        self.assertEqual(len(problems), 1)
        self.assertIn("excludes ['core/new'], which now hold trigger files", problems[0])

    def test_a_manifest_behind_the_version_file_fails(self) -> None:
        self.write("CMakeLists.txt", "project(Pulp\n    VERSION 1.3.0 # x-release-please-version\n)\n")
        with contextlib.redirect_stderr(io.StringIO()):
            problems = self.check()
        self.assertEqual(len(problems), 1)
        self.assertIn("records 1.2.3 but CMakeLists.txt carries 1.3.0", problems[0])

    def test_last_release_sha_must_be_the_manifest_tag_or_before_it(self) -> None:
        def commit(message: str) -> str:
            subprocess.run(["git", "-C", str(self.root), "-c", "user.email=t@e", "-c",
                            "user.name=t", "commit", "-q", "--allow-empty", "-m", message],
                           check=True)
            return subprocess.run(["git", "-C", str(self.root), "rev-parse", "HEAD"],
                                  check=True, capture_output=True, text=True,
                                  encoding="utf-8").stdout.strip()
        first = commit("first")
        tagged = commit("tagged")
        later = commit("later")
        subprocess.run(["git", "-C", str(self.root), "tag", "v1.2.3", tagged], check=True)
        for pin, ok in ((tagged, True), (first, True), (later, False)):
            self.write("rp/sdk.config.json", json.dumps(
                {"include-component-in-tag": False, "last-release-sha": pin,
                 "packages": {".": {"exclude-paths": ["docs", "rp"]}}}))
            with self.subTest(pin=pin), contextlib.redirect_stderr(io.StringIO()):
                problems = self.check()
                self.assertEqual(problems == [], ok, problems)

    def test_a_second_package_is_refused(self) -> None:
        self.write("rp/sdk.config.json", json.dumps({"packages": {".": {}, "core": {}}}))
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertIn("exactly one package", self.check()[0])


class RepositoryConfig(unittest.TestCase):
    def test_the_shipped_configs_match_versioning_json(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(rpc.main(["--check", "--repo", str(REPO)]), 0)

    def test_the_sdk_version_line_carries_the_generic_marker(self) -> None:
        text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        line = next(line for line in text.splitlines() if line.strip().startswith("VERSION "))
        self.assertIn("x-release-please-version", line)

    def test_the_plugin_config_never_writes_the_catalog_metadata_version(self) -> None:
        config = json.loads(
            (REPO / ".github/release-please/plugin.config.json").read_text(encoding="utf-8")
        )
        paths = {f["jsonpath"] for f in config["packages"]["."]["extra-files"] if f["type"] == "json"}
        self.assertTrue({"$.version", "$.plugins[0].version"} <= paths)
        self.assertFalse(any("metadata" in path for path in paths))

    def test_release_please_owns_no_changelog_and_tags_nothing(self) -> None:
        for surface in ("sdk", "plugin"):
            config = json.loads(
                (REPO / f".github/release-please/{surface}.config.json").read_text(encoding="utf-8")
            )
            self.assertIs(config["skip-changelog"], True, surface)
            self.assertIs(config["skip-github-release"], True, surface)


if __name__ == "__main__":
    unittest.main()
