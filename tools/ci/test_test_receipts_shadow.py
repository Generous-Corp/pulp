#!/usr/bin/env python3
"""Tests for tools/ci/test_receipts_shadow.py (per-test receipts, shadow mode).

What must hold:
- a test's key changes when anything its result can depend on changes: its
  executable's bytes, a file its command names, its properties or
  environment, the toolchain, a CI policy file; for a compiled test also the
  runtime surface (fixtures and other non-compiled tracked files) and the
  build's non-test executables; for a script test a declared input (a file
  under a declared directory included);
- the key does NOT change for an unrelated change: a compiled source the
  binary did not change for, a doc, a script test's undeclared neighbour, or
  the checkout and build living at another path;
- tests without a bounded input set have no key (undeclared scripts, nested
  cmake builds, a command naming the source root or a build directory);
- always-run: drift/lint/guard/probe names, pr-fast/GPU/host labels,
  top-level-directory declarers, recent failures, unkeyed tests;
- the verdict counts would-skip tests and the SAFETY number: would-skip
  tests that failed in the same run; a CMake change or a control run id
  would force a full run;
- receipts carry only passing keyed tests; reading accepts only this
  repository's build.yml merge_group runs with a matching receipt, within a
  time budget, never the current run.

Run:
    python3 tools/ci/test_test_receipts_shadow.py
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import urllib.error
import zipfile
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_receipts_shadow as trs  # noqa: E402
from test_protected_merge_receipt import BlobRedirectServer  # noqa: E402

GIT_ENV = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
           "GIT_COMMITTER_EMAIL": "t@t", "PATH": "/usr/bin:/bin:/opt/homebrew/bin"}


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                          check=True, env=GIT_ENV).stdout


def write(path: Path, text: str, exe: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if exe:
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class World:
    """A source checkout, a build directory, and a ctest inventory."""

    def __init__(self, root: Path) -> None:
        self.src = root / "src"
        self.build = root / "build"
        self.src.mkdir()
        git(self.src, "init", "-q")
        for rel, text in {
            "CMakeLists.txt": "project(p)\n",
            "core/a.cpp": "int a;\n",
            "core/b.cpp": "int b;\n",
            "test/fixtures/data.json": "{}\n",
            "docs/guide.md": "doc\n",
            "tools/scripts/check.py": "import helper\n",
            "tools/scripts/helper.py": "X = 1\n",
            "tools/scripts/other.py": "Y = 1\n",
            "tools/data/list.txt": "a\n",
            ".github/workflows/build.yml": "on: push\n",
        }.items():
            write(self.src / rel, text)
        self.commit("base")
        write(self.build / "test" / "pulp-test-a", "binary-a-v1", exe=True)
        write(self.build / "test" / "pulp-test-b", "binary-b-v1", exe=True)
        write(self.build / "pulp", "cli-v1", exe=True)
        write(self.build / "CMakeFiles" / "ignored", "x", exe=True)
        self.python = shutil.which("python3") or sys.executable
        self.script_inputs = {"check-selftest": ["tools/scripts/check.py", "tools/scripts/helper.py",
                                                 "tools/data"]}

    def commit(self, msg: str) -> None:
        git(self.src, "add", "-A")
        git(self.src, "commit", "-q", "-m", msg)

    def change(self, rel: str, text: str) -> None:
        write(self.src / rel, text)
        self.commit(f"touch {rel}")

    def inventory(self, env: str = "A=1") -> list[dict]:
        b, s = str(self.build), str(self.src)
        return [
            {"name": "A case one", "command": [f"{b}/test/pulp-test-a", "A case one"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": b},
                            {"name": "ENVIRONMENT", "value": [env]}]},
            {"name": "B case", "command": [f"{b}/test/pulp-test-b", "B case"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": b}]},
            {"name": "check-selftest", "command": [self.python, f"{s}/tools/scripts/check.py"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": s}]},
            {"name": "undeclared-selftest", "command": [self.python, f"{s}/tools/scripts/other.py"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": s}]},
            {"name": "nested-configure", "command": ["cmake", "-P", f"{s}/CMakeLists.txt"],
             "properties": []},
            {"name": "reads-the-tree", "command": [f"{b}/test/pulp-test-b", s], "properties": []},
        ]

    def keys(self, toolchain: str = "tc1", tests: list[dict] | None = None) -> dict:
        tests = tests if tests is not None else self.inventory()
        return trs.compute_keys(tests, self.build, self.src, self.script_inputs, trs.FileHasher(),
                                toolchain, trs.tracked_blobs(self.src))


class KeyTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.w = World(Path(tmp.name))
        self.base = self.w.keys()

    def assertKeyChanged(self, before: dict, after: dict, name: str, label: str) -> None:
        self.assertIsNotNone(after[name][0], label)
        self.assertNotEqual(before[name][0], after[name][0], label)

    def test_unkeyable_tests_have_no_key(self) -> None:
        self.assertEqual(self.base["undeclared-selftest"], (None, "undeclared-script"))
        self.assertEqual(self.base["nested-configure"], (None, "nested-build"))
        self.assertEqual(self.base["reads-the-tree"], (None, "names-the-source-root"))
        self.assertEqual(self.base["A case one"][1], "binary")
        self.assertEqual(self.base["check-selftest"][1], "script")

    def test_each_catch2_case_of_one_binary_has_its_own_key(self) -> None:
        tests = self.w.inventory()
        tests.append(dict(tests[0], name="A case two", command=[tests[0]["command"][0], "A case two"]))
        keys = self.w.keys(tests=tests)
        self.assertNotEqual(keys["A case one"][0], keys["A case two"][0])

    def test_binary_bytes_change_only_that_binarys_keys(self) -> None:
        write(self.w.build / "test" / "pulp-test-a", "binary-a-v2", exe=True)
        after = self.w.keys()
        self.assertKeyChanged(self.base, after, "A case one", "binary bytes")
        self.assertEqual(self.base["B case"], after["B case"])
        self.assertEqual(self.base["check-selftest"], after["check-selftest"])

    def test_compiled_source_and_docs_changes_leave_keys_unchanged(self) -> None:
        # The binary did not change, so a compiled source edit that did not
        # alter its bytes (and a doc) cannot change its result.
        self.w.change("core/a.cpp", "int a; // comment\n")
        self.w.change("docs/guide.md", "new doc\n")
        self.assertEqual(self.w.keys(), self.base)

    def test_runtime_surface_changes_compiled_keys_not_declared_script_keys(self) -> None:
        self.w.change("test/fixtures/data.json", '{"x": 1}\n')
        after = self.w.keys()
        self.assertKeyChanged(self.base, after, "A case one", "fixture")
        self.assertKeyChanged(self.base, after, "B case", "fixture")
        self.assertEqual(self.base["check-selftest"], after["check-selftest"])

    def test_product_executables_change_compiled_keys(self) -> None:
        write(self.w.build / "pulp", "cli-v2", exe=True)
        after = self.w.keys()
        self.assertKeyChanged(self.base, after, "A case one", "cli binary")
        self.assertEqual(self.base["check-selftest"], after["check-selftest"])
        write(self.w.build / "pulp", "cli-v1", exe=True)
        write(self.w.build / "CMakeFiles" / "ignored", "y", exe=True)
        self.assertEqual(self.w.keys(), self.base)

    def test_declared_inputs_change_script_keys_and_nothing_else_does(self) -> None:
        self.w.change("tools/scripts/other.py", "Y = 2\n")  # an undeclared neighbour
        self.assertEqual(self.w.keys()["check-selftest"], self.base["check-selftest"])
        for rel in ("tools/scripts/helper.py", "tools/data/list.txt", "tools/data/new.txt"):
            before = self.w.keys()
            self.w.change(rel, f"changed {rel}\n")
            self.assertKeyChanged(before, self.w.keys(), "check-selftest", rel)

    def test_policy_toolchain_and_properties_are_part_of_every_key(self) -> None:
        self.w.change(".github/workflows/build.yml", "on: merge_group\n")
        policy = self.w.keys()
        for name in ("A case one", "check-selftest"):
            self.assertKeyChanged(self.base, policy, name, "policy")
        tc = self.w.keys(toolchain="tc2")
        for name in ("A case one", "check-selftest"):
            self.assertKeyChanged(policy, tc, name, "toolchain")
        env = self.w.keys(tests=self.w.inventory(env="A=2"))
        self.assertKeyChanged(policy, env, "A case one", "environment")
        self.assertEqual(policy["B case"], env["B case"])

    def test_the_same_content_at_other_paths_keys_identically(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            other = Path(tmp)
            shutil.copytree(self.w.build, other / "b2", symlinks=True)
            git(other, "clone", "-q", str(self.w.src), str(other / "s2"))
            w2 = World.__new__(World)
            w2.src, w2.build, w2.python = other / "s2", other / "b2", self.w.python
            w2.script_inputs = self.w.script_inputs
            self.assertEqual(w2.keys(), self.base)

    def test_a_command_naming_a_build_directory_is_unkeyed(self) -> None:
        tests = self.w.inventory()
        tests[1] = dict(tests[1], command=tests[1]["command"] + [str(self.w.build / "test")])
        self.assertEqual(self.w.keys(tests=tests)["B case"], (None, "names-a-build-directory"))

    def test_a_build_directory_inside_the_checkout_is_still_a_build_directory(self) -> None:
        inner = self.w.src / "build-ci"
        shutil.copytree(self.w.build, inner, symlinks=True)
        (self.w.src / ".gitignore").write_text("build-ci/\n")
        w2 = World.__new__(World)
        w2.src, w2.build, w2.python, w2.script_inputs = self.w.src, inner, self.w.python, self.w.script_inputs
        tests = w2.inventory()
        tests[1] = dict(tests[1], command=tests[1]["command"] + [str(inner / "test")])
        self.assertEqual(w2.keys(tests=tests)["B case"], (None, "names-a-build-directory"))
        self.assertEqual(w2.keys()["A case one"][1], "binary")


def t(name: str, labels: list[str] | None = None) -> dict:
    props = [{"name": "LABELS", "value": labels}] if labels else []
    return {"name": name, "command": ["/b/x"], "properties": props}


class AlwaysRunAndVerdictTests(unittest.TestCase):
    def test_always_run_rules(self) -> None:
        cases = {
            "script-test-inputs-drift": None, "consumption-census-schema": None,
            "pulp-gpu-host-mapped-pointer-probe": None, "tools-registry-check": None,
            "build-parallelism-guard": None, "skills-doc-sync": None,
        }
        for name in cases:
            self.assertEqual(trs.always_run_reason(t(name), {}), "tree-reader-or-probe", name)
        for label in ("pr-fast", "gpu", "host", "lint"):
            self.assertEqual(trs.always_run_reason(t("plain", [label]), {}), "label", label)
        self.assertEqual(trs.always_run_reason(t("plain"), {"plain": ["tools/x.py", "core"]}),
                         "declares-a-top-level-directory")
        self.assertIsNone(trs.always_run_reason(t("plain", ["dsp"]), {"plain": ["tools/x.py"]}))
        self.assertIsNone(trs.always_run_reason(t("plain"), {"plain": ["CMakeLists.txt"]}))

    def evaluate(self, results, prior, cmake=False, run_id=123, keys=None, tests=None):
        tests = tests or [t("a"), t("b"), t("c"), t("lint-check"), t("u")]
        keys = keys or {"a": ("ka", "binary"), "b": ("kb", "binary"), "c": ("kc", "script"),
                        "lint-check": ("kl", "script"), "u": (None, "undeclared-script")}
        return trs.evaluate(tests, keys, prior, results, {}, cmake, run_id)

    def test_would_skip_counts_hits_and_names_would_skip_failures(self) -> None:
        prior = [{"run_id": "9", "passed": {"ka": {}, "kb": {}, "kl": {}}, "failed": []}]
        results = {"a": ("pass", 2.0), "b": ("fail", 3.0), "c": ("pass", 1.0),
                   "lint-check": ("pass", 1.0), "u": ("pass", 1.0)}
        v = self.evaluate(results, prior)
        self.assertEqual(v["would_skip"], 2)  # a, b; lint-check is always-run, c has no receipt
        self.assertEqual(v["would_skip_seconds"], 5.0)
        self.assertEqual((v["would_skip_failed"], v["would_skip_failed_names"]), (1, ["b"]))
        self.assertEqual(v["ran_reasons"], {"no-receipt": 1, "tree-reader-or-probe": 1, "unkeyed": 1})
        self.assertIsNone(v["would_force_full"])
        self.assertIn("would skip 2 of 5 tests", trs.summary_line(v))

    def test_a_recent_failure_is_never_skipped(self) -> None:
        prior = [{"run_id": "9", "passed": {"ka": {}}, "failed": []},
                 {"run_id": "8", "passed": {}, "failed": ["a"]}]
        v = self.evaluate({"a": ("pass", 1.0)}, prior)
        self.assertEqual(v["would_skip"], 0)
        self.assertEqual(v["ran_reasons"].get("failed-recently"), 1)

    def test_cmake_change_and_control_run_force_a_full_run(self) -> None:
        self.assertEqual(self.evaluate({}, [], cmake=True)["would_force_full"], "cmake-changed")
        self.assertEqual(self.evaluate({}, [], run_id=120)["would_force_full"], "control")
        self.assertIn("would run in full", trs.summary_line(self.evaluate({}, [], run_id=120)))
        self.assertEqual(sum(trs.is_control(r) for r in range(1000, 2000)), 1000 // trs.CONTROL_EVERY)

    def test_receipt_holds_only_passing_keyed_tests(self) -> None:
        keys = {"a": "ka", "b": "kb", "c": "kc", "u": None, "absent": "kx"}
        results = {"a": ("pass", 1.5), "b": ("fail", 1.0), "c": ("skip", 0.0), "u": ("pass", 1.0)}
        r = trs.build_receipt("77", "f" * 40, keys, results)
        self.assertEqual(r["passed"], {"ka": {"name": "a", "seconds": 1.5}})
        self.assertEqual(r["failed"], ["b"])
        self.assertEqual((r["run_id"], r["schema"], r["key_version"]),
                         ("77", trs.RECEIPT_SCHEMA, trs.KEY_VERSION))

    def test_junit_results(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "j.xml"
            p.write_text('<testsuite><testcase name="a" time="1.5"/>'
                         '<testcase name="b" time="2"><failure message="x"/></testcase>'
                         '<testcase name="c" status="notrun"><skipped/></testcase></testsuite>')
            self.assertEqual(trs.junit_results(p), {"a": ("pass", 1.5), "b": ("fail", 2.0),
                                                    "c": ("skip", 0.0)})


def receipt_zip(run_id, schema=trs.RECEIPT_SCHEMA, name=trs.RECEIPT_FILE) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(name, json.dumps({"schema": schema, "key_version": trs.KEY_VERSION,
                                     "run_id": str(run_id), "passed": {f"k{run_id}": {}}, "failed": []}))
    return buf.getvalue()


def run_rec(rid, event="merge_group", path=trs.WORKFLOW_PATH, head="O/R"):
    return {"id": rid, "event": event, "path": path, "repository": {"full_name": "O/R"},
            "head_repository": {"full_name": head}}


class StorageTests(unittest.TestCase):
    def world(self, runs: dict, archives: dict):
        arts = [{"name": trs.ARTIFACT_NAME, "expired": False, "created_at": f"2026-09-{10 + rid:02d}T00:00:00Z",
                 "workflow_run": {"id": rid}, "archive_download_url": f"u/{rid}"} for rid in runs]

        def fetch(url, token):
            if "/actions/artifacts?" in url:
                return {"artifacts": arts}
            return runs[int(url.rsplit("/", 1)[1])]
        return fetch, lambda url, token: archives[int(url.rsplit("/", 1)[1])]

    def test_only_trusted_merge_group_receipts_are_read_newest_first(self) -> None:
        runs = {1: run_rec(1), 2: run_rec(2, event="pull_request"), 3: run_rec(3, head="fork/R"),
                4: run_rec(4, path=".github/workflows/other.yml"), 5: run_rec(5), 6: run_rec(6),
                7: run_rec(7)}
        archives = {r: receipt_zip(r) for r in runs}
        archives[6] = receipt_zip(99)          # receipt names another run
        archives[5] = receipt_zip(5, name="x")  # unexpected layout
        fetch, download = self.world(runs, archives)
        got, refused, failed = trs.load_receipts("O/R", "t", exclude_run="7", fetch=fetch, download=download)
        self.assertEqual([r["run_id"] for r in got], ["1"])
        self.assertEqual(len(refused), 5)
        self.assertEqual(failed, [])
        got, _, _ = trs.load_receipts("O/R", "t", exclude_run=None, fetch=fetch, download=download, lookback=1)
        self.assertEqual([r["run_id"] for r in got], ["7"])

    def test_the_read_budget_stops_reading(self) -> None:
        runs = {r: run_rec(r) for r in range(1, 6)}
        fetch, download = self.world(runs, {r: receipt_zip(r) for r in runs})
        ticks = iter(range(0, 1000, 50))
        got, refused, _ = trs.load_receipts("O/R", "t", None, fetch=fetch, download=download,
                                            budget_secs=120, clock=lambda: next(ticks))
        self.assertLess(len(got), 5)
        self.assertIn("read budget", refused[-1])

    def test_a_401_download_is_a_failed_lookup_not_a_refusal(self) -> None:
        runs = {1: run_rec(1), 2: run_rec(2)}
        fetch, _ = self.world(runs, {})

        def download(url, token):
            err = urllib.error.HTTPError(url, 401, "Server failed to authenticate the request", {}, io.BytesIO())
            self.addCleanup(err.close)
            raise err
        got, refused, failed = trs.load_receipts("O/R", "t", None, fetch=fetch, download=download)
        self.assertEqual((got, refused), ([], []))
        self.assertEqual(failed, ["run 2: HTTP 401 Server failed to authenticate the request",
                                  "run 1: HTTP 401 Server failed to authenticate the request"])
        self.assertEqual(trs.lookup_failure(failed),
                         "receipt lookup failed: HTTP 401 Server failed to authenticate the request (2 request(s))")

    def test_the_default_download_follows_the_blob_redirect_without_the_token(self) -> None:
        with BlobRedirectServer(receipt_zip(4)) as server:
            arts = [{"name": trs.ARTIFACT_NAME, "expired": False, "created_at": "2026-09-14T00:00:00Z",
                     "workflow_run": {"id": 4}, "archive_download_url": server.archive_url}]

            def fetch(url, token):
                return {"artifacts": arts} if "/actions/artifacts?" in url else run_rec(4)
            got, refused, failed = trs.load_receipts("O/R", "tok", None, fetch=fetch)
            self.assertEqual((refused, failed), ([], []))
            self.assertEqual([r["run_id"] for r in got], ["4"])
            self.assertEqual(server.blob_saw_authorization, [False])


class CliTests(unittest.TestCase):
    def test_run_writes_receipts_and_an_annotation_and_exits_0(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            w = World(Path(tmp))
            w.script_inputs = {}
            sel = Path(tmp) / "selected.json"
            sel.write_text(json.dumps({"tests": w.inventory()}))
            junit = Path(tmp) / "j.xml"
            junit.write_text('<testsuite><testcase name="A case one" time="1"/>'
                             '<testcase name="B case" time="1"><failure/></testcase></testsuite>')
            out, summary = Path(tmp) / "r" / "test-receipts.json", Path(tmp) / "summary.md"
            env = {k: v for k, v in os.environ.items() if k != "GITHUB_TOKEN"}
            proc = subprocess.run([sys.executable, str(HERE / "test_receipts_shadow.py"), "run",
                                   "--build-dir", str(w.build), "--source-root", str(w.src),
                                   "--repository", "O/R", "--merge-sha", "HEAD", "--junit", str(junit),
                                   "--selected-json", str(sel), "--run-id", "55", "--receipts-out", str(out),
                                   "--summary", str(summary), "--toolchain-id", "tc",
                                   "--keys-out", str(Path(tmp) / "keys.json")],
                                  capture_output=True, text=True, timeout=120, env=env)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            receipt = json.loads(out.read_text())
            self.assertEqual([v["name"] for v in receipt["passed"].values()], ["A case one"])
            self.assertEqual(receipt["failed"], ["B case"])
            line = next(l for l in proc.stdout.splitlines() if l.startswith("::notice title=test-receipts-shadow::"))
            verdict = json.loads(line.split("::", 2)[2])
            self.assertEqual((verdict["schema"], verdict["would_skip"], verdict["receipt_runs"]),
                             (trs.SCHEMA, 0, 0))
            self.assertIn("Per-test receipts (shadow)", summary.read_text())
            # Every entry is listed with its key, passed or failed; the passing
            # one's key is the receipt's.
            keys = json.loads((Path(tmp) / "keys.json").read_text())
            self.assertEqual(sorted(keys), sorted(t["name"] for t in w.inventory()))
            self.assertIn(keys["A case one"]["key"], receipt["passed"])
            self.assertEqual(keys["B case"]["kind"], "binary")

    def test_a_failed_lookup_is_a_warning_and_named_in_the_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            w = World(Path(tmp))
            w.script_inputs = {}
            sel = Path(tmp) / "selected.json"
            sel.write_text(json.dumps({"tests": w.inventory()}))
            junit = Path(tmp) / "j.xml"
            junit.write_text('<testsuite><testcase name="A case one" time="1"/></testsuite>')
            out, summary = Path(tmp) / "r" / "test-receipts.json", Path(tmp) / "summary.md"
            failed = ["run 3: HTTP 401 Server failed to authenticate the request"]
            stdout = io.StringIO()
            with mock.patch.object(trs, "load_receipts", return_value=([], [], failed)), \
                    contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
                rc = trs.main(["trs", "run", "--build-dir", str(w.build), "--source-root", str(w.src),
                               "--repository", "O/R", "--merge-sha", "HEAD", "--junit", str(junit),
                               "--selected-json", str(sel), "--run-id", "55", "--receipts-out", str(out),
                               "--summary", str(summary), "--toolchain-id", "tc", "--token", "t"])
            self.assertEqual(rc, 0)
            lines = stdout.getvalue().splitlines()
            warning = next(l for l in lines if l.startswith(f"::warning title={trs.TITLE}-lookup::"))
            self.assertIn("receipt lookup failed: HTTP 401", warning)
            notice = next(l for l in lines if l.startswith(f"::notice title={trs.TITLE}::"))
            verdict = json.loads(notice.split("::", 2)[2])
            self.assertEqual(verdict["lookup_errors"], 1)
            self.assertIn("receipt lookup failed: HTTP 401", verdict["lookup_error"])
            self.assertIn("receipt lookup failed: HTTP 401", summary.read_text())

    def test_unreadable_inputs_give_no_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run([sys.executable, str(HERE / "test_receipts_shadow.py"), "run",
                                   "--build-dir", tmp, "--source-root", tmp, "--repository", "O/R",
                                   "--merge-sha", "HEAD", "--junit", f"{tmp}/none.xml",
                                   "--selected-json", f"{tmp}/none.json", "--receipts-out", f"{tmp}/r.json",
                                   "--toolchain-id", "tc"],
                                  capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 2)
        self.assertNotIn("::notice", proc.stdout)


if __name__ == "__main__":
    unittest.main()
