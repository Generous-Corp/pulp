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
  and `--check` fails on a NEW undeclared source against the base list.

Run:
    python3 tools/scripts/test_script_test_inputs.py
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
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
            {"name": "beta", "command": ["/bin/bash", f"{r}/tools/scripts/test_beta.sh"], "properties": []},
            {"name": "mod", "command": ["/usr/bin/python3", "-m", "test_mod"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": f"{r}/tools/scripts"}]},
            {"name": "cmake-nested", "command": ["/opt/homebrew/bin/cmake", "-P", f"{r}/test/x.cmake"], "properties": []},
            {"name": "no-command", "command": [], "properties": []},
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
                "sources": ["test/test_a.cpp"], "undeclared_sources": []})
            self.assertIn("alpha", lst["tests"])  # script entries unchanged, same file

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

    def test_a_stale_compiled_entry_is_drift(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)); compiled_evidence(repo)
            self.assertEqual(self.run_tool(repo, "--write").returncode, 0)
            compiled_evidence(repo, declare_b=True)   # declared, list not regenerated
            proc = self.run_tool(repo, "--check", "--full")
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("stale compiled entry: pulp-test-b", proc.stdout)


if __name__ == "__main__":
    unittest.main()
