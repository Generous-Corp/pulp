#!/usr/bin/env python3
"""Tests for tools/scripts/diff_cover_targets.py and its local_diff_cover.sh wiring.

Each case builds a throwaway git repository plus a hand-written CMake File API
reply, then shells out to the real helper (and, for the skip path, the real
local_diff_cover.sh). No compiler or coverage build is involved.

Run:
    python3 tools/scripts/test_diff_cover_targets.py
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parent
HELPER = SCRIPTS / "diff_cover_targets.py"
SHELL = SCRIPTS / "local_diff_cover.sh"
CTEST_POLICY = SCRIPTS.parent.parent / "scripts" / "coverage_ctest_policy.sh"
GOVERNED_BUILD = SCRIPTS.parent / "ci" / "governed-build.sh"
sys.path.insert(0, str(SCRIPTS))
import diff_cover_targets  # noqa: E402


def _git(root: pathlib.Path, *args: str) -> None:
    subprocess.run(["git", *args], cwd=root, check=True, capture_output=True)


# Fixture graph (arrows point from consumer to dependency):
#
#   pulp-test-state ──▶ pulp-state ──▶ pulp-runtime ◀── pulp-test-runtime
#   pulp-cli ─────────▶ pulp-state
#   pulp-test-cli-shellout ··(add_dependencies)··▶ pulp-cli
#   pulp-test-view ───▶ pulp-view ──▶ pulp-runtime
#   docs-gen (UTILITY) ─▶ pulp-state
TARGETS = {
    "pulp-runtime": ("STATIC_LIBRARY", "core/runtime",
                     ["core/runtime/src/log.cpp"], [], "core/runtime/libpulp-runtime.a"),
    "pulp-state": ("STATIC_LIBRARY", "core/state",
                   ["core/state/src/param_json.cpp", "core/state/src/store.cpp"],
                   ["pulp-runtime"], "core/state/libpulp-state.a"),
    "pulp-view": ("STATIC_LIBRARY", "core/view",
                  ["core/view/src/view.cpp"], ["pulp-runtime"], "core/view/libpulp-view.a"),
    "pulp-cli": ("EXECUTABLE", "tools/cli", ["tools/cli/main.cpp"],
                 ["pulp-state"], "tools/cli/pulp-cli"),
    "pulp-test-state": ("EXECUTABLE", "test", ["test/test_state.cpp"],
                        ["pulp-state"], "test/pulp-test-state"),
    "pulp-test-runtime": ("EXECUTABLE", "test", ["test/test_runtime.cpp"],
                          ["pulp-runtime"], "test/pulp-test-runtime"),
    "pulp-test-view": ("EXECUTABLE", "test", ["test/test_view.cpp"],
                       ["pulp-view"], "test/pulp-test-view"),
    "pulp-test-cli-shellout": ("EXECUTABLE", "test", ["test/test_cli_shellout.cpp"],
                               ["pulp-cli"], "test/pulp-test-cli-shellout"),
    "docs-gen": ("UTILITY", "docs", [], ["pulp-state"], None),
}


def write_codemodel(build: pathlib.Path, source: pathlib.Path) -> None:
    reply = build / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)
    refs = []
    for name, (kind, src_dir, sources, deps, artifact) in TARGETS.items():
        data = {
            "name": name,
            "id": f"{name}::@fixture",
            "type": kind,
            "paths": {"source": src_dir, "build": src_dir},
            "sources": [{"path": s} for s in sources],
            "dependencies": [{"id": f"{d}::@fixture"} for d in deps],
        }
        if artifact:
            data["artifacts"] = [{"path": artifact}]
        fname = f"target-{name}.json"
        (reply / fname).write_text(json.dumps(data))
        refs.append({"name": name, "id": data["id"], "jsonFile": fname})
    codemodel = {
        "paths": {"source": str(source), "build": str(build)},
        "configurations": [{"name": "Debug", "targets": refs}],
    }
    (reply / "codemodel-v2-fixture.json").write_text(json.dumps(codemodel))
    (reply / "index-2026-01-01T00-00-00-0000.json").write_text(json.dumps({
        "objects": [{"kind": "codemodel", "version": {"major": 2, "minor": 7},
                     "jsonFile": "codemodel-v2-fixture.json"}],
    }))


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._td.name).resolve() / "repo"
        self.root.mkdir()
        _git(self.root, "init", "-q", "-b", "main")
        _git(self.root, "config", "user.email", "fixture@example.com")
        _git(self.root, "config", "user.name", "Fixture")
        for _name, (_k, _d, sources, _deps, _a) in TARGETS.items():
            for src in sources:
                self._write(src, "int f() { return 1; }\n")
        self._write("core/state/include/pulp/state/store.hpp", "#pragma once\n")
        self._write("README.md", "hello\n")
        self.config = self.root / "coverage_config.json"
        self.config.write_text(json.dumps({"diff_cover_excludes": ["**/log.cpp"]}))
        _git(self.root, "add", ".")
        _git(self.root, "commit", "-qm", "base")
        _git(self.root, "branch", "base")
        self.build = self.root / "build-cov"
        write_codemodel(self.build, self.root)

    def tearDown(self) -> None:
        self._td.cleanup()

    def _write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def plan(self, env: dict | None = None) -> dict:
        result = subprocess.run(
            [sys.executable, str(HELPER), "plan", "--repo", str(self.root),
             "--build-dir", str(self.build), "--base", "base",
             "--config", str(self.config)],
            capture_output=True, text=True, check=True,
            env={**os.environ, **(env or {})},
        )
        return json.loads(result.stdout)


class PlanTests(Fixture):
    def test_core_cpp_change_maps_to_owner_and_its_consumers(self) -> None:
        self._write("core/state/src/param_json.cpp", "int f() { return 2; }\n")
        plan = self.plan()
        self.assertEqual(plan["mode"], "targeted", plan)
        self.assertEqual(
            plan["targets"],
            ["pulp-cli", "pulp-state", "pulp-test-cli-shellout", "pulp-test-state"],
        )
        # The unrelated library, its test, the dependency the change sits on,
        # and the UTILITY consumer are all left out.
        for absent in ("pulp-view", "pulp-test-view", "pulp-runtime",
                       "pulp-test-runtime", "docs-gen"):
            self.assertNotIn(absent, plan["targets"])
        self.assertEqual(plan["owners"]["core/state/src/param_json.cpp"]["via"], "source")

    def test_likely_tier_picks_the_test_that_includes_the_changed_file(self) -> None:
        self._write("test/test_state.cpp", '#include <pulp/state/store.hpp>\nint f() { return 1; }\n')
        _git(self.root, "commit", "-qam", "test includes store.hpp")
        _git(self.root, "branch", "-f", "base", "HEAD")
        self._write("core/state/src/store.cpp", "int f() { return 5; }\n")
        plan = self.plan()
        self.assertEqual((plan["mode"], plan["tier"]), ("targeted", "likely"), plan)
        self.assertEqual(plan["targets"], ["pulp-state", "pulp-test-state"])
        # The widening set is recorded for the escalation pass.
        self.assertIn("pulp-test-cli-shellout", plan["closure_targets"])

    def test_likely_tier_matches_a_test_named_after_the_file(self) -> None:
        self._write("core/view/src/view.cpp", "int f() { return 6; }\n")
        TARGETS_BACKUP = dict(TARGETS)
        try:
            TARGETS["pulp-test-view"] = ("EXECUTABLE", "test", ["test/test_view_paint.cpp"],
                                         ["pulp-view"], "test/pulp-test-view")
            shutil.rmtree(self.build)
            write_codemodel(self.build, self.root)
            plan = self.plan()
        finally:
            TARGETS.clear()
            TARGETS.update(TARGETS_BACKUP)
        self.assertEqual(plan["tier"], "likely", plan)
        self.assertEqual(plan["targets"], ["pulp-test-view", "pulp-view"])

    def test_header_change_never_uses_the_likely_tier(self) -> None:
        # An inline function in a header is only mapped in the TUs that use
        # it; a guessed subset could leave the changed line unmeasured, which
        # reads as a pass. Headers go straight to the closure.
        self._write("test/test_state.cpp", '#include <pulp/state/store.hpp>\n')
        _git(self.root, "commit", "-qam", "include")
        _git(self.root, "branch", "-f", "base", "HEAD")
        self._write("core/state/include/pulp/state/store.hpp", "#pragma once\ninline int g() { return 1; }\n")
        plan = self.plan()
        self.assertEqual(plan["tier"], "closure", plan)
        self.assertIn("not a listed translation unit", plan["reason"])

    def test_unscanned_owner_never_uses_the_likely_tier(self) -> None:
        self._write("test/test_state.cpp", '#include <pulp/state/store.hpp>\n')
        _git(self.root, "commit", "-qam", "include")
        _git(self.root, "branch", "-f", "base", "HEAD")
        self._write("core/state/src/store.cpp", "int f() { return 7; }\n")
        TARGETS_BACKUP = dict(TARGETS)
        try:
            kind, d, srcs, deps, _ = TARGETS["pulp-state"]
            TARGETS["pulp-state"] = (kind, d, srcs, deps, "core/state/libstate-internal.a")
            shutil.rmtree(self.build)
            write_codemodel(self.build, self.root)
            plan = self.plan()
        finally:
            TARGETS.clear()
            TARGETS.update(TARGETS_BACKUP)
        self.assertEqual(plan["tier"], "closure", plan)
        self.assertIn("not scanned", plan["reason"])

    def test_closure_tier_on_request(self) -> None:
        self._write("test/test_state.cpp", '#include <pulp/state/store.hpp>\n')
        _git(self.root, "commit", "-qam", "include")
        _git(self.root, "branch", "-f", "base", "HEAD")
        self._write("core/state/src/store.cpp", "int f() { return 8; }\n")
        out = subprocess.run(
            [sys.executable, str(HELPER), "plan", "--repo", str(self.root),
             "--build-dir", str(self.build), "--base", "base",
             "--config", str(self.config), "--tier", "closure"],
            capture_output=True, text=True, check=True)
        plan = json.loads(out.stdout)
        self.assertEqual(plan["tier"], "closure")
        self.assertEqual(plan["targets"], plan["closure_targets"])

    def test_committed_change_is_seen_as_well_as_working_tree(self) -> None:
        self._write("core/view/src/view.cpp", "int f() { return 3; }\n")
        _git(self.root, "commit", "-qam", "view change")
        plan = self.plan()
        self.assertEqual(plan["targets"], ["pulp-test-view", "pulp-view"])

    def test_unlisted_header_maps_through_its_cmake_directory(self) -> None:
        self._write("core/state/include/pulp/state/store.hpp", "#pragma once\nint g();\n")
        plan = self.plan()
        self.assertEqual(plan["mode"], "targeted", plan)
        self.assertIn("pulp-test-state", plan["targets"])
        self.assertEqual(
            plan["owners"]["core/state/include/pulp/state/store.hpp"]["via"],
            "directory core/state",
        )

    def test_docs_only_change_needs_no_build(self) -> None:
        self._write("README.md", "changed\n")
        plan = self.plan()
        self.assertEqual(plan["mode"], "none", plan)
        self.assertEqual(plan["targets"], [])

    def test_test_only_and_excluded_native_changes_need_no_build(self) -> None:
        # test/ is dropped by the llvm-cov ignore regex and log.cpp by
        # diff_cover_excludes, so diff-cover has no line of either to measure.
        self._write("test/test_state.cpp", "int f() { return 9; }\n")
        self._write("core/runtime/src/log.cpp", "int f() { return 9; }\n")
        plan = self.plan()
        self.assertEqual(plan["mode"], "none", plan)

    def test_unclassifiable_change_without_a_measured_line_keeps_all(self) -> None:
        # A .cmake edit or a deleted native file has no line to measure, but
        # the preflight refuses to prove it cannot affect the native build;
        # the selector must not turn that refusal into a skip.
        for label, mutate in (
            ("cmake", lambda: self._write("test/cmake/extra.cmake", "add_test(x)\n")),
            ("delete", lambda: (self.root / "test/test_view.cpp").unlink()),
        ):
            with self.subTest(label):
                mutate()
                _git(self.root, "add", "-A", "--", "test")
                plan = self.plan()
                self.assertEqual(plan["mode"], "all", plan)
                self.assertIn("keeps the whole-tree build", plan["reason"])
                _git(self.root, "reset", "-q", "--hard", "HEAD")
                _git(self.root, "clean", "-qfd", "--", "test")

    def test_measured_file_without_an_owner_falls_back_to_all(self) -> None:
        self._write("orphan/new_feature.cpp", "int h() { return 1; }\n")
        _git(self.root, "add", "-N", "orphan/new_feature.cpp")
        self._write("core/state/src/store.cpp", "int f() { return 4; }\n")
        plan = self.plan()
        self.assertEqual(plan["mode"], "all", plan)
        self.assertIn("orphan/new_feature.cpp", plan["reason"])

    def test_missing_codemodel_falls_back_to_all(self) -> None:
        self._write("core/state/src/store.cpp", "int f() { return 4; }\n")
        shutil.rmtree(self.build / ".cmake")
        plan = self.plan()
        self.assertEqual(plan["mode"], "all", plan)
        self.assertIn("File API", plan["reason"])

    def test_external_selector_seam_is_honoured_and_validated(self) -> None:
        self._write("core/state/src/store.cpp", "int f() { return 4; }\n")
        good = self.root / "selector.py"
        good.write_text(
            "import json\nprint(json.dumps({'schema': 'pulp.diff-cover-plan/v1',"
            " 'mode': 'targeted', 'targets': ['x'], 'reason': 'external'}))\n"
        )
        plan = self.plan({"PULP_DIFF_COVER_SELECTOR_CMD": f"{sys.executable} {good}"})
        self.assertEqual((plan["mode"], plan["targets"]), ("targeted", ["x"]))
        bad = self.root / "bad.py"
        bad.write_text("print('{}')\n")
        plan = self.plan({"PULP_DIFF_COVER_SELECTOR_CMD": f"{sys.executable} {bad}"})
        self.assertEqual(plan["mode"], "all")


class TestSelectionTests(Fixture):
    """`tests` keeps only CTests whose command runs a built target."""

    def test_selects_tests_by_artifact_and_placeholder(self) -> None:
        b = self.build
        ctest_json = {"tests": [
            {"name": "state reads", "command": [str(b / "test/pulp-test-state"), "state reads"]},
            {"name": "view paints", "command": [str(b / "test/pulp-test-view"), "view paints"]},
            {"name": "cli help", "command": ["/usr/bin/env", f"PULP_CLI={b / 'tools/cli/pulp-cli'}", "sh", "-c", "x"]},
            {"name": "cli version", "command": [str(b / "test/pulp-test-cli-shellout")]},
            {"name": "pulp-test-state_NOT_BUILT-0123abc", "properties": []},
            {"name": "pulp-test-view_NOT_BUILT-0123abc", "properties": []},
            {"name": "python lint", "command": ["/usr/bin/python3", "lint.py"]},
        ]}
        shim_dir = self.root / "shim"
        shim_dir.mkdir()
        ctest = shim_dir / "ctest"
        ctest.write_text("#!/bin/sh\ncat <<'JSON'\n" + json.dumps(ctest_json) + "\nJSON\n")
        ctest.chmod(0o755)
        plan_file = self.root / "plan.json"
        plan_file.write_text(json.dumps({"targets": [
            "pulp-cli", "pulp-state", "pulp-test-cli-shellout", "pulp-test-state"]}))
        out = self.root / "selected.txt"
        result = subprocess.run(
            [sys.executable, str(HELPER), "tests", "--build-dir", str(b),
             "--plan", str(plan_file), "--out", str(out)],
            capture_output=True, text=True,
            env={**os.environ, "PATH": f"{shim_dir}{os.pathsep}{os.environ['PATH']}"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(out.read_text().splitlines(), [
            "state reads", "cli help", "cli version",
            "pulp-test-state_NOT_BUILT-0123abc",
        ])


class ShellWiringTests(unittest.TestCase):
    """The real local_diff_cover.sh skips a diff with nothing to measure."""

    def test_test_only_native_change_skips_before_any_build_command(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td).resolve() / "wt"
            (root / "tools" / "scripts").mkdir(parents=True)
            (root / "tools" / "ci").mkdir(parents=True)
            (root / "scripts").mkdir()
            for src, dst in ((SHELL, "tools/scripts/local_diff_cover.sh"),
                             (HELPER, "tools/scripts/diff_cover_targets.py"),
                             (CTEST_POLICY, "scripts/coverage_ctest_policy.sh"),
                             (GOVERNED_BUILD, "tools/ci/governed-build.sh")):
                (root / dst).write_text(src.read_text())
            (root / "tools" / "scripts" / "coverage_config.json").write_text(json.dumps({
                "diff_coverage_fail_under": 75, "compare_branch": "base",
                "min_free_disk_gib": 0, "diff_cover_excludes": [],
            }))
            _git(root, "init", "-q", "-b", "main")
            _git(root, "config", "user.email", "f@example.com")
            _git(root, "config", "user.name", "F")
            (root / "test").mkdir()
            (root / "test" / "test_x.cpp").write_text("int t() { return 1; }\n")
            _git(root, "add", ".")
            _git(root, "commit", "-qm", "base")
            _git(root, "branch", "base")
            (root / "test" / "test_x.cpp").write_text("int t() { return 2; }\n")

            spy = root / "spy"
            spy.mkdir()
            marker = root / "ran"
            for cmd in ("df", "cmake", "ctest", "clang", "llvm-cov", "llvm-profdata"):
                shim = spy / cmd
                shim.write_text(f"#!/bin/sh\necho {cmd} >> '{marker}'\nexit 97\n")
                shim.chmod(0o755)
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith("PULP_DIFF_COVER") and k != "PULP_SKIP_DIFF_COVER"}
            env["PATH"] = f"{spy}{os.pathsep}{env['PATH']}"
            env["PULP_DIFF_COVER_COMPARE_BRANCH"] = "base"
            result = subprocess.run(["bash", str(root / "tools/scripts/local_diff_cover.sh")],
                                    cwd=root, env=env, capture_output=True, text=True,
                                    timeout=120)
            out = result.stdout + result.stderr
            self.assertEqual(result.returncode, 0, out)
            self.assertIn("no changed file has a line diff-cover measures", out)
            self.assertFalse(marker.exists(), marker.read_text() if marker.exists() else "")

            # Negative control: the same change with the selector forced to
            # the whole tree must NOT skip (it proceeds to the build path,
            # where the spy shims make it fail).
            env["PULP_DIFF_COVER_SELECT"] = "all"
            forced = subprocess.run(["bash", str(root / "tools/scripts/local_diff_cover.sh")],
                                    cwd=root, env=env, capture_output=True, text=True,
                                    timeout=120)
            self.assertNotEqual(forced.returncode, 0, forced.stdout + forced.stderr)
            self.assertNotIn("no changed file has a line diff-cover measures",
                             forced.stdout + forced.stderr)


class ContractTests(unittest.TestCase):
    def _widen(self, tier: str, select: str) -> subprocess.CompletedProcess:
        """Call the real widen_if_likely with exec stubbed out."""
        with tempfile.TemporaryDirectory() as td:
            snippet = f"""
