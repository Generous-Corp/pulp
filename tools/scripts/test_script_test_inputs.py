#!/usr/bin/env python3
"""Tests for tools/scripts/script_test_inputs.py.

A temp repository holds one Python test importing a local module (which
itself names a data file literally), one Node test importing a relative
module, one shell test sourcing a helper, one `python -m` test, and one
cmake-driven test. What must hold:
- each declared entry lists the entry script, its transitive local inputs and
  the repository paths its command line names, all repo-relative and sorted;
- the cmake-driven test and a test with no command get no entry (fail-closed
  stays with the shadow selector);
- `--check` is clean against a fresh `--write`, reports a stale entry after an
  import is added, reports a missing entry for a new test, and ignores tests
  absent from the current inventory (another platform's list);
- an unreadable inventory exits 2;
- compiled executables fold into the same list from configure evidence: a
  declared fixture is listed, an undeclared reader is `data: undeclared`,
  and `--check` fails on a NEW undeclared source against the base list;
- a source that finds the checkout by walking two levels up from its working
  directory or from its own `__FILE__` reads it, and one level up (the build
  tree) does not;
- the scan covers an executable defined outside test/ when ctest runs it
  (as a command or through a Catch2 discovery include), keyed by the program
  name ctest runs, and not one a test only passes as an argument;
- WHOLE_CHECKOUT reads as `data: whole_checkout`, never undeclared;
- every scanned executable is keyed by its artifact, whether or not it lives
  under test/, `executable_targets` maps a renamed one back to its target,
  and two targets building one artifact refuse to be listed;
- a build configured off the gate's profile (examples ON, a sanitizer, a
  Debug build) cannot stand for the list's compiled entries: `--check`
  still compares the script entries, then reports the compiled half
  SKIPPED with the switch that differs, and `--write` refuses.

Run:
    python3 tools/scripts/test_script_test_inputs.py
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import script_test_inputs as sti  # noqa: E402

SCRIPT = HERE / "script_test_inputs.py"


def write(root: Path, rel: str, text: str) -> Path:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")
    return p


class Repo:
    def __init__(self, tmp: Path) -> None:
        self.root = tmp / "repo"
        self.root.mkdir()
        write(self.root, "tools/scripts/test_alpha.py", "import alpha_lib\nfrom helpers import sub\n")
        write(self.root, "tools/scripts/alpha_lib.py", "DATA = 'docs/status/alpha.yaml'\n")
        write(self.root, "tools/scripts/helpers/__init__.py", "")
        write(self.root, "tools/scripts/helpers/sub.py", "x = 1\n")
        write(self.root, "docs/status/alpha.yaml", "a: 1\n")
        write(self.root, "tools/import/run.mjs", "import { f } from './lib/util.mjs';\nconst c = require('./config.json');\n")
        write(self.root, "tools/import/lib/util.mjs", "export const f = 1;\n")
        write(self.root, "tools/import/config.json", "{}\n")
        write(self.root, "tools/scripts/tmp_leak_guard.py", "import alpha_lib\n")
        write(self.root, "tools/scripts/test_beta.sh", "#!/bin/bash\nsource \"$(dirname \"$0\")/lib.sh\"\n. tools/scripts/other.sh\n")
        write(self.root, "tools/scripts/lib.sh", "x=1\n")
        write(self.root, "tools/scripts/other.sh", "y=1\n")
        write(self.root, "tools/scripts/test_mod.py", "import alpha_lib\n")
        write(self.root, "test/fixtures/data.txt", "d\n")
        self.build = tmp / "build"
        self.build.mkdir()

    def inventory(self) -> dict:
        r = str(self.root)
        return {"tests": [
            {"name": "alpha", "command": ["/usr/bin/python3", f"{r}/tools/scripts/test_alpha.py", f"{r}/test/fixtures/data.txt"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": f"{r}/tools/scripts"}]},
            {"name": "node-run", "command": ["/opt/homebrew/bin/node", "--test", f"{r}/tools/import/run.mjs"], "properties": []},
            {"name": "guarded-node-run",
             "command": ["/usr/bin/python3", f"{r}/tools/scripts/tmp_leak_guard.py", "--ignore", "x-*", "--",
                         "/opt/homebrew/bin/node", "--test", f"{r}/tools/import/run.mjs"],
             "properties": []},
            {"name": "beta", "command": ["/bin/bash", f"{r}/tools/scripts/test_beta.sh"], "properties": []},
            {"name": "mod", "command": ["/usr/bin/python3", "-m", "test_mod"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": f"{r}/tools/scripts"}]},
            {"name": "cmake-nested", "command": ["/opt/homebrew/bin/cmake", "-P", f"{r}/test/x.cmake"], "properties": []},
            {"name": "no-command", "command": [], "properties": []},
            # Registered names model the gate's materialized-runtime tests;
            # empty commands keep this generic fixture's declared list stable.
            {"name": "pulp-materialized-runtime-conformance", "command": [], "properties": []},
            {"name": "pulp-materialized-runtime-node-dependencies", "command": [], "properties": []},
            {"name": "compiled", "command": [f"{self.build}/test/pulp-test-x", "case"], "properties": []},
        ]}


class ProfileIndependenceTests(unittest.TestCase):
    """`--write` must not depend on the local configure: the gate builds with
    examples OFF, and its build directory has no fixed name."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.repo = Repo(self.tmp)

    def _node_inventory(self, *, complete: bool) -> dict:
        r = str(self.repo.root)
        tests = [{"name": "node-unit", "command": ["/usr/bin/node", "unit.mjs"], "properties": []}]
        if complete:
            tests += [
                {"name": "pulp-materialized-runtime-conformance", "command": ["/usr/bin/node", "conformance.mjs"], "properties": []},
                {"name": "pulp-materialized-runtime-node-dependencies", "command": ["/usr/bin/node", "--test", "deps.mjs"], "properties": []},
            ]
        return {"tests": tests, "root": r}

    def _profile_cache(self, build: Path) -> None:
        write(build, "CMakeCache.txt", "CMAKE_BUILD_TYPE:STRING=Release\nPULP_BUILD_EXAMPLES:BOOL=OFF\nPULP_SANITIZER:STRING=\nPULP_GPU_AUDIO_EXACT_PROVIDER_PROOF:BOOL=OFF\n")

    def test_node_dependencies_missing_registration_refuses_profile(self) -> None:
        self._profile_cache(self.repo.build)
        reason = sti.outside_gate_profile_build(self.repo.build, self._node_inventory(complete=False))
        self.assertEqual(len(reason), 1)
        self.assertIn("jsx-runtime npm dependencies", reason[0])
        self.assertIn("npm ci --prefix tools/import-design/jsx-runtime", reason[0])

    def test_gate_shaped_node_dependency_registration_is_accepted(self) -> None:
        self._profile_cache(self.repo.build)
        self.assertEqual(sti.outside_gate_profile_build(self.repo.build, self._node_inventory(complete=True)), [])

    def test_cli_write_refuses_missing_node_dependency_registration_without_touching_list(self) -> None:
        self._profile_cache(self.repo.build)
        write(self.repo.build, "test/test-data/executables.json", json.dumps({"executables": {}}))
        write(self.repo.build, "test/test-data/runtime-targets.json", json.dumps({"artifacts": {}}))
        inv = self._node_inventory(complete=False)
        inv_path = self.repo.build / "inv.json"
        inv_path.write_text(json.dumps(inv), encoding="utf-8")
        out = self.repo.build / "list.json"
        out.write_bytes(b"sentinel\n")
        proc = subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(self.repo.root),
                               "--build-dir", str(self.repo.build), "--inventory-json", str(inv_path),
                               "--list", str(out), "--write"], capture_output=True, text=True,
                              timeout=60, env=tool_env(event=None, strict=False))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("npm ci --prefix tools/import-design/jsx-runtime", proc.stderr)
        self.assertEqual(out.read_bytes(), b"sentinel\n")

    def inventory_with_example(self, keep_root: Path | None = None) -> dict:
        r = str(self.repo.root)
        inv = self.repo.inventory()
        write(self.repo.root, "examples/pulp-gain/CMakeLists.txt", "add_test(auval-PulpGain ...)\n")
        write(self.repo.root, "tools/ci/run_auval.py", "x = 1\n")
        inv["backtraceGraph"] = {"files": [f"{r}/examples/pulp-gain/CMakeLists.txt", f"{r}/test/cmake/quality_tests.cmake"],
                                 "nodes": [{"file": 0}, {"file": 1}, {"parent": 1}]}
        inv["tests"] += [
            {"name": "auval-PulpGain", "command": ["/usr/bin/python3", f"{r}/tools/ci/run_auval.py"], "properties": [], "backtrace": 0},
            {"name": "auval-helper-selftest", "command": ["/usr/bin/python3", f"{r}/tools/ci/run_auval.py"], "properties": [], "backtrace": 2},
        ]
        return inv

    def test_example_registered_tests_are_excluded_so_an_examples_on_build_writes_the_gate_list(self) -> None:
        inv = self.inventory_with_example()
        with_examples = sti.build_list(inv, self.repo.root)
        self.assertNotIn("auval-PulpGain", with_examples["tests"])
        self.assertIn("auval-helper-selftest", with_examples["tests"], "a test registered from test/ is not an example")
        gate_only = dict(inv, tests=[t for t in inv["tests"] if t["name"] != "auval-PulpGain"])
        self.assertEqual(json.dumps(with_examples, indent=1, sort_keys=True),
                         json.dumps(sti.build_list(gate_only, self.repo.root), indent=1, sort_keys=True))
        self.assertEqual(sti.outside_gate_profile(inv, self.repo.root), {"auval-PulpGain"})

    def test_the_census_profile_decides_the_scope(self) -> None:
        inv = self.inventory_with_example()
        write(self.repo.root, "docs/status/consumption-profiles.json", json.dumps(
            {"profiles": {"darwin-a": {"build_scope": {"PULP_BUILD_TESTS": "ON", "PULP_BUILD_EXAMPLES": "ON"}}}}))
        self.assertEqual(sti.gate_build_scope(self.repo.root)["PULP_BUILD_EXAMPLES"], "ON")
        self.assertIn("auval-PulpGain", sti.build_list(inv, self.repo.root)["tests"])
        write(self.repo.root, "docs/status/consumption-profiles.json", "not json")
        self.assertEqual(sti.gate_build_scope(self.repo.root), sti.GATE_BUILD_SCOPE)

    def test_an_entry_generated_into_the_build_tree_records_a_token_not_the_directory_name(self) -> None:
        lists = {}
        for name in ("build-gate", "bld"):
            build = self.tmp / name
            write(build, "test/run_mutation_control.py", "import sys\n")
            inv = self.repo.inventory()
            inv["tests"].append({"name": "mutation-control", "properties": [],
                                 "command": ["/usr/bin/python3", f"{build}/test/run_mutation_control.py", f"{build}/test/pulp-test-x"]})
            lists[name] = json.dumps(sti.build_list(inv, self.repo.root, build), indent=1, sort_keys=True)
        self.assertEqual(lists["build-gate"], lists["bld"])
        entry = json.loads(lists["bld"])["tests"]["mutation-control"]
        self.assertEqual(entry["entry"], "${CMAKE_BINARY_DIR}/test/run_mutation_control.py")
        self.assertNotIn("bld", lists["bld"])
        self.assertNotIn("build-gate", lists["build-gate"])

    def test_write_from_two_build_directories_is_byte_identical_through_the_cli(self) -> None:
        outs = {}
        for name in ("build-gate", "elsewhere"):
            build = self.tmp / name
            write(build, "test/run_mutation_control.py", "import sys\n")
            inv = self.inventory_with_example()
            inv["tests"].append({"name": "mutation-control", "properties": [],
                                 "command": ["/usr/bin/python3", f"{build}/test/run_mutation_control.py"]})
            inv_path = build / "inv.json"; inv_path.write_text(json.dumps(inv), encoding="utf-8")
            out = build / "list.json"
            proc = subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(self.repo.root), "--build-dir", str(build),
                                   "--inventory-json", str(inv_path), "--list", str(out), "--write"],
                                  capture_output=True, text=True, timeout=60, env=tool_env(event=None, strict=False))
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("excluded 1 test(s) registered from examples/", proc.stdout)
            outs[name] = out.read_bytes()
        self.assertEqual(outs["build-gate"], outs["elsewhere"])
        self.assertIn(b"${CMAKE_BINARY_DIR}/test/run_mutation_control.py", outs["elsewhere"])
        self.assertNotIn(b"auval-PulpGain", outs["elsewhere"])


class BuildListTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.repo = Repo(Path(tmp.name))
        self.lst = sti.build_list(self.repo.inventory(), self.repo.root)

    def test_python_entry_lists_transitive_imports_literals_and_args(self) -> None:
        a = self.lst["tests"]["alpha"]
        self.assertEqual(a["kind"], "python")
        self.assertEqual(a["entry"], "tools/scripts/test_alpha.py")
        self.assertEqual(a["inputs"], sorted([
            "tools/scripts/test_alpha.py", "tools/scripts/alpha_lib.py",
            "tools/scripts/helpers/__init__.py", "tools/scripts/helpers/sub.py",
            "docs/status/alpha.yaml", "test/fixtures/data.txt"]))

    def test_node_entry_follows_relative_imports_and_requires(self) -> None:
        n = self.lst["tests"]["node-run"]
        self.assertEqual(n["inputs"], ["tools/import/config.json", "tools/import/lib/util.mjs", "tools/import/run.mjs"])

    def test_a_temp_leak_guarded_command_is_declared_as_what_it_wraps(self) -> None:
        # Classified as the guard, the node test would lose its import walk and
        # a change to its imports would no longer select it.
        n = self.lst["tests"]["guarded-node-run"]
        self.assertEqual(n["kind"], "node")
        self.assertEqual(n["entry"], "tools/import/run.mjs")
        self.assertEqual(n["inputs"], [
            "docs/status/alpha.yaml", "tools/import/config.json", "tools/import/lib/util.mjs", "tools/import/run.mjs",
            "tools/scripts/alpha_lib.py", "tools/scripts/tmp_leak_guard.py"])

    def test_shell_entry_follows_source_lines(self) -> None:
        b = self.lst["tests"]["beta"]
        self.assertEqual(b["inputs"], ["tools/scripts/lib.sh", "tools/scripts/other.sh", "tools/scripts/test_beta.sh"])

    def test_python_dash_m_resolves_in_the_working_directory(self) -> None:
        m = self.lst["tests"]["mod"]
        self.assertEqual(m["entry"], "tools/scripts/test_mod.py")
        self.assertIn("tools/scripts/alpha_lib.py", m["inputs"])

    def test_undeclarable_tests_get_no_entry(self) -> None:
        for name in ("cmake-nested", "no-command", "compiled"):
            self.assertNotIn(name, self.lst["tests"], name)

    def test_a_shared_walker_lists_what_a_fresh_one_does_for_every_test(self) -> None:
        # build_list() shares one memoizing Walker across tests; each record
        # must equal the one a walker that saw no other test produces.
        for t in self.repo.inventory()["tests"]:
            if t["name"] in self.lst["tests"]:
                with self.subTest(test=t["name"]):
                    self.assertEqual(self.lst["tests"][t["name"]], sti.inputs_for(t, self.repo.root))

    def test_an_argument_longer_than_a_file_name_is_not_an_input(self) -> None:
        inv = self.repo.inventory()
        long_arg = "key=true.*" * 40  # 400 bytes in one path component
        inv["tests"].append({"name": "long-arg", "properties": [{"name": "WORKING_DIRECTORY",
                             "value": str(self.repo.root)}],
                             "command": ["/usr/bin/python3", str(self.repo.root / "tools/scripts/test_alpha.py"),
                                         long_arg]})
        lst = sti.build_list(inv, self.repo.root)
        want = [p for p in self.lst["tests"]["alpha"]["inputs"] if p != "test/fixtures/data.txt"]
        self.assertEqual(lst["tests"]["long-arg"]["inputs"], want)

    def test_output_is_sorted_and_repo_relative(self) -> None:
        self.assertEqual(list(self.lst["tests"]), sorted(self.lst["tests"]))
        for rec in self.lst["tests"].values():
            for p in rec["inputs"]:
                self.assertFalse(p.startswith("/"), p)


