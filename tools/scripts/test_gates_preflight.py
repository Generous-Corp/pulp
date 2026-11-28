#!/usr/bin/env python3
"""Self-test gates_preflight.py: each missing prerequisite is named with its exact fix.

Every case builds a real git repository (a depth-1 clone for the shallow case),
removes exactly one prerequisite from an otherwise complete checkout, and asserts
the report names that one prerequisite and the command that fixes it. The
complete checkout is the control: it must report nothing, or a broken detector
would pass every other case by also reporting nothing wrong.
"""

from __future__ import annotations

import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gates_preflight as pre  # noqa: E402

SYSTEM = platform.system().lower()
MACHINE = platform.machine().lower()
JSX = Path("tools/import-design/jsx-runtime")
INSTALLER = """#!/bin/sh
git config core.hooksPath .githooks
git config merge.pulp-gpu-ledger.driver "tools/scripts/gpu_ledger_merge_driver.py %O %A %B"
"""
SETUP = """#!/bin/sh
mkdir -p external/vst3sdk/pluginterfaces external/AudioUnitSDK/include/AudioUnitSDK
touch external/AudioUnitSDK/include/AudioUnitSDK/AUBase.h
"""
FAKE_NPM = """#!/bin/sh
prefix=""
while [ $# -gt 0 ]; do
  case "$1" in --prefix) prefix="$2"; shift ;; esac
  shift
done
mkdir -p "$prefix/node_modules/esbuild" "$prefix/node_modules/@babel/parser"
touch "$prefix/node_modules/esbuild/package.json" "$prefix/node_modules/@babel/parser/package.json"
"""
FAKE_CMAKE = """#!/bin/sh
build=""
while [ $# -gt 0 ]; do
  case "$1" in -B) build="$2"; shift ;; esac
  shift
done
mkdir -p "$build/.cmake/api/v1/reply"
touch "$build/.cmake/api/v1/reply/codemodel-v2-fake.json"
"""


def sh(cwd: Path, *cmd: str, env: dict[str, str] | None = None) -> str:
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True, encoding="utf-8",
                          env={**os.environ, **(env or {})}).stdout


