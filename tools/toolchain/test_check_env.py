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
        self.host.text["rustc"] = "error: toolchain '1.97.1-x86_64-unknown-linux-gnu' is not installed"
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


REPO_ROOT = HERE.parents[1]


def workflow_step_script() -> str:
    """The `run:` block of build.yml's toolchain report step, as bash text."""
    lines = (REPO_ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if "name: Toolchain report against the manifest" in line)
    run = next(i for i in range(start, len(lines)) if lines[i].strip() == "run: |")
    indent = len(lines[run + 1]) - len(lines[run + 1].lstrip())
    body = []
    for line in lines[run + 1:]:
        if line.strip() and len(line) - len(line.lstrip()) < indent:
            break
        body.append(line[indent:])
    return "\n".join(body) + "\n"


@unittest.skipIf(os.name == "nt" or shutil.which("bash") is None, "runs the step under bash -e")
class WorkflowStepTests(unittest.TestCase):
    """The CI step is report only: it must exit 0 under `bash -e` whatever is missing."""

    def run_step(self, cwd: Path, path: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [shutil.which("bash"), "-e", "-c", workflow_step_script()], cwd=str(cwd), env={"PATH": path, "HOME": str(cwd)},
            capture_output=True, text=True, encoding="utf-8", check=False,
        )

    def test_no_python_on_path_is_a_warning_not_a_failure(self) -> None:
        out = self.run_step(REPO_ROOT, "/nonexistent")
        self.assertEqual(0, out.returncode, out.stderr)
        self.assertIn("no python3 or python on PATH", out.stdout)

    def test_missing_tools_and_an_unreadable_expectation_still_exit_0(self) -> None:
        python_dir = str(Path(sys.executable).parent)
        out = self.run_step(REPO_ROOT, python_dir)
        self.assertEqual(0, out.returncode, out.stderr)
        self.assertIn("difference(s) from the manifest", out.stdout)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "tools/toolchain").mkdir(parents=True)
            (root / "tools/toolchain/check_env.py").write_bytes((HERE / "check_env.py").read_bytes())
            out = self.run_step(root, python_dir)
            self.assertEqual(0, out.returncode, out.stderr)
            self.assertIn("check_env.py exited 3", out.stdout)

    def test_a_hung_probe_is_bounded(self) -> None:
        self.assertLessEqual(check_env.PROBE_TIMEOUT_S, 5)
        self.assertIn("timeout-minutes: 2", (REPO_ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8"))


class FleetReportTests(unittest.TestCase):
    """tools/toolchain/fleet_report.py: one table across hosts."""

    def setUp(self) -> None:
        import fleet_report

        self.fr = fleet_report

    def fake_run(self, results):
        calls = []

        def run(target, script, path):
            calls.append((target, path))
            return results[target]

        return run, calls

    def report(self, findings):
        return json.dumps({"findings": findings})

    def test_table_marks_pins_names_drift_and_unreachable_hosts(self) -> None:
        results = {
            "m1": (0, self.report([
                {"tool": "python3", "found": "3.11.16", "note": ""},
                {"tool": "clang-format", "found": "nothing", "note": "not installed"},
            ]), ""),
            "localhost": (0, self.report([]), ""),
            "m9": (255, "", "ssh: Could not resolve hostname m9"),
        }
        run, calls = self.fake_run(results)
        import io
        from contextlib import redirect_stdout

        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(0, self.fr.main(["--host", "m1", "--host", "m3=localhost", "--host", "m9"], run=run))
        text = out.getvalue()
        self.assertIn("| tool | m1 | m3 | m9 | pin |", text)
        self.assertIn("| python3 | 3.11.16 | = 3.14.4 | unreachable (ssh: Could not resolve hostname m9) | 3.14.4 |", text)
        self.assertIn("| clang-format | none | = 21.1.8 |", text)
        self.assertEqual(["m1", "localhost", "m9"], [c[0] for c in calls])
        self.assertTrue(all("/opt/homebrew/bin" in c[1] for c in calls))

    def test_payload_runs_check_env_and_leaves_nothing_behind(self) -> None:
        env_path = f"{Path(sys.executable).parent}{os.pathsep}/usr/bin{os.pathsep}/bin"
        before = set(Path(tempfile.gettempdir()).glob("pulp-fleet-report-*"))
        code, stdout, stderr = self.fr.run_on("local", self.fr.payload(), env_path)
        self.assertEqual(0, code, stderr)
        self.assertIn("findings", json.loads(stdout))
        self.assertEqual(before, set(Path(tempfile.gettempdir()).glob("pulp-fleet-report-*")))


REPO = HERE.parents[1]


@unittest.skipIf(shutil.which("cmake") is None or os.name == "nt", "needs cmake and a POSIX stub")
class ConfigureWiringTests(unittest.TestCase):
    """tools/cmake/PulpToolchainCheck.cmake runs the check once per configure."""

    def configure(self, exit_code: int, *, subproject: bool = False, env_extra=None) -> subprocess.CompletedProcess:
        td = tempfile.mkdtemp(prefix="pulp-toolchain-cmake-")
        self.addCleanup(shutil.rmtree, td, True)
        root = Path(td)
        fake = root / "pulp"
        (fake / "tools/toolchain").mkdir(parents=True)
        (fake / "tools/toolchain/check_env.py").write_text(
            "import pathlib, sys\n"
            "pathlib.Path(sys.argv[0]).with_name('ran').write_text(' '.join(sys.argv[1:]), encoding='utf-8')\n"
            "print('toolchain-check: cmake: expected 4.3.3, found 3.28.3; fix: x')\n"
            f"sys.exit({exit_code})\n", encoding="utf-8")
        module = REPO / "tools/cmake/PulpToolchainCheck.cmake"
        (fake / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\nproject(FakePulp NONE)\n"
            f'set(PULP_ROOT_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}")\ninclude("{module}")\n', encoding="utf-8")
        source = fake
        if subproject:
            outer = root / "outer"
            outer.mkdir()
            (outer / "CMakeLists.txt").write_text(
                f'cmake_minimum_required(VERSION 3.21)\nproject(Outer NONE)\nadd_subdirectory("{fake}" pulp)\n',
                encoding="utf-8")
            source = outer
        env = {k: v for k, v in os.environ.items() if k != "GITHUB_ACTIONS"}
        env.update(env_extra or {})
        self.ran = fake / "tools/toolchain/ran"
        return subprocess.run(
            ["cmake", "-S", str(source), "-B", str(root / "build"), f"-DPython3_EXECUTABLE={sys.executable}"],
            env=env, capture_output=True, text=True, encoding="utf-8", check=False)

    def test_warn_mode_prints_and_configures(self) -> None:
        out = self.configure(0)
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        self.assertIn("-- toolchain-check: cmake: expected 4.3.3, found 3.28.3; fix: x", out.stdout)
        self.assertIn("--quiet", self.ran.read_text(encoding="utf-8"))

    def test_enforce_mode_stops_the_configure(self) -> None:
        out = self.configure(1)
        self.assertNotEqual(0, out.returncode)
        self.assertIn("expected 4.3.3, found 3.28.3", out.stderr)

    def test_an_unrunnable_check_does_not_stop_the_configure(self) -> None:
        out = self.configure(3)
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        self.assertIn("toolchain-check: could not run (exit 3)", out.stdout)

    def test_skipped_as_a_subproject_and_under_github_actions(self) -> None:
        self.assertEqual(0, self.configure(1, subproject=True).returncode)
        self.assertFalse(self.ran.exists())
        self.assertEqual(0, self.configure(1, env_extra={"GITHUB_ACTIONS": "true"}).returncode)
        self.assertFalse(self.ran.exists())

    def test_the_checkout_configure_includes_it(self) -> None:
        text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('include("${PULP_ROOT_DIR}/tools/cmake/PulpToolchainCheck.cmake")', text)


class SetupWiringTests(unittest.TestCase):
    def test_setup_runs_the_check_before_fetching_dependencies(self) -> None:
        text = (REPO / "setup.sh").read_text(encoding="utf-8")
        check = text.index('"$TOOLCHAIN_PY" "$TOOLCHAIN_CHECK" --root "$REPO_ROOT"')
        self.assertLess(check, text.index('step "Setting up external SDKs"'))
        self.assertLess(text.index('if $DEPS_ONLY; then\n    DEPENDENCY_ERRORS'), len(text))
        # It sits outside the full-setup-only audit block, so `pulp build`'s
        # `setup.sh --deps-only` runs it too.
        audit_end = text.index("fi # full environment audit")
        self.assertGreater(check, audit_end)


if __name__ == "__main__":
    unittest.main()
