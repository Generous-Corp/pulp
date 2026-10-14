#!/usr/bin/env python3
"""Tests for tools/toolchain/check_env.py.

Every "reports nothing" case has a paired control that must report something
on the same fixture, so a checker that never looks cannot pass.
"""
from __future__ import annotations

import ast
import copy
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import check_env

HERE = Path(__file__).resolve().parent
PLAT = "macos-arm64"
VERSION_TEXT = {
    "python3": "Python 3.14.4",
    "cmake": "cmake version 4.3.3\n\nCMake suite maintained and supported by Kitware (kitware.com/cmake).",
    "ninja": "1.13.2",
    "node": "v26.10.0",
    "rustc": "rustc 1.97.1 (6b00bc388 2026-06-12)",
    "clang-format": "clang-format version 21.1.8",
    "mise": "2026.10.8 macos-arm64 (2026-10-13)",
}


def expected_doc(mise_sha: str = "0" * 64) -> dict:
    return {
        "schema": 1,
        "enforcement": "warn",
        "tools": {
            "python": "3.14.4", "cmake": "4.3.3", "ninja": "1.13.2",
            "node": "26.10.0", "rust": "1.97.1", "clang_format": "21.1.8",
        },
        "platform_overrides": {"linux": {"cmake": "system", "rust": "absent"}},
        "mise": {"version": "2026.10.8", "path": "~/.local/opt/mise-2026.10.8/bin/mise", "sha256": {PLAT: mise_sha}},
        "npm_prefixes": [],
    }