def tool_env(*, event: str | None, strict: bool) -> dict[str, str]:
    """Every case pins the event environment the script reads, so the selftest
    gives the same verdict inside a pull-request job, a merge-group job (where
    the runner exports GITHUB_EVENT_NAME=merge_group and the script would
    otherwise go advisory under a case that expects a block), and a shell."""
    env = {k: v for k, v in os.environ.items() if k not in ("GITHUB_EVENT_NAME", "PULP_SCRIPT_INPUTS_STRICT")}
    if event is not None:
        env["GITHUB_EVENT_NAME"] = event
    if strict:
        env["PULP_SCRIPT_INPUTS_STRICT"] = "1"
    return env


class CheckModeTests(unittest.TestCase):
    def run_tool(self, repo: Repo, *args: str, inventory: dict | None = None,
                 event: str = "pull_request", strict: bool = False) -> subprocess.CompletedProcess[str]:
        inv = repo.build / "inv.json"
        inv.write_text(json.dumps(inventory or repo.inventory()), encoding="utf-8")
        return subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(repo.root),
                               "--inventory-json", str(inv), *args], capture_output=True, text=True, timeout=60,
                              env=tool_env(event=event, strict=strict))

    def test_write_then_check_is_clean_then_drifts_on_a_new_import(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            clean = self.run_tool(repo, "--check")
            self.assertEqual(clean.returncode, 0, clean.stdout + clean.stderr)
            self.assertIn("in sync", clean.stdout)
            write(repo.root, "tools/scripts/alpha_lib.py", "import helpers.sub\nDATA = 'docs/status/alpha.yaml'\n")
            write(repo.root, "tools/scripts/extra.py", "z = 1\n")
            write(repo.root, "tools/scripts/alpha_lib.py", "import extra\nDATA = 'docs/status/alpha.yaml'\n")
            stale = self.run_tool(repo, "--check")
            self.assertEqual(stale.returncode, 1, stale.stdout)
            self.assertIn("stale entry: alpha", stale.stdout)

    def test_check_reports_a_new_test_and_ignores_tests_absent_here(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            inv = repo.inventory()
            inv["tests"].append({"name": "gamma", "command": ["/usr/bin/python3", f"{repo.root}/tools/scripts/test_mod.py"], "properties": []})
            new = self.run_tool(repo, "--check", inventory=inv)
            self.assertEqual(new.returncode, 1)
            self.assertIn("missing from list: gamma", new.stdout)
            fewer = {"tests": [t for t in repo.inventory()["tests"] if t["name"] != "beta"]}
            self.assertEqual(self.run_tool(repo, "--check", inventory=fewer).returncode, 0)

    def git_repo(self, repo: Repo) -> None:
        env = {"PATH": "/usr/bin:/bin:/opt/homebrew/bin", "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
               "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
        g = lambda *a: subprocess.run(["git", "-C", str(repo.root), *a], check=True, env=env, capture_output=True)
        g("init", "-q", "-b", "main"); g("add", "-A"); g("commit", "-q", "-m", "base")
        g("branch", "base-ref")
        self.g = g

    def test_diff_scoped_check_ignores_drift_the_change_does_not_touch(self) -> None:
        """Main moved a script this PR never touched: advisory note, exit 0."""
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)
            # Someone else's drift (simulating main): alpha_lib gains an import, but this
            # change (HEAD vs base-ref) touches only an unrelated file.
            write(repo.root, "tools/scripts/extra.py", "z = 1\n")
            write(repo.root, "tools/scripts/alpha_lib.py", "import extra\nDATA = 'docs/status/alpha.yaml'\n")
            self.g("add", "-A"); self.g("commit", "-q", "-m", "main moved (already in base)")
            self.g("branch", "-f", "base-ref", "HEAD")
            write(repo.root, "README.md", "unrelated\n"); self.g("add", "-A"); self.g("commit", "-q", "-m", "pr")
            proc = self.run_tool(repo, "--check", "--base", "base-ref")
            self.assertEqual(proc.returncode, 0, proc.stdout)
            self.assertIn("drifted from scripts this change does not touch", proc.stdout)
            self.assertIn("stale entry: alpha", proc.stdout)
            full = self.run_tool(repo, "--check", "--full")
            self.assertEqual(full.returncode, 1, full.stdout)

    def test_diff_scoped_check_blocks_drift_in_a_script_the_change_touches(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)
            write(repo.root, "tools/scripts/extra.py", "z = 1\n")
            write(repo.root, "tools/scripts/alpha_lib.py", "import extra\nDATA = 'docs/status/alpha.yaml'\n")
            self.g("add", "-A"); self.g("commit", "-q", "-m", "pr edits a listed input")
            proc = self.run_tool(repo, "--check", "--base", "base-ref")
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("in scripts this change touches", proc.stdout)
            self.assertIn("stale entry: alpha", proc.stdout)

    def test_diff_scoped_check_blocks_a_new_test_whose_script_the_change_adds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)
            write(repo.root, "tools/scripts/test_gamma.py", "x = 1\n")
            self.g("add", "-A"); self.g("commit", "-q", "-m", "pr adds a test script")
            inv = repo.inventory()
            inv["tests"].append({"name": "gamma", "command": ["/usr/bin/python3", f"{repo.root}/tools/scripts/test_gamma.py"], "properties": []})
            proc = self.run_tool(repo, "--check", "--base", "base-ref", inventory=inv)
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("missing from list: gamma", proc.stdout)

    def test_only_tracked_paths_are_inputs(self) -> None:
        """A build directory inside the checkout or an untracked file must not
        become an input: the list would then depend on where it was generated."""
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.git_repo(repo)
            inv = repo.inventory()
            build_inside = repo.root / "build-here"; build_inside.mkdir()
            write(repo.root, "tools/scripts/untracked_helper.py", "u = 1\n")  # exists, not committed
            inv["tests"].append({"name": "drift-self", "command": ["/usr/bin/python3", f"{repo.root}/tools/scripts/test_mod.py",
                                                                    "--repo-root", str(repo.root), "--build-dir", str(build_inside),
                                                                    f"{repo.root}/tools/scripts/untracked_helper.py"], "properties": []})
            lst = sti.build_list(inv, repo.root)
            self.assertEqual(lst["tests"]["drift-self"]["inputs"],
                             ["docs/status/alpha.yaml", "tools/scripts/alpha_lib.py", "tools/scripts/test_mod.py"])

    def test_missing_entry_blocks_only_when_the_change_touches_its_script(self) -> None:
        """A test present only in another platform's inventory shares directory
        inputs with everything; touching that directory must not block."""
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)
            write(repo.root, "tools/scripts/new_tool.py", "n = 1\n"); self.g("add", "-A"); self.g("commit", "-q", "-m", "pr touches tools/scripts")
            inv = repo.inventory()
            inv["tests"].append({"name": "linux-only", "command": ["/usr/bin/python3", f"{repo.root}/tools/scripts/test_alpha.py", f"{repo.root}/tools/scripts"], "properties": []})
            proc = self.run_tool(repo, "--check", "--base", "base-ref", inventory=inv)
            self.assertEqual(proc.returncode, 0, proc.stdout)
            self.assertIn("missing from list: linux-only", proc.stdout)   # advisory, not blocking

    def test_merge_group_reports_blocking_drift_as_a_warning_and_exits_0(self) -> None:
        """The pull-request head is the enforcement point; a merge group must
        never eject a batch over a stale generated list."""
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)
            write(repo.root, "tools/scripts/extra.py", "z = 1\n")
            write(repo.root, "tools/scripts/alpha_lib.py", "import extra\nDATA = 'docs/status/alpha.yaml'\n")
            self.g("add", "-A"); self.g("commit", "-q", "-m", "pr edits a listed input")
            proc = self.run_tool(repo, "--check", "--base", "base-ref", event="merge_group")
            self.assertEqual(proc.returncode, 0, proc.stdout)
            self.assertIn("::warning title=script-test inputs stale (advisory in a merge group)::", proc.stdout)
            self.assertIn("stale entry: alpha", proc.stdout)
            strict = self.run_tool(repo, "--check", "--base", "base-ref", event="merge_group", strict=True)
            self.assertEqual(strict.returncode, 1, strict.stdout)
            head = self.run_tool(repo, "--check", "--base", "base-ref", event="pull_request")
            self.assertEqual(head.returncode, 1, head.stdout)

    def test_unreadable_inventory_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            proc = subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(repo.root),
                                   "--inventory-json", str(repo.build / "absent.json"), "--check"],
                                  capture_output=True, text=True, timeout=60, env=tool_env(event="merge_group", strict=False))
            self.assertEqual(proc.returncode, 2)


