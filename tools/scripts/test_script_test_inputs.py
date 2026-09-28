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
- an unreadable inventory exits 2.

Run:
    python3 tools/scripts/test_script_test_inputs.py
"""
from __future__ import annotations

import json
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


class CheckModeTests(unittest.TestCase):
    def run_tool(self, repo: Repo, *args: str, inventory: dict | None = None) -> subprocess.CompletedProcess[str]:
        inv = repo.build / "inv.json"
        inv.write_text(json.dumps(inventory or repo.inventory()), encoding="utf-8")
        return subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(repo.root),
                               "--inventory-json", str(inv), *args], capture_output=True, text=True, timeout=60)

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

    def test_unreadable_inventory_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            proc = subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(repo.root),
                                   "--inventory-json", str(repo.build / "absent.json"), "--check"],
                                  capture_output=True, text=True, timeout=60)
            self.assertEqual(proc.returncode, 2)


if __name__ == "__main__":
    unittest.main()