class Host:
    """A fake PATH: executable name -> (path, --version text)."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.text = dict(VERSION_TEXT)
        self.present = set(VERSION_TEXT)
        self.mise_bytes = b"pinned mise"
        self.mise_path = root / "bin/mise"
        self.mise_path.parent.mkdir(parents=True, exist_ok=True)
        self.mise_path.write_bytes(self.mise_bytes)

    @property
    def mise_sha(self) -> str:
        return hashlib.sha256(self.mise_bytes).hexdigest()

    def which(self, name: str):
        if name not in self.present:
            return None
        return str(self.mise_path) if name == "mise" else f"/fake/bin/{name}"

    def run(self, argv, cwd):
        name = Path(argv[0]).name
        return self.text.get(name)


class CheckEnvTests(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.tmp = Path(self.td.name)
        self.host = Host(self.tmp)
        self.home = self.tmp / "home"
        self.home.mkdir()

    def tearDown(self) -> None:
        self.td.cleanup()

    def collect(self, doc: dict, plat: str = PLAT):
        return check_env.collect(doc, self.tmp, self.home, plat, self.host.which, self.host.run)

    def matching(self) -> dict:
        return expected_doc(self.host.mise_sha)

    def test_matching_host_reports_nothing_and_a_mismatch_is_seen(self) -> None:
        self.assertEqual([], self.collect(self.matching()))
        # Control on the same fixture: one changed version must be reported.
        self.host.text["cmake"] = "cmake version 4.4.3"
        findings = self.collect(self.matching())
        self.assertEqual(["cmake"], [f.tool for f in findings])

    def test_each_tool_mismatch_names_tool_versions_path_and_fix(self) -> None:
        cases = {
            "python3": ("Python 3.11.9", "3.11.9", "3.14.4"),
            "cmake": ("cmake version 3.28.3", "3.28.3", "4.3.3"),
            "ninja": ("1.12.1", "1.12.1", "1.13.2"),
            "node": ("v24.21.0", "24.21.0", "26.10.0"),
            "rustc": ("rustc 1.99.0 (abc 2026-09-01)", "1.99.0", "1.97.1"),
            "clang-format": ("Homebrew clang-format version 22.1.0", "22.1.0", "21.1.8"),
        }
        for name, (text, found, want) in cases.items():
            with self.subTest(tool=name):
                self.host.text = dict(VERSION_TEXT)
                self.host.text[name] = text
                findings = self.collect(self.matching())
                self.assertEqual(1, len(findings), [f.line() for f in findings])
                line = findings[0].line()
                self.assertIn(f"{name}: expected {want}", line)
                self.assertIn(f"found {found} at /fake/bin/{name}", line)
                self.assertIn("fix: ", line)

    def test_rust_is_asked_where_cargo_builds(self) -> None:
        (self.tmp / check_env.RUST_DIR).mkdir(parents=True)
        seen = {}

        def run(argv, cwd):
            seen[Path(argv[0]).name] = cwd
            return self.host.run(argv, cwd)

        check_env.collect(self.matching(), self.tmp, self.home, PLAT, self.host.which, run)
        self.assertEqual(self.tmp / check_env.RUST_DIR, seen["rustc"])
        self.assertIsNone(seen["cmake"])

    def test_missing_rustup_toolchain_is_named_and_never_installed(self) -> None:
        self.host.text["rustc"] = "error: toolchain '1.97.1-aarch64-apple-darwin' is not installed"
        findings = self.collect(self.matching())
        self.assertIn("found a rust-toolchain.toml toolchain that is not installed", findings[0].line())

    @unittest.skipIf(os.name == "nt", "a shell-script stand-in for rustc")
    def test_version_probe_turns_off_rustup_auto_install(self) -> None:
        probe = self.tmp / "rustc"
        probe.write_text('#!/bin/sh\necho "RUSTUP_AUTO_INSTALL=$RUSTUP_AUTO_INSTALL"\n', encoding="utf-8")
        probe.chmod(0o755)
        self.assertIn("RUSTUP_AUTO_INSTALL=0", check_env.run_version([str(probe), "--version"], None))

    def test_unparseable_version_is_reported_as_unknown(self) -> None:
        self.host.text["ninja"] = "ninja: something odd"
        findings = self.collect(self.matching())
        self.assertIn("found unknown", findings[0].line())

    def test_missing_tool_is_reported(self) -> None:
        self.host.present.discard("clang-format")
        findings = self.collect(self.matching())
        self.assertEqual(["clang-format"], [f.tool for f in findings])
        self.assertIn("not installed", findings[0].line())

    def test_python_falls_back_to_python_on_windows_hosts(self) -> None:
        self.host.present.discard("python3")
        self.host.present.add("python")
        self.host.text["python"] = "Python 3.14.4"
        self.assertEqual([], self.collect(self.matching()))

    def test_declared_platform_exceptions(self) -> None:
        self.host.text["cmake"] = "cmake version 3.28.3"
        self.host.present.discard("rustc")
        findings = self.collect(self.matching(), plat="linux-x64")
        tools = [f.tool for f in findings]
        # rust = "absent" on linux: a missing rustc is not a finding there ...
        self.assertNotIn("rustc", tools)
        # ... but the same missing rustc is one on macOS (the control).
        self.assertIn("rustc", [f.tool for f in self.collect(self.matching())])
        # cmake = "system" is still reported, and labelled as the declared exception.
        cmake = [f for f in findings if f.tool == "cmake"]
        self.assertEqual(1, len(cmake))
        self.assertIn("declared exception: platform.linux cmake = system", cmake[0].line())

    def test_mise_version_mismatch(self) -> None:
        self.host.text["mise"] = "2026.5.16 macos-arm64 (2026-05-16)"
        findings = self.collect(self.matching())
        self.assertEqual(["mise"], [f.tool for f in findings])
        self.assertIn("found 2026.5.16", findings[0].line())

    def test_mise_same_version_different_binary(self) -> None:
        findings = self.collect(expected_doc("f" * 64))
        self.assertEqual(["mise"], [f.tool for f in findings])
        self.assertIn("same version, different binary", findings[0].line())
        self.assertIn(self.host.mise_sha, findings[0].line())

    def test_mise_without_a_pinned_digest_is_not_a_match(self) -> None:
        findings = self.collect(self.matching(), plat="macos-x64")
        self.assertEqual(["mise"], [f.tool for f in findings])
        self.assertIn("unpinned platform", findings[0].line())

    def test_mise_off_path_but_installed_names_the_path_fix(self) -> None:
        self.host.present.discard("mise")
        doc = self.matching()
        pinned = self.home / ".local/opt/mise-2026.10.8/bin/mise"
        findings = self.collect(doc)
        self.assertIn("not installed", findings[0].line())
        pinned.parent.mkdir(parents=True)
        pinned.write_bytes(b"x")
        findings = self.collect(doc)
        self.assertIn(f"put {pinned.parent} first on PATH", findings[0].line())

    def write_npm(self, prefix: str, locked: dict, installed) -> dict:
        directory = self.tmp / prefix
        directory.mkdir(parents=True)
        (directory / "package-lock.json").write_text(json.dumps({"packages": locked}), encoding="utf-8")
        if installed is not None:
            (directory / "node_modules").mkdir()
            (directory / "node_modules/.package-lock.json").write_text(json.dumps({"packages": installed}), encoding="utf-8")
        doc = self.matching()
        doc["npm_prefixes"] = [prefix]
        return doc

    LOCKED = {
        "": {"name": "root"},
        "node_modules/esbuild": {"version": "0.25.0"},
        "node_modules/@esbuild/linux-x64": {"version": "0.25.0", "optional": True},
    }

    def test_npm_installed_from_the_lockfile_matches(self) -> None:
        installed = {"node_modules/esbuild": {"version": "0.25.0"}}
        self.assertEqual([], self.collect(self.write_npm("a", self.LOCKED, installed)))

    def test_npm_drift_is_reported(self) -> None:
        cases = {
            "not installed": None,
            "esbuild 0.24.0 != 0.25.0": {"node_modules/esbuild": {"version": "0.24.0"}},
            "node_modules/esbuild missing": {},
            "node_modules/left-pad not in the lockfile": {
                "node_modules/esbuild": {"version": "0.25.0"}, "node_modules/left-pad": {"version": "1.0.0"},
            },
        }
        for index, (needle, installed) in enumerate(cases.items()):
            with self.subTest(case=needle):
                findings = self.collect(self.write_npm(f"p{index}", self.LOCKED, installed))
                self.assertEqual(1, len(findings))
                self.assertIn(needle, findings[0].line())
                self.assertIn("mise run npm-deps", findings[0].line())

    def finding(self):
        return [check_env.Finding("cmake", "4.3.3", "4.4.3", "/x/cmake", "fix it")]

    def test_verdicts(self) -> None:
        f = self.finding()
        self.assertEqual((0, []), check_env.verdict([], "enforce", {}, self.home))
        self.assertEqual(0, check_env.verdict(f, "warn", {}, self.home)[0])
        self.assertEqual(1, check_env.verdict(f, "enforce", {}, self.home)[0])
        status, banners = check_env.verdict(f, "enforce", {"PULP_TOOLCHAIN_UNPINNED": "1"}, self.home)
        self.assertEqual(0, status)
        self.assertIn("UNPINNED TOOLCHAIN", banners[0])
        self.assertIn("cmake 4.4.3", banners[0])

    def test_unpinned_is_refused_on_a_fleet_host(self) -> None:
        f = self.finding()
        env = {"PULP_TOOLCHAIN_UNPINNED": "1", "PULP_FLEET_HOST": "1"}
        status, banners = check_env.verdict(f, "enforce", env, self.home)
        self.assertEqual(1, status)
        self.assertIn("refused on a fleet host", banners[1])
        marker = self.home / ".config/pulp/fleet-host"
        marker.parent.mkdir(parents=True)
        marker.write_text("", encoding="utf-8")
        self.assertEqual(1, check_env.verdict(f, "enforce", {"PULP_TOOLCHAIN_UNPINNED": "1"}, self.home)[0])

    def test_unreadable_expectation_exits_3(self) -> None:
        bad = self.tmp / "expected.json"
        bad.write_text("{}", encoding="utf-8")
        self.assertEqual(3, check_env.main(["--expected", str(bad)]))
        self.assertEqual(3, check_env.main(["--expected", str(self.tmp / "absent.json")]))

    def test_platform_names_match_the_lock(self) -> None:
        expected = json.loads((HERE / "expected.json").read_text(encoding="utf-8"))
        for plat in expected["mise"]["sha256"]:
            self.assertRegex(plat, r"^(linux|macos|windows)-(arm64|x64)$")
        self.assertRegex(check_env.host_platform(), r"^[a-z0-9]+-[a-z0-9_]+$")

    def test_source_parses_as_python_39(self) -> None:
        # It runs on a stock Mac's /usr/bin/python3 (3.9) before mise exists.
        source = (HERE / "check_env.py").read_text(encoding="utf-8")
        ast.parse(source, feature_version=(3, 9))
        self.assertNotIn("tomllib", source)


@unittest.skipIf(os.name == "nt", "the fake tools are shell scripts")
class EndToEndTests(unittest.TestCase):
    """Run the script as CI does, against a PATH of fake tools."""

    def run_script(self, root: Path, path: Path, *args: str) -> subprocess.CompletedProcess:
        env = {"PATH": str(path), "HOME": str(root / "home")}
        return subprocess.run(
            [sys.executable, str(HERE / "check_env.py"), "--root", str(root), *args],
            env=env, capture_output=True, text=True, encoding="utf-8", check=False,
        )

    def test_github_annotations_and_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            for name, text in VERSION_TEXT.items():
                script = bin_dir / name
                script.write_text(f"#!/bin/sh\nprintf '%s\\n' '{text}'\n", encoding="utf-8")
                script.chmod(0o755)
            mise_sha = hashlib.sha256((bin_dir / "mise").read_bytes()).hexdigest()
            doc = expected_doc()
            doc["mise"]["sha256"] = {check_env.host_platform(): mise_sha}
            (root / "tools/toolchain").mkdir(parents=True)
            target = root / "tools/toolchain/expected.json"
            target.write_text(json.dumps(doc), encoding="utf-8")
            ok = self.run_script(root, bin_dir)
            self.assertEqual(0, ok.returncode, ok.stdout + ok.stderr)
            self.assertIn("matches tools/toolchain/manifest.toml (warn mode)", ok.stdout)

            (bin_dir / "cmake").write_text("#!/bin/sh\necho 'cmake version 3.28.3'\n", encoding="utf-8")
            warn = self.run_script(root, bin_dir, "--format", "github")
            self.assertEqual(0, warn.returncode)
            self.assertIn("::warning title=toolchain cmake::cmake: expected 4.3.3", warn.stdout)
            self.assertIn("found 3.28.3", warn.stdout)

            enforced = copy.deepcopy(doc)
            enforced["enforcement"] = "enforce"
            target.write_text(json.dumps(enforced), encoding="utf-8")
            refused = self.run_script(root, bin_dir)
            self.assertEqual(1, refused.returncode)
            self.assertIn("refusing (enforce mode)", refused.stdout)

            report = json.loads(self.run_script(root, bin_dir, "--format", "json").stdout)
            self.assertEqual(["cmake"], [f["tool"] for f in report["findings"]])

    @unittest.skipUnless(Path("/usr/bin/python3").exists(), "no system python3")
    def test_runs_on_the_system_python(self) -> None:
        out = subprocess.run(
            ["/usr/bin/python3", str(HERE / "check_env.py"), "--format", "json"],
            capture_output=True, text=True, encoding="utf-8", check=False,
        )
        # 0 or 1 by the manifest's mode; 3 or a traceback means it cannot run there.
        self.assertIn(out.returncode, (0, 1), out.stderr)
        self.assertIn("findings", json.loads(out.stdout))


if __name__ == "__main__":
    unittest.main()