def compiled_evidence(repo: Repo, *, declare_b: bool = False) -> None:
    """Configure-time evidence as tools/cmake/PulpTestData.cmake writes it.
    pulp-test-a declares its fixture; pulp-test-b reads the checkout through
    PULP_SOURCE_DIR, pulp-test-c through a checkout definition's name, and
    pulp-test-d reads nothing (no entry)."""
    write(repo.root, "test/fixtures/a/clip.wav", "RIFF\n")
    write(repo.root, "test/test_a.cpp", 'auto p = fs::path(PULP_SOURCE_DIR) / "test/fixtures/a/clip.wav";\n')
    write(repo.root, "test/test_b.cpp", "auto root = fs::path(PULP_SOURCE_DIR);\n")
    write(repo.root, "test/test_c.cpp", "auto dir = fs::path(PULP_CORPUS_DIR);\n")
    write(repo.root, "test/test_d.cpp", "int x = 1;\n")
    ev = repo.build / "test" / "test-data"
    ev.mkdir(parents=True, exist_ok=True)
    (ev / "executables.json").write_text(json.dumps({"schema": "pulp-test-executables/v1", "executables": {
        "pulp-test-a": {"sources": ["test/test_a.cpp"], "tree_defines": ["PULP_SOURCE_DIR"]},
        "pulp-test-b": {"sources": ["test/test_b.cpp"], "tree_defines": ["PULP_SOURCE_DIR"]},
        "pulp-test-c": {"sources": ["test/test_c.cpp"], "tree_defines": ["PULP_CORPUS_DIR"]},
        "pulp-test-d": {"sources": ["test/test_d.cpp"], "tree_defines": []}}}), encoding="utf-8")
    declared = {"pulp-test-a": (["test/test_a.cpp"], ["test/fixtures/a"])}
    if declare_b:
        declared["pulp-test-b"] = (["test/test_b.cpp"], ["test/fixtures"])
    for exe in ("pulp-test-a", "pulp-test-b"):
        (ev / f"{exe}.inputs.json").unlink(missing_ok=True)
    for exe, (srcs, paths) in declared.items():
        (ev / f"{exe}.inputs.json").write_text(json.dumps({
            "schema": "pulp-test-data-inputs/v1", "executable": exe, "kind": "compiled",
            "sources": srcs, "inputs": paths}), encoding="utf-8")


def spawn_evidence(repo: Repo) -> None:
    """Executables that start a process, as configure records them:
    edge (a runtime target), reviewed (pulp_test_spawns NONE), hidden (the
    call sits in a test/support header it includes, with no edge), member
    (a `.system(` member call, which is not a process API), and data-and-spawn
    (reads a declared fixture and spawns with no edge)."""
    write(repo.root, "test/test_edge.cpp", "auto r = pulp::platform::ChildProcess::run(tool, {});\n")
    write(repo.root, "test/test_reviewed.cpp", "FILE* f = popen(\"git status\", \"r\");\n")
    write(repo.root, "test/test_hidden.cpp", '#include "support/runner.hpp"\nint x = run_it();\n')
    write(repo.root, "test/support/runner.hpp", "inline int run_it() { return fork(); }\n")
    write(repo.root, "test/test_loader.cpp", "auto slot = PluginSlot::load(info);\n")
    write(repo.root, "test/test_exec.cpp", "auto r = pulp::platform::exec(tool, {\"--version\"}, 1000);\n")
    write(repo.root, "test/test_member_exec.cpp", "auto m = pattern.exec(text);\nauto n = db->exec(sql);\n")
    write(repo.root, "test/test_scanner.cpp", "auto found = scanner.scan_directory(dir, PluginFormat::CLAP);\n")
    write(repo.root, "test/test_fake_slot.cpp", "struct FakeSlot : PluginSlot { bool is_loaded() const; };\n")
    write(repo.root, "test/test_member.cpp",
          "physics.system(1);\nint my_system(int);\nauto m = ThemeMode::system();\n"
          "// the effect system (bloom) runs popen(cmd) in prose\n/* fork() */\n")
    write(repo.root, "test/test_both.cpp",
          'auto p = fs::path(PULP_SOURCE_DIR) / "test/fixtures/a";\nint rc = std::system(cmd);\n')
    ev = repo.build / "test" / "test-data"
    ev.mkdir(parents=True, exist_ok=True)
    row = lambda src, runtime=(), none=False, defines=(): {
        "sources": [src], "tree_defines": list(defines), "runtime_targets": list(runtime), "spawns_none": none,
        "spawns_none_reason": "runs the system git, nothing the tree builds" if none else None}
    (ev / "executables.json").write_text(json.dumps({"schema": "pulp-test-executables/v1", "executables": {
        "pulp-test-edge": row("test/test_edge.cpp", runtime=["pulp-cli"]),
        "pulp-test-reviewed": row("test/test_reviewed.cpp", none=True),
        "pulp-test-hidden": row("test/test_hidden.cpp"),
        "pulp-test-member": row("test/test_member.cpp"),
        "pulp-test-both": row("test/test_both.cpp", defines=["PULP_SOURCE_DIR"]),
        "pulp-test-loader": row("test/test_loader.cpp"),
        "pulp-test-exec": row("test/test_exec.cpp"),
        "pulp-test-member-exec": row("test/test_member_exec.cpp"),
        "pulp-test-scanner": row("test/test_scanner.cpp"),
        "pulp-test-fake-slot": row("test/test_fake_slot.cpp"),
        }}), encoding="utf-8")
    (ev / "runtime-targets.json").write_text(json.dumps({"schema": "pulp-runtime-targets/v1", "artifacts": {}}),
                                             encoding="utf-8")
    (ev / "pulp-test-both.inputs.json").write_text(json.dumps({
        "schema": "pulp-test-data-inputs/v1", "executable": "pulp-test-both", "kind": "compiled",
        "sources": ["test/test_both.cpp"], "inputs": ["test/fixtures/a"]}), encoding="utf-8")


def named_program_evidence(repo: Repo) -> None:
    """Spawners whose code names programs this tree builds. `pulp-cli` builds
    pulp-cpp; the broker runs the host; one plugin builds as AU and CLAP under
    one artifact name."""
    # A name inside a comment is history, not a program the test runs.
    write(repo.root, "test/test_direct.cpp",
          'ChildProcess::run(dir / "pulp-cpp.exe", {});\n// it once also ran "helper-tool"\n')
    write(repo.root, "test/test_missing.cpp", 'ChildProcess::run(dir / "helper-tool", {});\n')
    write(repo.root, "test/test_through.cpp", 'ChildProcess::run(broker, {"--host", dir + "/control-host"});\n')
    write(repo.root, "test/test_fake.cpp", 'auto fake = dir / "pulp-screenshot";\nauto r = popen(cmd, "r");\n')
    write(repo.root, "test/test_plugin.cpp", 'info.path = "PulpGain.clap";\nauto slot = PluginSlot::load(info);\n')
    ev = repo.build / "test" / "test-data"
    ev.mkdir(parents=True, exist_ok=True)
    row = lambda src, runtime=(), none=False, not_run=(): {
        "sources": [src], "tree_defines": [], "runtime_targets": list(runtime), "spawns_none": none,
        "named_not_run": list(not_run)}
    (ev / "executables.json").write_text(json.dumps({"schema": "pulp-test-executables/v1", "executables": {
        "direct": row("test/test_direct.cpp", runtime=["pulp-cli"]),
        "missing": row("test/test_missing.cpp", runtime=["pulp-cli"]),
        "through": row("test/test_through.cpp", runtime=["broker"]),
        "fake": row("test/test_fake.cpp", none=True, not_run=["pulp-screenshot"]),
        "plugin": row("test/test_plugin.cpp", runtime=["PulpGain_CLAP"]),
        # Declares a tool this configuration does not build, and has no other edge.
        "absent": dict(row("test/test_direct.cpp"), absent_spawns=["pulp-cli"])}}), encoding="utf-8")
    art = lambda name, runtime=(): {"artifact": name, "runtime_targets": list(runtime)}
    (ev / "runtime-targets.json").write_text(json.dumps({"schema": "pulp-runtime-targets/v1", "artifacts": {
        "pulp-cli": art("pulp-cpp"), "helper-tool": art("helper-tool"), "broker": art("broker", ["control-host"]),
        "control-host": art("control-host"), "pulp-screenshot": art("pulp-screenshot"),
        "PulpGain_AU": art("PulpGain"), "PulpGain_CLAP": art("PulpGain")}}), encoding="utf-8")


