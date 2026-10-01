#!/usr/bin/env python3
"""Tests for header_fanout_guard.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

THIS_DIR = Path(__file__).parent
SCRIPT = THIS_DIR / "header_fanout_guard.py"
LEDGER = THIS_DIR / "header_fanout_guard.json"


def load_module():
    spec = importlib.util.spec_from_file_location("header_fanout_guard", str(SCRIPT))
    module = importlib.util.module_from_spec(spec)
    sys.modules["header_fanout_guard"] = module
    spec.loader.exec_module(module)
    return module


hfg = load_module()

HEAVY = "core/a/include/pulp/a/heavy.hpp"
UMBRELLA = "core/a/include/pulp/a/umbrella.hpp"


def git(cwd: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(cwd), *args], check=True, capture_output=True, text=True
    ).stdout


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class FixtureRepo:
    """A repo where 30 test TUs include an umbrella header and one includes a heavy one."""

    def __init__(self, root: Path, max_tus: int = 5) -> None:
        self.root = root
        git(root, "init", "-q", "-b", "main")
        git(root, "config", "user.email", "t@example.com")
        git(root, "config", "user.name", "t")
        write(root, HEAVY, "#pragma once\nstruct Heavy {};\n")
        write(root, UMBRELLA, "#pragma once\n#include <vector>\nstruct Umbrella {};\n")
        for i in range(30):
            write(root, f"test/test_u{i}.cpp", '#include <pulp/a/umbrella.hpp>\n#include "support/h.hpp"\n')
        write(root, "test/support/h.hpp", "#pragma once\n")
        write(root, "core/a/src/heavy.cpp", '#include "pulp/a/heavy.hpp"\n')
        write(root, "tools/scripts/header_fanout_guard.json", json.dumps({
            "schema_version": 1,
            "headers": [{"path": HEAVY, "max_tus": max_tus}],
        }))
        self.commit("base")

    def commit(self, message: str) -> None:
        git(self.root, "add", ".")
        git(self.root, "commit", "-q", "-m", message)

    def run_guard(self, *extra: str) -> tuple[int, str]:
        err = io.StringIO()
        cwd = os.getcwd()
        os.chdir(self.root)
        try:
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                rc = hfg.main(["--base", "main", "--head", "HEAD", *extra])
        finally:
            os.chdir(cwd)
        return rc, err.getvalue()


class ConfigTests(unittest.TestCase):
    def test_rejects_duplicate_paths(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate header path"):
            hfg.parse_config({"schema_version": 1, "headers": [
                {"path": "a.hpp", "max_tus": 1}, {"path": "a.hpp", "max_tus": 2}]})

    def test_rejects_non_header_and_bad_ceiling(self) -> None:
        with self.assertRaisesRegex(ValueError, "not a header"):
            hfg.parse_config({"schema_version": 1, "headers": [{"path": "a.cpp", "max_tus": 1}]})
        with self.assertRaisesRegex(ValueError, "positive integer"):
            hfg.parse_config({"schema_version": 1, "headers": [{"path": "a.hpp", "max_tus": 0}]})
        with self.assertRaisesRegex(ValueError, "positive integer"):
            hfg.parse_config({"schema_version": 1, "headers": [{"path": "a.hpp", "max_tus": True}]})

    def test_suggested_ceiling_has_headroom(self) -> None:
        self.assertEqual(hfg.suggested_ceiling(5), 15)
        self.assertEqual(hfg.suggested_ceiling(500), 550)


class IncludeGraphTests(unittest.TestCase):
    def test_resolves_quoted_relative_include_roots_and_ignores_system_headers(self) -> None:
        files = ["core/x/include/pulp/x/a.hpp", "core/x/src/b.cpp", "core/x/src/local.hpp",
                 "test/support/s.hpp", "test/t.cpp"]
        graph = hfg.IncludeGraph(files, {
            "core/x/src/b.cpp": ["local.hpp", "pulp/x/a.hpp", "vector"],
            "test/t.cpp": ["support/s.hpp", "s.hpp"],
        })
        self.assertEqual(graph.edges["core/x/src/b.cpp"], {"core/x/src/local.hpp", "core/x/include/pulp/x/a.hpp"})
        self.assertEqual(graph.edges["test/t.cpp"], {"test/support/s.hpp"})
        self.assertEqual(graph.translation_units("core/x/include/pulp/x/a.hpp"), {"core/x/src/b.cpp"})

    def test_reach_is_transitive_through_headers(self) -> None:
        files = ["h1.hpp", "h2.hpp", "a.cpp", "b.mm"]
        graph = hfg.IncludeGraph(files, {"h2.hpp": ["h1.hpp"], "a.cpp": ["h2.hpp"], "b.mm": ["h1.hpp"]})
        self.assertEqual(graph.translation_units("h1.hpp"), {"a.cpp", "b.mm"})


class GuardEndToEndTests(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self.addCleanup(self._td.cleanup)
        self.repo = FixtureRepo(Path(self._td.name))
        git(self.repo.root, "checkout", "-q", "-b", "feature")

    def test_clean_range_passes(self) -> None:
        rc, err = self.repo.run_guard()
        self.assertEqual(rc, 0, err)
        self.assertIn("within their fan-out ceilings", err)

    def test_umbrella_include_fails_and_names_the_edge(self) -> None:
        write(self.repo.root, UMBRELLA, "#pragma once\n#include <pulp/a/heavy.hpp>\nstruct Umbrella {};\n")
        self.repo.commit("umbrella include")
        rc, err = self.repo.run_guard()
        self.assertEqual(rc, 1, err)
        self.assertIn(f"{HEAVY}: fan-out grew 1 -> 31 translation units", err)
        self.assertIn(f"new include {UMBRELLA} -> {HEAVY} reaches 30 of them", err)
        self.assertIn("Fanout-Grow:", err)

    def test_edge_the_reach_does_not_depend_on_is_not_blamed(self) -> None:
        write(self.repo.root, UMBRELLA, "#pragma once\n#include <pulp/a/heavy.hpp>\n")
        write(self.repo.root, "test/test_u0.cpp",
              '#include <pulp/a/umbrella.hpp>\n#include <pulp/a/heavy.hpp>\n#include "support/h.hpp"\n')
        self.repo.commit("umbrella include plus a redundant direct include")
        rc, err = self.repo.run_guard()
        self.assertEqual(rc, 1, err)
        self.assertIn(f"new include {UMBRELLA} -> {HEAVY}", err)
        self.assertNotIn("new include test/test_u0.cpp", err)

    def test_hint_mode_reports_but_passes(self) -> None:
        write(self.repo.root, UMBRELLA, "#pragma once\n#include <pulp/a/heavy.hpp>\n")
        self.repo.commit("umbrella include")
        rc, err = self.repo.run_guard("--mode", "hint")
        self.assertEqual(rc, 0)
        self.assertIn("grew past its ceiling", err)

    def test_trailer_authorizes_growth(self) -> None:
        write(self.repo.root, UMBRELLA, "#pragma once\n#include <pulp/a/heavy.hpp>\n")
        git(self.repo.root, "add", ".")
        git(self.repo.root, "commit", "-q", "-m", f"umbrella\n\nFanout-Grow: {HEAVY} reason=\"intended\"")
        rc, err = self.repo.run_guard()
        self.assertEqual(rc, 0, err)
        self.assertIn("authorized by Fanout-Grow", err)

    def test_growth_within_ceiling_passes(self) -> None:
        write(self.repo.root, "test/test_direct.cpp", "#include <pulp/a/heavy.hpp>\n")
        self.repo.commit("one more direct user")
        rc, err = self.repo.run_guard()
        self.assertEqual(rc, 0, err)

    def test_already_over_ceiling_but_not_grown_passes(self) -> None:
        git(self.repo.root, "checkout", "-q", "main")
        write(self.repo.root, UMBRELLA, "#pragma once\n#include <pulp/a/heavy.hpp>\n")
        self.repo.commit("main drifted over")
        git(self.repo.root, "checkout", "-q", "-b", "later")
        write(self.repo.root, "README.md", "x\n")
        self.repo.commit("unrelated")
        rc, err = self.repo.run_guard()
        self.assertEqual(rc, 0, err)
        self.assertIn("did not grow it", err)

    def test_missing_tracked_header_fails(self) -> None:
        git(self.repo.root, "rm", "-q", HEAVY)
        self.repo.commit("remove heavy")
        rc, err = self.repo.run_guard()
        self.assertEqual(rc, 1, err)
        self.assertIn("no longer exists", err)


class RealLedgerTests(unittest.TestCase):
    """The committed ledger against this checkout: the instrument must see the tree."""

    def test_ledger_parses_and_every_tracked_header_is_reached(self) -> None:
        headers = hfg.parse_config(json.loads(LEDGER.read_text(encoding="utf-8")))
        self.assertGreaterEqual(len(headers), 20)
        root = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=THIS_DIR,
                                   capture_output=True, text=True, check=True).stdout.strip())
        graph = hfg.graph_at_ref("HEAD", cwd=root)
        # Control: a header nearly every view TU includes must read wide. A zero
        # here means the graph failed to resolve includes, not a narrow header.
        self.assertGreater(len(graph.translation_units("core/view/include/pulp/view/view.hpp")), 300)
        for tracked in headers:
            self.assertIn(tracked.path, graph.files, tracked.path)
            self.assertGreater(len(graph.translation_units(tracked.path)), 0, tracked.path)


if __name__ == "__main__":
    unittest.main()