def write(path: Path, text: str, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if executable:
        path.chmod(path.stat().st_mode | stat.S_IXUSR)


def profile(vst3: str, ausdk: str) -> dict:
    return {"features": {"PULP_HAS_VST3": vst3, "PULP_HAS_AUSDK": ausdk}}


def make_origin(base: Path) -> Path:
    origin = base / "origin"
    origin.mkdir()
    sh(origin, "git", "init", "-q", "-b", "main")
    sh(origin, "git", "config", "user.email", "t@example.invalid")
    sh(origin, "git", "config", "user.name", "t")
    write(origin / ".gitattributes", "ledger.yaml merge=pulp-gpu-ledger\n")
    write(origin / "ledger.yaml", "a: 1\n")
    write(origin / ".githooks/pre-push", "#!/bin/sh\n", executable=True)
    write(origin / "setup.sh", SETUP, executable=True)
    write(origin / "tools/scripts/install-githooks.sh", INSTALLER, executable=True)
    write(origin / pre.CENSUS_FILE, json.dumps({"profiles": {f"{SYSTEM}-{MACHINE}-all": profile("ON", "ON")}}))
    write(origin / JSX / "package-lock.json", "{}\n")
    sh(origin, "git", "add", "-A")
    sh(origin, "git", "commit", "-q", "-m", "one")
    write(origin / "ledger.yaml", "a: 2\n")
    sh(origin, "git", "commit", "-q", "-am", "two")
    return origin


class PreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.base = Path(self._tmp.name).resolve()
        self.origin = make_origin(self.base)
        self.bin = self.base / "bin"
        write(self.bin / "npm", FAKE_NPM, executable=True)
        write(self.bin / "cmake", FAKE_CMAKE, executable=True)
        self.env = {"PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}", "CMAKE": str(self.bin / "cmake"),
                    "npm_config_cache": str(self.base / "npm-cache"), "PULP_CONFIGURE_ONLY_GOVERNOR": ""}

    def clone(self, name: str = "work", depth: int | None = None) -> Path:
        root = self.base / name
        cmd = ["git", "clone", "-q"] + (["--depth", str(depth)] if depth else [])
        sh(self.base, *cmd, f"file://{self.origin}", str(root))
        return root

    def complete(self, name: str = "work") -> Path:
        """A full clone with every prerequisite present."""
        root = self.clone(name)
        self.assertEqual(pre.install_worktree_hooks(root), "")
        sh(root, "sh", "setup.sh")
        sh(root, "sh", "-c", f"PATH={self.bin}:$PATH npm ci --prefix {JSX}")
        return root

    def collect(self, root: Path, **extra: str) -> pre.Report:
        return pre.collect(root, {**self.env, **extra})

    def keys(self, report: pre.Report) -> list[str]:
        return [f.key for f in report.findings]

    def only(self, report: pre.Report, key: str) -> pre.Finding:
        self.assertEqual(self.keys(report), [key], report)
        return report.findings[0]

    def test_control_a_complete_checkout_reports_nothing(self) -> None:
        report = self.collect(self.complete())
        self.assertEqual(self.keys(report), [])

    def test_control_a_bare_clone_reports_every_prerequisite(self) -> None:
        # The detectors must be able to fail: the same checkout, unprepared.
        report = self.collect(self.clone(depth=1))
        self.assertEqual(self.keys(report), ["shallow", "githooks", "sdks", "jsx-runtime"])

    def test_shallow_clone_names_unshallow(self) -> None:
        root = self.complete()
        sh(root, "git", "fetch", "-q", "--depth", "1")
        finding = self.only(self.collect(root), "shallow")
        self.assertEqual(finding.fix, ["git fetch --unshallow"])

    def test_missing_merge_driver_names_the_installer(self) -> None:
        root = self.complete()
        sh(root, "git", "config", "--worktree", "--unset", "merge.pulp-gpu-ledger.driver")
        finding = self.only(self.collect(root), "githooks")
        self.assertIn("git config --worktree core.hooksPath .githooks", finding.fix[0])
        self.assertIn("git config --worktree merge.pulp-gpu-ledger.driver", finding.fix[0])
        self.assertIn("this worktree", finding.what)
        self.assertIn("pulp-gpu-ledger", finding.what)

    def test_missing_hooks_path_is_the_same_finding(self) -> None:
        root = self.complete()
        sh(root, "git", "config", "--worktree", "--unset", "core.hooksPath")
        self.only(self.collect(root), "githooks")

    def test_missing_sdks_name_setup_deps_only(self) -> None:
        root = self.complete()
        shutil.rmtree(root / "external/vst3sdk")
        finding = self.only(self.collect(root), "sdks")
        self.assertEqual(finding.fix, ["./setup.sh --deps-only --non-interactive"])
        self.assertIn("VST3 SDK", finding.what)
        self.assertNotIn("AudioUnit", finding.what)

    def test_sdks_are_not_required_when_no_profile_records_them(self) -> None:
        root = self.complete()
        write(root / pre.CENSUS_FILE, json.dumps({"profiles": {f"{SYSTEM}-{MACHINE}-none": profile("OFF", "OFF")}}))
        shutil.rmtree(root / "external")
        self.assertEqual(self.keys(self.collect(root)), [])

    def test_sdks_of_another_platform_are_not_required(self) -> None:
        root = self.complete()
        write(root / pre.CENSUS_FILE, json.dumps({"profiles": {"plan9-mips-all": profile("ON", "ON")}}))
        shutil.rmtree(root / "external")
        self.assertEqual(self.keys(self.collect(root)), [])

    def test_missing_jsx_packages_name_npm_ci(self) -> None:
        root = self.complete()
        shutil.rmtree(root / JSX / "node_modules")
        finding = self.only(self.collect(root), "jsx-runtime")
        self.assertEqual(finding.fix, [f"(cd {JSX.as_posix()} && npm ci --cache {self.base / 'npm-cache'})"])

    def test_no_lockfile_means_no_jsx_requirement(self) -> None:
        root = self.complete()
        shutil.rmtree(root / JSX / "node_modules")
        (root / JSX / "package-lock.json").unlink()
        self.assertEqual(self.keys(self.collect(root)), [])

    def make_build(self, root: Path, name: str, reply: bool, vellum: str | None = None) -> Path:
        build = root / name
        build.mkdir()
        cache = "CMAKE_GENERATOR:INTERNAL=Ninja\n" + (f"{vellum}:BOOL=ON\n" if vellum else "")
        write(build / "CMakeCache.txt", cache)
        write(build / "CTestTestfile.cmake", "")
        if reply:
            write(build / pre.CODEMODEL_QUERY, "")
            write(build / pre.CODEMODEL_REPLY / "codemodel-v2-x.json", "{}")
        return build

    def test_build_dir_without_the_file_api_reply_names_the_query_and_configure(self) -> None:
        root = self.complete()
        self.make_build(root, "build", reply=False)
        finding = self.only(self.collect(root), "file-api")
        self.assertEqual(finding.fix, [
            "mkdir -p build/.cmake/api/v1/query && touch build/.cmake/api/v1/query/codemodel-v2",
            "cmake -S . -B build"])

    def reserved_host(self, role: str) -> Path:
        """A checkout carrying the real host policy, with tartci faked to report ``role``."""
        root = self.complete(f"host-{role}")
        (root / "tools/scripts").mkdir(parents=True, exist_ok=True)
        shutil.copy2(HERE / "gate_host_policy.sh", root / "tools/scripts/gate_host_policy.sh")
        write(self.bin / "tartci", f'#!/bin/sh\necho "TARTCI_SERVES_GATE=1"\necho "TARTCI_ROLE={role}"\n', executable=True)
        self.env["PULP_TARTCI_BIN"] = str(self.bin / "tartci")
        self.env["CMAKE_LOG"] = str(self.base / "cmake.log")
        return root

    def test_bootstrap_never_reconfigures_a_kept_build_dir_on_a_gate_host(self) -> None:
        for role in ("dev-overflow", "dedicated-builder", "a-role-added-later"):
            with self.subTest(role=role):
                root = self.reserved_host(role)
                build = self.make_build(root, f"build-{role}", reply=False)
                write(self.bin / "cmake", FAKE_CMAKE + f'echo "$@" >> "{self.base / "cmake.log"}"\n', executable=True)
                report = self.collect(root)
                self.assertEqual(self.keys(report), [], "no blocking finding to act on")
                self.assertTrue(any("serves the required gate" in n for n in report.notes), report.notes)
                proc = self.run_cli(root, "--bootstrap")
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertFalse((self.base / "cmake.log").exists(), "cmake must never run on the kept build dir")
                self.assertFalse(pre.has_codemodel_reply(build))
                self.assertFalse((build / pre.CODEMODEL_QUERY).exists(), "the query is not even touched")

    def test_control_a_light_host_does_reconfigure_the_kept_build_dir(self) -> None:
        root = self.reserved_host("light")
        build = self.make_build(root, "build", reply=False)
        write(self.bin / "cmake", FAKE_CMAKE + f'echo "$@" >> "{self.base / "cmake.log"}"\n', executable=True)
        self.only(self.collect(root), "file-api")
        proc = self.run_cli(root, "--bootstrap")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue((self.base / "cmake.log").exists())
        self.assertTrue(pre.has_codemodel_reply(build))

    def test_control_a_build_dir_with_the_reply_reports_nothing(self) -> None:
        root = self.complete()
        self.make_build(root, "build", reply=True)
        self.assertEqual(self.keys(self.collect(root)), [])

    def test_touching_the_query_alone_does_not_satisfy_the_check(self) -> None:
        # check_families skips its own reconfigure once the query exists, so the
        # query without a reply is exactly the state that must still be reported.
        root = self.complete()
        build = self.make_build(root, "build", reply=False)
        write(build / pre.CODEMODEL_QUERY, "")
        self.only(self.collect(root), "file-api")

    def test_vellum_dir_is_ignored_and_never_a_finding(self) -> None:
        root = self.complete()
        self.make_build(root, "build-vellum", reply=True, vellum="PULP_ENABLE_PULP_VELLUM")
        plain = self.make_build(root, "build", reply=True)
        report = self.collect(root)
        self.assertEqual(self.keys(report), [])
        self.assertTrue(any("build-vellum has Vellum options on" in n for n in report.notes), report.notes)
        self.assertTrue(any(f"gate build dir: {plain.name}" in n for n in report.notes), report.notes)
        self.assertTrue((root / "build-vellum").is_dir(), "the Vellum tree must be left in place")

    def test_declared_vellum_build_dir_is_blocking_and_names_the_unset(self) -> None:
        root = self.complete()
        vellum = self.make_build(root, "build-vellum", reply=True, vellum="PULP_ENABLE_PULP_VELLUM_GRAPH")
        finding = self.only(self.collect(root, PULP_GATES_BUILD_DIR=str(vellum)), "vellum-build")
        self.assertTrue(finding.fix[0].startswith("unset PULP_GATES_BUILD_DIR"))
        self.assertIsNone(finding.action, "a declared choice is never overridden silently")

    def run_cli(self, root: Path, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(HERE / "gates_preflight.py"), "--repo-root", str(root), *args],
                              capture_output=True, text=True, encoding="utf-8", check=False,
                              env={**os.environ, **self.env})

    def test_cli_exits_one_and_prints_every_fix_in_one_list(self) -> None:
        proc = self.run_cli(self.clone(depth=1))
        self.assertEqual(proc.returncode, 1, proc.stderr)
        for command in ("git fetch --unshallow", "git config --worktree core.hooksPath .githooks",
                        "./setup.sh --deps-only --non-interactive", "npm ci --cache"):
            self.assertIn(command, proc.stderr)
        self.assertIn("4 missing prerequisite(s)", proc.stderr)

    def test_cli_exits_zero_on_a_complete_checkout(self) -> None:
        proc = self.run_cli(self.complete())
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("preflight ok", proc.stderr)

    def test_bootstrap_fixes_every_automatic_prerequisite(self) -> None:
        root = self.clone(depth=1)
        self.make_build(root, "build", reply=False)
        before = self.run_cli(root)
        self.assertEqual(before.returncode, 1)
        proc = self.run_cli(root, "--bootstrap")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(sh(root, "git", "rev-parse", "--is-shallow-repository").strip(), "false")
        self.assertTrue((root / "external/vst3sdk/pluginterfaces").is_dir())
        self.assertTrue((root / JSX / "node_modules/esbuild/package.json").is_file())
        self.assertTrue(sh(root, "git", "config", "--get", "merge.pulp-gpu-ledger.driver").strip())
        shared = subprocess.run(["git", "config", "--local", "--get-regexp", "core.hookspath|merge.pulp"], cwd=root,
                                capture_output=True, text=True, encoding="utf-8", check=False).stdout
        self.assertEqual(shared.strip(), "", "the shared .git/config must stay untouched")
        self.assertTrue(pre.has_codemodel_reply(root / "build"))
        self.assertEqual(self.run_cli(root).returncode, 0)

    def test_bootstrap_leaves_a_failure_it_cannot_fix_reported(self) -> None:
        root = self.complete()
        vellum = self.make_build(root, "build-vellum", reply=True, vellum="PULP_ENABLE_PULP_VELLUM")
        env = {**os.environ, **self.env, "PULP_GATES_BUILD_DIR": str(vellum)}
        proc = subprocess.run([sys.executable, str(HERE / "gates_preflight.py"), "--repo-root", str(root),
                               "--bootstrap"], capture_output=True, text=True, encoding="utf-8", env=env,
                              check=False)
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertIn("[vellum-build]", proc.stderr)
        self.assertTrue(vellum.is_dir())

    def gates_checkout(self, shallow: bool) -> Path:
        """A checkout carrying the real gates.sh and preflight (and the two modules it imports)."""
        repo = HERE.parent.parent
        for rel in ("tools/scripts/gates.sh", "tools/scripts/gates_preflight.py", "tools/scripts/configure_only.py",
                    "tools/ci/source_selftests.py"):
            target = self.origin / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(repo / rel, target)
        sh(self.origin, "git", "add", "-A")
        sh(self.origin, "git", "commit", "-q", "-m", "gates")
        return self.clone(depth=1 if shallow else None)

    def run_gates(self, root: Path, *args: str, **extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", str(root / "tools/scripts/gates.sh"), *args], cwd=root, capture_output=True,
                              text=True, encoding="utf-8", check=False,
                              env={**os.environ, **self.env, **extra})

    def test_bootstrap_in_a_linked_worktree_leaves_the_main_clone_and_siblings_alone(self) -> None:
        main = self.complete("main-clone")
        sh(main, "git", "config", "--worktree", "--unset", "core.hooksPath")
        sh(main, "git", "config", "--worktree", "--unset", "merge.pulp-gpu-ledger.driver")
        linked = self.base / "linked"
        sh(main, "git", "worktree", "add", "-q", "--detach", str(linked))
        sibling = self.base / "sibling"
        sh(main, "git", "worktree", "add", "-q", "--detach", str(sibling))
        self.assertEqual(pre.install_worktree_hooks(linked), "")
        self.assertEqual(sh(linked, "git", "config", "--get", "core.hooksPath").strip(), ".githooks")
        for other in (main, sibling):
            proc = subprocess.run(["git", "config", "--get", "core.hooksPath"], cwd=other, capture_output=True,
                                  text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.stdout.strip(), "", f"{other.name} must not inherit the hook path")

    def test_configure_prerequisites_are_advisory_when_the_diff_needs_no_configure(self) -> None:
        root = self.clone(depth=1)
        report = pre.collect(root, self.env, needs_configure=False)
        blocking = [f.key for f in pre.blocking(report)]
        self.assertEqual(blocking, ["shallow", "githooks"], "only what fails whatever the diff is blocking")
        self.assertEqual([f.key for f in report.findings if f.advisory], ["sdks", "jsx-runtime"])

    def test_control_the_same_checkout_blocks_on_everything_when_the_diff_needs_a_configure(self) -> None:
        report = pre.collect(self.clone(depth=1), self.env, needs_configure=True)
        self.assertEqual([f.key for f in pre.blocking(report)], ["shallow", "githooks", "sdks", "jsx-runtime"])

    def test_cli_with_a_docs_only_base_exits_zero_on_a_fresh_worktree_that_has_only_the_always_checks(self) -> None:
        root = self.complete()
        shutil.rmtree(root / "external")
        shutil.rmtree(root / JSX / "node_modules")
        with mock.patch.object(pre, "diff_needs_configure", return_value=False):
            report = pre.collect(root, self.env, needs_configure=pre.diff_needs_configure(root, "origin/main"))
        self.assertEqual(pre.blocking(report), [])
        self.assertEqual(len(report.findings), 2)

    def test_the_diff_decides_through_the_real_reason_functions(self) -> None:
        root = self.clone()
        # A base that does not resolve means the diff cannot be read: fail closed.
        self.assertTrue(pre.diff_needs_configure(root, "no-such-ref"))
        # Control: a readable, empty diff reaches no configure lane.
        self.assertFalse(pre.diff_needs_configure(root, "origin/main"))
        write(root / "test/cmake/new_tests.cmake", "add_test(NAME x COMMAND y)\n")
        sh(root, "git", "add", "-A")
        sh(root, "git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", "c")
        self.assertTrue(pre.diff_needs_configure(root, "origin/main"))

    def test_gates_sh_stops_at_the_preflight_before_any_gate(self) -> None:
        proc = self.run_gates(self.gates_checkout(shallow=True))
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertIn("preflight found 4 missing prerequisite(s)", proc.stderr)
        self.assertIn("gates.sh --bootstrap", proc.stderr)
        # Nothing past the preflight ran: no gate banner, and origin/main was not refreshed.
        self.assertNotIn("▸", proc.stderr)
        self.assertNotIn("gates: base =", proc.stderr)

    def test_gates_sh_bootstrap_flag_is_not_taken_for_a_base_ref(self) -> None:
        root = self.gates_checkout(shallow=False)
        proc = self.run_gates(root, "--bootstrap")
        # Preflight passes after bootstrapping; the first thing past it names the real base.
        self.assertIn("preflight ok", proc.stderr)
        self.assertNotIn("--bootstrap", proc.stderr.split("preflight ok", 1)[1])

    def test_gates_sh_skip_is_listed_not_checked(self) -> None:
        source = (HERE / "gates.sh").read_text(encoding="utf-8")
        self.assertIn('echo "[preflight] NOT CHECKED: prerequisites (PULP_GATES_SKIP_PREFLIGHT=1)', source)
        proc = self.run_gates(self.gates_checkout(shallow=True), PULP_GATES_SKIP_PREFLIGHT="1")
        self.assertNotIn("preflight found", proc.stderr)


if __name__ == "__main__":
    unittest.main()