class NamedProgramTests(unittest.TestCase):
    def test_every_named_program_needs_an_edge(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); named_program_evidence(repo)
            ex = sti.compiled_entries(repo.root, repo.build)
            # "pulp-cpp.exe" is pulp-cli's artifact, and the test has that edge.
            self.assertEqual(ex["direct"]["spawns"], "declared")
            self.assertNotIn("unmatched_programs", ex["direct"])
            # An edge to something else does not cover a second program.
            self.assertEqual((ex["missing"]["spawns"], ex["missing"]["unmatched_programs"]),
                             ("undeclared", ["helper-tool"]))
            # The host is reached through the broker the test runs.
            self.assertEqual(ex["through"]["spawns"], "declared")
            # A reviewed "named, not run" answers the name; NONE stays NONE.
            self.assertEqual(ex["fake"]["spawns"], "none")
            # One plugin built in several formats is one name in the code.
            self.assertEqual(ex["plugin"]["spawns"], "declared")
            # A declared tool this configuration does not build is not an edge.
            self.assertEqual((ex["absent"]["spawns"], ex["absent"]["unmatched_programs"]), ("undeclared", ["pulp-cli"]))

    def test_without_the_runtime_index_every_spawner_is_undeclared(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); named_program_evidence(repo)
            (repo.build / "test" / "test-data" / "runtime-targets.json").unlink()
            ex = sti.compiled_entries(repo.root, repo.build)
            self.assertEqual({k: v["spawns"] for k, v in ex.items()},
                             dict.fromkeys(("direct", "missing", "through", "fake", "plugin", "absent"), "undeclared"))


class SpawnScanTests(unittest.TestCase):
    def test_each_spawn_state(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); spawn_evidence(repo)
            ex = sti.compiled_entries(repo.root, repo.build)
            self.assertEqual((ex["pulp-test-edge"]["spawns"], ex["pulp-test-edge"]["runtime_targets"]),
                             ("declared", ["pulp-cli"]))
            self.assertEqual(ex["pulp-test-reviewed"]["spawns"], "none")
            self.assertEqual(ex["pulp-test-hidden"]["spawns"], "undeclared")
            # The call is found in the header the source includes, not the source.
            self.assertEqual(ex["pulp-test-hidden"]["spawning_sources"], ["test/support/runner.hpp"])
            # Spawning alone makes an entry; its data side says it reads nothing.
            self.assertEqual((ex["pulp-test-edge"]["data"], ex["pulp-test-edge"]["inputs"]), ("none", []))
            self.assertNotIn("pulp-test-member", ex)
            # Loading a plugin is a runtime edge; implementing the interface is not.
            self.assertEqual(ex["pulp-test-loader"]["spawns"], "undeclared")
            # The process API's free exec() runs a program; a member exec() does not.
            self.assertEqual(ex["pulp-test-exec"]["spawns"], "undeclared")
            self.assertNotIn("pulp-test-member-exec", ex)
            # So is a scanner call that opens bundles from disk.
            self.assertEqual(ex["pulp-test-scanner"]["spawns"], "undeclared")
            self.assertNotIn("pulp-test-fake-slot", ex)
            self.assertEqual((ex["pulp-test-both"]["data"], ex["pulp-test-both"]["spawns"]), ("declared", "undeclared"))
            summary = sti.data_summary(repo.root, repo.build)
            self.assertEqual((summary["data_reading"], summary["declared"], summary["undeclared"]), (1, 1, 0))

    def test_every_process_starting_class_of_the_repo_api_is_a_signal(self) -> None:
        decl = re.compile(r"^\s*(?:static\s+|virtual\s+)*[\w:<>&*]+\s+(?:run|start\w*|launch)\s*\(", re.M)
        starters = set()
        for rel in ("core/platform/include/pulp/platform/child_process.hpp",
                    "core/events/include/pulp/events/child_process_manager.hpp"):
            parts = re.split(r"\n(?:class|struct)\s+(\w+)[^;{]*\{", (HERE.parents[1] / rel).read_text(encoding="utf-8"))
            starters |= {name for name, body in zip(parts[1::2], parts[2::2]) if decl.search(body)}
        self.assertGreaterEqual(len(starters), 3, starters)  # the parse found the API at all
        self.assertLessEqual(starters, set(sti.SPAWN_CLASSES))
        # Free functions that run a program and hand back its result.
        header = (HERE.parents[1] / "core/platform/include/pulp/platform/child_process.hpp").read_text(encoding="utf-8")
        runners = set(re.findall(r"^ProcessResult\s+(\w+)\s*\(", header, re.M))
        self.assertGreaterEqual(len(runners), 1, runners)
        self.assertLessEqual(runners, set(sti.SPAWN_FUNCTIONS))

    def test_every_listed_loader_still_exists_in_core_host(self) -> None:
        host = "\n".join((HERE.parents[1] / "core/host/include/pulp/host" / name).read_text(encoding="utf-8")
                         for name in ("plugin_slot.hpp", "scanner.hpp", "dl_shim.hpp", "node_pack.hpp",
                                      "signal_graph_runtime.hpp", "graph_serializer.hpp"))
        for api in sti.LOAD_APIS:
            name = re.sub(r"\\s\*\\\($", "", api).split("::")[-1]
            self.assertRegex(host, r"\b%s\s*\(" % re.escape(name), api)


class NoneReviewTests(unittest.TestCase):
    """A pulp_test_spawns(NONE) review states a claim, and one whose sources
    name a build path is confirmed by its owner in data."""

    def review(self, sources: dict[str, str], reason: str = "runs the system git, nothing the tree builds",
               reviews: dict[str, str] | None = None) -> list[tuple[str, str, set[str]]]:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            for rel, text in sources.items():
                write(repo.root, rel, text)
            if reviews is not None:
                write(repo.root, sti.NONE_BUILD_PATH_REVIEWS.as_posix(),
                      json.dumps({"schema": "pulp-spawn-none-build-path-reviews/v1", "reviews": reviews}))
            ev = repo.build / "test" / "test-data"
            ev.mkdir(parents=True)
            (ev / "executables.json").write_text(json.dumps({"schema": "pulp-test-executables/v1", "executables": {
                "pulp-test-r": {"sources": sorted(sources), "tree_defines": [], "runtime_targets": [],
                                "spawns_none": True, "spawns_none_reason": reason}}}), encoding="utf-8")
            return sti.none_review_problems(repo.root, repo.build)

    def test_a_plain_review_passes(self) -> None:
        # Control: a reviewed test whose sources name no build path.
        self.assertEqual(self.review({"test/r.cpp": 'auto out = run("git status");\n'}), [])

    def test_a_review_must_state_a_claim(self) -> None:
        for reason in ("legacy", "predates the rule", "TODO", " "):
            with self.subTest(reason=reason):
                kinds = [k for k, _, _ in self.review({"test/r.cpp": ""}, reason=reason)]
                self.assertEqual(kinds, ["NONE review states no claim"])

    def test_a_build_path_needs_the_owners_confirmation(self) -> None:
        for text in ('const char* b = std::getenv("PULP_BUILD_DIR");\n',
                     'const char* p = getenv("PATH");\n',
                     'for (auto& e : fs::directory_iterator("build/tools")) {}\n'):
            with self.subTest(text=text):
                problems = self.review({"test/r.cpp": text})
                self.assertEqual([k for k, _, _ in problems], ["NONE review names a build path"])
        # Not a build path: a home or source variable, a walk with no build literal.
        self.assertEqual(self.review({"test/r.cpp": 'getenv("HOME"); getenv("PULP_SOURCE_DIR");\n'
                                                      'fs::directory_iterator(tmp);\n'}), [])
        confirmed = self.review({"test/r.cpp": 'getenv("PULP_BUILD_DIR");\n'},
                                reviews={"pulp-test-r": "the test points PULP_BUILD_DIR at a temp dir"})
        self.assertEqual(confirmed, [])

    def test_a_review_for_a_test_no_longer_reviewed_is_stale(self) -> None:
        problems = self.review({"test/r.cpp": ""}, reviews={"pulp-test-gone": "x"})
        self.assertEqual([(k, n) for k, n, _ in problems], [("stale NONE build-path review", "pulp-test-gone")])