source "{SHELL}"
exec() {{ echo "EXEC $*"; }}
coverage_build_identity() {{ echo same; }}
COVERAGE_BUILD_IDENTITY_START=same
BUILD_ID_FILE="{td}/id"
plan_tier={tier}
DIFF_COVER_SELECT={select}
widen_if_likely "fell short"
echo "id=$(cat "{td}/id" 2>/dev/null)"
echo returned
"""
            return subprocess.run(
                ["bash", "-c", snippet], capture_output=True, text=True, timeout=60,
                env={**os.environ, "PULP_DIFF_COVER_LIB_ONLY": "1"},
            )

    def test_likely_tier_widens_to_the_closure_and_keeps_its_build(self) -> None:
        result = self._widen("likely", "affected")
        self.assertIn("EXEC env PULP_DIFF_COVER_SELECT=closure bash", result.stdout)
        # The identity is recorded so the widened run reuses build-cov
        # instead of deleting it as unproven.
        self.assertIn("id=same", result.stdout)

    def test_widening_happens_once_and_only_from_the_likely_tier(self) -> None:
        for tier, select in (("closure", "closure"), ("closure", "affected"),
                             ("likely", "closure"), ("", "affected")):
            with self.subTest(tier=tier, select=select):
                result = self._widen(tier, select)
                self.assertNotIn("EXEC", result.stdout)
                self.assertIn("returned", result.stdout)

    def test_non_native_proofs_match_the_shell_preflight(self) -> None:
        shell = SHELL.read_text()
        block = re.search(r"known_non_native_suffixes = \{(.*?)\}", shell, re.S)
        self.assertIsNotNone(block)
        self.assertEqual(set(re.findall(r'"(\.[a-z]+)"', block.group(1))),
                         diff_cover_targets.KNOWN_NON_NATIVE_SUFFIXES)
        paths = re.search(r"known_non_native_paths = \{(.*?)\}", shell, re.S)
        self.assertEqual(set(re.findall(r'"([^"]+)"', paths.group(1))),
                         diff_cover_targets.KNOWN_NON_NATIVE_PATHS)

    def test_native_suffixes_match_the_shell_preflight(self) -> None:
        block = re.search(r"coverable_suffixes = \{(.*?)\}", SHELL.read_text(), re.S)
        self.assertIsNotNone(block)
        shell_suffixes = set(re.findall(r'"(\.[a-z]+)"', block.group(1)))
        self.assertEqual(shell_suffixes, diff_cover_targets.NATIVE_SUFFIXES)


if __name__ == "__main__":
    unittest.main()