class LoaderCoverageTests(unittest.TestCase):
    """Every public core/host function that reaches a plugin or module loader
    is one LOAD_APIS can see called, or is exempt with its covering mechanism."""

    def host(self, files: dict[str, str]) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "core/host/include/pulp/host/api.hpp",
                  "void* dl_open(const char* path, int flags);\nint public_load(const char* p);\n"
                  "int wrap(const char* p);\n")
            for rel, text in files.items():
                write(root, rel, text)
            return sti.loader_coverage(root)

    def test_the_live_tree_is_covered(self) -> None:
        # Control: on this tree every loader is listed or exempt.
        self.assertEqual(sti.loader_coverage(HERE.parents[1]), [])

    def test_a_deleted_load_api_is_reported(self) -> None:
        live = [api for api in sti.LOAD_APIS if not api.startswith("load_node_pack")]
        self.assertEqual(len(live), len(sti.LOAD_APIS) - 1)
        with mock.patch.object(sti, "LOAD_APIS", tuple(live)):
            missing = sti.loader_coverage(HERE.parents[1])
        self.assertEqual([m.split(" ")[0] for m in missing], ["load_node_pack"])

    def test_a_public_loader_outside_load_apis_is_reported(self) -> None:
        missing = self.host({"core/host/src/a.cpp": "int public_load(const char* p) { return dlopen(p, 0) != 0; }\n"})
        self.assertEqual(missing, ["public_load (core/host/src/a.cpp)"])

    def test_a_public_wrapper_of_an_internal_loader_is_reported(self) -> None:
        missing = self.host({"core/host/src/a.cpp":
                             "static int internal_open(const char* p) { return dlopen(p, 0) != 0; }\n"
                             "int wrap(const char* p) { return internal_open(p); }\n"})
        self.assertEqual(missing, ["wrap (core/host/src/a.cpp)"])
        # The same wrapper, exempt with its covering mechanism, passes.
        covered = self.host({"core/host/src/a.cpp":
                             "static int internal_open(const char* p) { return dlopen(p, 0) != 0; }\n"
                             "int wrap(const char* p) { return internal_open(p); }\n",
                             sti.LOAD_API_EXEMPTIONS.as_posix(): json.dumps({"exemptions": {"wrap": "edge"}})})
        self.assertEqual(covered, [])


class CompiledDataTests(unittest.TestCase):
    def run_tool(self, repo: Repo, *args: str, event: str = "pull_request") -> subprocess.CompletedProcess[str]:
        inv = repo.build / "inv.json"
        inv.write_text(json.dumps(repo.inventory()), encoding="utf-8")
        return subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(repo.root), "--build-dir", str(repo.build),
                               "--inventory-json", str(inv), *args], capture_output=True, text=True, timeout=60,
                              env=tool_env(event=event, strict=False))

    def git_repo(self, repo: Repo) -> None:
        CheckModeTests.git_repo(self, repo)  # type: ignore[arg-type]

    def test_a_declared_fixture_shows_up_in_the_list(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            lst = json.loads((repo.root / sti.DEFAULT_LIST).read_text(encoding="utf-8"))
            self.assertEqual(lst["executables"]["pulp-test-a"], {
                "kind": "compiled", "data": "declared", "inputs": ["test/fixtures/a"],
                "sources": ["test/test_a.cpp"], "detected_sources": ["test/test_a.cpp"],
                "undeclared_sources": []})
            self.assertIn("alpha", lst["tests"])  # script entries unchanged, same file
            self.assertEqual(lst["executables_scanned_for"], ["data", "spawns"])
            # Every executable configure saw is listed, clean ones included
            # (pulp-test-d reads nothing and has no entry), so a missing entry
            # can be told apart from a never-scanned executable.
            self.assertEqual(lst["executables_scanned"], ["pulp-test-a", "pulp-test-b", "pulp-test-c", "pulp-test-d"])
            self.assertNotIn("pulp-test-d", lst["executables"])

    def test_detected_sources_lists_only_what_a_signal_matched(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            # pulp-test-d's source has no signal; declaring it must not make
            # the scan look as if it detected a reader there.
            (repo.build / "test" / "test-data" / "pulp-test-d.inputs.json").write_text(json.dumps({
                "schema": "pulp-test-data-inputs/v1", "executable": "pulp-test-d", "kind": "compiled",
                "sources": ["test/test_d.cpp"], "inputs": ["test/fixtures/a"]}), encoding="utf-8")
            ex = sti.compiled_entries(repo.root, repo.build)
            self.assertEqual(ex["pulp-test-d"]["data"], "declared")
            self.assertEqual(ex["pulp-test-d"]["sources"], ["test/test_d.cpp"])
            self.assertEqual(ex["pulp-test-d"]["detected_sources"], [])
            self.assertEqual(ex["pulp-test-a"]["detected_sources"], ["test/test_a.cpp"])
            self.assertEqual(ex["pulp-test-b"]["detected_sources"], ["test/test_b.cpp"])

    def test_a_generated_source_in_an_in_checkout_build_dir_records_the_token(self) -> None:
        # Two in-checkout build directories must write the same compiled entry
        # for a configure-generated source, or every other configuration
        # reports the entry stale.
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            write(repo.root, "test/test_gen.cpp", 'auto p = fs::path(PULP_SOURCE_DIR) / "test/fixtures/a";\n')
            seen = []
            for name in ("build-gate", "build-linux"):
                build = repo.root / name
                ev = build / "test" / "test-data"
                ev.mkdir(parents=True)
                gen = f"{name}/tools/cli/generated/index.cpp"
                (ev / "executables.json").write_text(json.dumps({"schema": "pulp-test-executables/v1", "executables": {
                    "pulp-gen": {"sources": ["test/test_gen.cpp", gen], "tree_defines": ["PULP_SOURCE_DIR"]}}}),
                    encoding="utf-8")
                (ev / "pulp-gen.inputs.json").write_text(json.dumps({
                    "schema": "pulp-test-data-inputs/v1", "executable": "pulp-gen", "kind": "compiled",
                    "whole_checkout": True, "sources": ["test/test_gen.cpp", gen], "inputs": []}), encoding="utf-8")
                seen.append(sti.compiled_entries(repo.root, build)["pulp-gen"])
            self.assertEqual(seen[0], seen[1])
            self.assertIn("${CMAKE_BINARY_DIR}/tools/cli/generated/index.cpp", seen[0]["sources"])
            self.assertFalse(any(s.startswith("build-") for s in seen[0]["sources"]))

    def test_reading_without_a_declaration_is_undeclared_and_a_quiet_source_gets_no_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            ex = sti.compiled_entries(repo.root, repo.build)
            self.assertEqual(ex["pulp-test-b"]["data"], "undeclared")
            self.assertEqual(ex["pulp-test-b"]["undeclared_sources"], ["test/test_b.cpp"])
            # A definition pointing into the checkout counts by its own name.
            self.assertEqual(ex["pulp-test-c"]["undeclared_sources"], ["test/test_c.cpp"])
            self.assertNotIn("pulp-test-d", ex)
            summary = sti.data_summary(repo.root, repo.build)
            self.assertEqual((summary["reading_pulp_source_dir"], summary["reading_pulp_source_dir_with_manifest"]), (2, 1))
            self.assertEqual((summary["declared"], summary["undeclared"]), (1, 2))

    def test_without_configure_evidence_the_list_has_no_compiled_section(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.assertNotIn("executables", sti.build_list(repo.inventory(), repo.root, repo.build))

    def test_guard_fails_on_a_new_undeclared_source_and_passes_the_backlog(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)  # base: b and c are the backlog
            backlog = self.run_tool(repo, "--check", "--base", "base-ref")
            self.assertEqual(backlog.returncode, 0, backlog.stdout)
            # A new test reading the checkout without a declaration, and the
            # author regenerating the list: the base list is the ratchet.
            write(repo.root, "test/test_e.cpp", "auto root = fs::path(PULP_SOURCE_DIR);\n")
            idx = repo.build / "test" / "test-data" / "executables.json"
            doc = json.loads(idx.read_text(encoding="utf-8"))
            doc["executables"]["pulp-test-e"] = {"sources": ["test/test_e.cpp"], "tree_defines": ["PULP_SOURCE_DIR"]}
            idx.write_text(json.dumps(doc), encoding="utf-8")
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.g("add", "-A"); self.g("commit", "-q", "-m", "pr adds an undeclared reader")
            proc = self.run_tool(repo, "--check", "--base", "base-ref")
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("new undeclared data source: pulp-test-e: test/test_e.cpp", proc.stdout)
            self.assertIn("pulp_test_data(<suite> PATHS", proc.stdout)
            self.assertNotIn("pulp-test-b: test/test_b.cpp", proc.stdout)
            group = self.run_tool(repo, "--check", "--base", "base-ref", event="merge_group")
            self.assertEqual(group.returncode, 0, group.stdout)

    def test_an_undeclared_reader_only_another_configuration_lists_is_reported_not_blocking(self) -> None:
        """A Linux-only executable is absent from a macOS-written base list; a
        change that leaves its source alone must not go red for it."""
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            write(repo.root, "test/test_linux.cpp", "auto root = fs::path(PULP_SOURCE_DIR);\n")
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)
            idx = repo.build / "test" / "test-data" / "executables.json"
            doc = json.loads(idx.read_text(encoding="utf-8"))
            doc["executables"]["pulp-test-linux"] = {"sources": ["test/test_linux.cpp"], "tree_defines": []}
            idx.write_text(json.dumps(doc), encoding="utf-8")
            write(repo.root, "test/cmake/x_tests.cmake", "# unrelated\n")
            self.g("add", "-A"); self.g("commit", "-q", "-m", "pr touches the test CMake only")
            proc = self.run_tool(repo, "--check", "--base", "base-ref")
            self.assertEqual(proc.returncode, 0, proc.stdout)
            self.assertIn("new undeclared data source: pulp-test-linux: test/test_linux.cpp", proc.stdout)
            self.assertEqual(self.run_tool(repo, "--check", "--full").returncode, 1)

    def test_dropping_a_declaration_is_caught_through_the_test_cmake(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo, declare_b=True)
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)
            compiled_evidence(repo)  # the pulp_test_data() call for b is gone
            write(repo.root, "test/cmake/b_tests.cmake", "# declaration removed\n")
            self.g("add", "-A"); self.g("commit", "-q", "-m", "pr drops a declaration")
            proc = self.run_tool(repo, "--check", "--base", "base-ref")
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("new undeclared data source: pulp-test-b: test/test_b.cpp", proc.stdout)

    def test_declaring_a_backlog_source_shrinks_it_and_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.git_repo(repo)
            compiled_evidence(repo, declare_b=True)
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            self.g("add", "-A"); self.g("commit", "-q", "-m", "declare b")
            proc = self.run_tool(repo, "--check", "--base", "base-ref")
            self.assertEqual(proc.returncode, 0, proc.stdout)
            lst = json.loads((repo.root / sti.DEFAULT_LIST).read_text(encoding="utf-8"))
            self.assertEqual(lst["executables"]["pulp-test-b"]["data"], "declared")

    def test_a_change_that_adds_a_spawn_owns_the_new_entry(self) -> None:
        """A spawns-only entry has no data `sources`; its spawn site still
        makes the drift the change's own."""
        for pr_adds_the_spawn in (True, False):
            with self.subTest(pr_adds_the_spawn=pr_adds_the_spawn), tempfile.TemporaryDirectory() as tmp:
                repo = Repo(Path(tmp)); compiled_evidence(repo)
                self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
                self.git_repo(repo)
                write(repo.root, "test/test_d.cpp", "int x = 1;\npid_t child = fork();\n")
                if pr_adds_the_spawn:
                    self.g("add", "-A"); self.g("commit", "-q", "-m", "pr adds a fork")
                else:  # main already moved; this change touches something else
                    self.g("add", "-A"); self.g("commit", "-q", "-m", "main adds a fork")
                    self.g("branch", "-f", "base-ref", "HEAD")
                    write(repo.root, "README.md", "unrelated\n"); self.g("add", "-A"); self.g("commit", "-q", "-m", "pr")
                proc = self.run_tool(repo, "--check", "--base", "base-ref")
                self.assertIn("missing compiled entry: pulp-test-d", proc.stdout)
                self.assertEqual(proc.returncode, 1 if pr_adds_the_spawn else 0, proc.stdout)

    def test_a_new_clean_executable_missing_from_the_scan_is_drift(self) -> None:
        """A clean executable has no entry; only executables_scanned records it."""
        for pr_adds_the_suite in (True, False):
            with self.subTest(pr_adds_the_suite=pr_adds_the_suite), tempfile.TemporaryDirectory() as tmp:
                repo = Repo(Path(tmp)); compiled_evidence(repo)
                self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
                self.git_repo(repo)
                write(repo.root, "test/test_e.cpp", "int y = 2;\n")
                index = repo.build / "test" / "test-data" / "executables.json"
                doc = json.loads(index.read_text(encoding="utf-8"))
                doc["executables"]["pulp-test-e"] = {"sources": ["test/test_e.cpp"], "tree_defines": []}
                index.write_text(json.dumps(doc), encoding="utf-8")
                if pr_adds_the_suite:
                    self.g("add", "-A"); self.g("commit", "-q", "-m", "pr adds a suite")
                else:  # main added it; this change touches something else
                    self.g("add", "-A"); self.g("commit", "-q", "-m", "main adds a suite")
                    self.g("branch", "-f", "base-ref", "HEAD")
                    write(repo.root, "README.md", "unrelated\n"); self.g("add", "-A"); self.g("commit", "-q", "-m", "pr")
                proc = self.run_tool(repo, "--check", "--base", "base-ref")
                self.assertIn("unscanned executable: pulp-test-e", proc.stdout)
                self.assertEqual(proc.returncode, 1 if pr_adds_the_suite else 0, proc.stdout)

    def test_a_stale_compiled_entry_is_drift(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            compiled_evidence(repo, declare_b=True)   # declared, list not regenerated
            proc = self.run_tool(repo, "--check", "--full")
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("stale compiled entry: pulp-test-b", proc.stdout)


class GateProfileTests(unittest.TestCase):
    run_tool = CompiledDataTests.run_tool

    def cache(self, repo: Repo, **values: str) -> None:
        lines = [f"{k}:STRING={v}" for k, v in values.items()]
        (repo.build / "CMakeCache.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def test_a_gate_shaped_build_compares_the_compiled_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            self.cache(repo, PULP_BUILD_EXAMPLES="OFF", CMAKE_BUILD_TYPE="Release", PULP_SANITIZER="",
                       PULP_HAS_VST3="TRUE", PULP_HAS_AUSDK="TRUE")
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            compiled_evidence(repo, declare_b=True)
            proc = self.run_tool(repo, "--check", "--full")
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("stale compiled entry: pulp-test-b", proc.stdout)

    def test_an_off_profile_build_skips_the_compiled_half_by_name(self) -> None:
        for values, reason in ((dict(PULP_BUILD_EXAMPLES="ON"), "PULP_BUILD_EXAMPLES=ON"),
                               (dict(PULP_SANITIZER="address", CMAKE_BUILD_TYPE="Debug"), "PULP_SANITIZER=address"),
                               (dict(CMAKE_BUILD_TYPE="Debug"), "CMAKE_BUILD_TYPE=Debug"),
                               (dict(PULP_HAS_VST3="FALSE"), "PULP_HAS_VST3=FALSE"),
                               (dict(PULP_HAS_AUSDK="FALSE"), "PULP_HAS_AUSDK=FALSE"),
                               (dict(PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF="ON"),
                                "PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON")):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as tmp:
                repo = Repo(Path(tmp)); compiled_evidence(repo)
                self.assertEqual(self.run_tool(repo, "--write").returncode, 0)   # no cache: the gate's
                compiled_evidence(repo, declare_b=True)                         # a compiled entry moves
                self.cache(repo, **values)
                proc = self.run_tool(repo, "--check", "--full")
                self.assertEqual(proc.returncode, sti.SKIP_EXIT, proc.stdout + proc.stderr)
                self.assertIn("SKIPPED the compiled entries", proc.stdout)
                self.assertIn(reason, proc.stdout)
                self.assertNotIn("stale compiled entry", proc.stdout)
                # Script entries still compare: a stale one fails as before.
                write(repo.root, "tools/scripts/test_alpha.py", "import alpha_lib\nimport newmod\n")
                write(repo.root, "tools/scripts/newmod.py", "")
                self.assertEqual(self.run_tool(repo, "--check", "--full").returncode, 1)
                # And the list is never written from such a build.
                proc = self.run_tool(repo, "--write")
                self.assertEqual(proc.returncode, 2)
                self.assertIn("refusing to write", proc.stderr)


class GatePlatformTests(unittest.TestCase):
    def cache(self, repo: Repo, CMAKE_SYSTEM_NAME: str) -> None:
        # Where CMake records it: not CMakeCache.txt, but CMakeSystem.cmake.
        write(repo.root / "build", "CMakeFiles/4.3.3/CMakeSystem.cmake",
              f'set(CMAKE_SYSTEM_NAME "{CMAKE_SYSTEM_NAME}")\n')

    def run_tool(self, repo: Repo, *args: str) -> subprocess.CompletedProcess:
        inv = repo.root / "build" / "inv.json"
        inv.parent.mkdir(parents=True, exist_ok=True)
        inv.write_text(json.dumps(repo.inventory()), encoding="utf-8")
        return subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(repo.root),
                               "--build-dir", str(repo.root / "build"), "--inventory-json", str(inv), *args],
                              capture_output=True, text=True, timeout=60, env=tool_env(event=None, strict=False))

    def test_another_platform_skips_by_name_and_never_writes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            self.cache(repo, CMAKE_SYSTEM_NAME="Darwin")
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            # A Linux configure registers tests the macOS-written list cannot have.
            self.cache(repo, CMAKE_SYSTEM_NAME="Linux")
            write(repo.root, "tools/scripts/newmod.py", "")
            write(repo.root, "tools/scripts/test_alpha.py", "import alpha_lib\nimport newmod\n")
            proc = self.run_tool(repo, "--check", "--full")
            self.assertEqual(proc.returncode, sti.SKIP_EXIT, proc.stdout + proc.stderr)
            self.assertIn("SKIPPED: CMAKE_SYSTEM_NAME=Linux", proc.stdout)
            self.assertIn("this is a skip, not a pass", proc.stdout)
            proc = self.run_tool(repo, "--write")
            self.assertEqual(proc.returncode, 2)
            self.assertIn("refusing to write: CMAKE_SYSTEM_NAME=Linux", proc.stderr)
            # Control: the same drift on the gate's platform still fails.
            self.cache(repo, CMAKE_SYSTEM_NAME="Darwin")
            self.assertEqual(self.run_tool(repo, "--check", "--full").returncode, 1)


class ScanScopeTests(unittest.TestCase):
    def test_walking_up_to_the_checkout_is_a_read(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "test/two.cpp", 'auto r = fs::current_path() / ".." / "..";\n')
            write(root, "test/joined.cpp", 'auto r = std::filesystem::current_path() / "../..";\n')
            write(root, "test/file.cpp", "auto r = fs::path(__FILE__).parent_path().parent_path();\n")
            write(root, "test/build.cpp", 'auto b = fs::current_path() / ".." / "tools";\n')
            write(root, "test/log.cpp", 'log(__FILE__, __LINE__);\n')
            self.assertEqual(sti.source_signals(root, "test/two.cpp", []), [sti.WALK_UP_SIGNAL])
            self.assertEqual(sti.source_signals(root, "test/joined.cpp", []), [sti.WALK_UP_SIGNAL])
            self.assertEqual(sti.source_signals(root, "test/file.cpp", []), [sti.SOURCE_FILE_SIGNAL])
            self.assertEqual(sti.source_signals(root, "test/build.cpp", []), [])  # the build tree
            self.assertEqual(sti.source_signals(root, "test/log.cpp", []), [])

    def scope_build(self, tmp: Path) -> tuple[Path, Path]:
        root, build = tmp / "repo", tmp / "build"
        write(root, "test/t.cpp", "auto p = fs::path(PULP_SOURCE_DIR);\n")
        write(root, "tools/cli/run.cpp", "auto p = fs::path(PULP_SOURCE_DIR);\n")
        write(root, "tools/cli/disc.cpp", "auto p = fs::path(PULP_SOURCE_DIR);\n")
        write(root, "tools/cli/arg.cpp", "auto p = fs::path(PULP_SOURCE_DIR);\n")
        write(root, "tools/cli/idle.cpp", "auto p = fs::path(PULP_SOURCE_DIR);\n")
        ev = build / "test" / "test-data"
        ev.mkdir(parents=True)
        rec = lambda src, under: {"sources": [src], "tree_defines": ["PULP_SOURCE_DIR"], "under_test": under}
        (ev / "executables.json").write_text(json.dumps({"schema": "pulp-test-executables/v1", "executables": {
            "in-test": rec("test/t.cpp", True), "cli-target": rec("tools/cli/run.cpp", False),
            "discovered": rec("tools/cli/disc.cpp", False), "arg-only": rec("tools/cli/arg.cpp", False),
            "idle": rec("tools/cli/idle.cpp", False)}}), encoding="utf-8")
        (ev / "runtime-targets.json").write_text(json.dumps({"schema": "pulp-runtime-targets/v1", "artifacts": {
            "cli-target": {"artifact": "cli-run", "runtime_targets": []}}}), encoding="utf-8")
        b = str(build)
        write(build, "CTestTestfile.cmake",
              f'add_test([=[direct]=] "{b}/tools/cli/cli-run" "doctor")\n'
              f'add_test([=[script]=] "/usr/bin/python3" "{root}/check.py" "--binary" "{b}/tools/cli/arg-only")\n'
              f'include("{b}/tools/cli/discovered-1a2b3c4_include.cmake")\n')
        return root, build

    def test_an_executable_ctest_runs_outside_test_is_scanned_by_its_program_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, build = self.scope_build(Path(tmp))
            ex = sti.compiled_entries(root, build)
            # Run by ctest under its artifact name; found through a discovery
            # include; an argument to a script is not run by ctest; unused.
            self.assertEqual(sorted(ex), ["cli-run", "discovered", "in-test"])
            self.assertEqual(ex["cli-run"]["data"], "undeclared")
            self.assertEqual(sorted(sti.test_executables(build)), ["cli-run", "discovered", "in-test"])
            # Without a CTestTestfile nothing can be ruled out, so all are scanned.
            (build / "CTestTestfile.cmake").unlink()
            self.assertEqual(sorted(sti.compiled_entries(root, build)),
                             ["arg-only", "cli-run", "discovered", "idle", "in-test"])

    def test_every_key_is_the_artifact_and_renamed_ones_map_to_their_target(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, build = self.scope_build(Path(tmp))
            artifacts = build / "test" / "test-data" / "runtime-targets.json"
            doc = json.loads(artifacts.read_text(encoding="utf-8"))
            doc["artifacts"]["in-test"] = {"artifact": "in-test-bin", "runtime_targets": []}
            artifacts.write_text(json.dumps(doc), encoding="utf-8")
            lst = sti.build_list({"tests": []}, root, build)
            self.assertEqual(lst["executables_scanned"], ["cli-run", "discovered", "in-test-bin"])
            self.assertEqual(lst["executable_targets"], {"cli-run": "cli-target", "in-test-bin": "in-test"})
            self.assertIn("in-test-bin", lst["executables"])
            # A discovery include carries the target name, not the artifact.
            doc["artifacts"]["discovered"] = {"artifact": "disc-bin", "runtime_targets": []}
            artifacts.write_text(json.dumps(doc), encoding="utf-8")
            self.assertIn("disc-bin", sti.test_executables(build))
            # Two targets that build one program cannot be told apart by a selector.
            doc["artifacts"]["discovered"] = {"artifact": "in-test-bin", "runtime_targets": []}
            artifacts.write_text(json.dumps(doc), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "both build in-test-bin"):
                sti.test_executables(build)

    def test_a_declaration_on_a_renamed_program_is_read_by_its_target(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, build = self.scope_build(Path(tmp))
            (build / "test" / "test-data" / "cli-target.inputs.json").write_text(json.dumps({
                "schema": "pulp-test-data-inputs/v1", "executable": "cli-target", "kind": "compiled",
                "sources": ["tools/cli/run.cpp"], "inputs": [], "whole_checkout": True}), encoding="utf-8")
            entry = sti.compiled_entries(root, build)["cli-run"]
            self.assertEqual((entry["data"], entry["undeclared_sources"]), ("whole_checkout", []))
            self.assertEqual(entry["detected_sources"], ["tools/cli/run.cpp"])


if __name__ == "__main__":
    unittest.main()
