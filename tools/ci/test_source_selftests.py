#!/usr/bin/env python3
"""Tests for the source-only selftest lane (tools/ci/source_selftests.py).

The guard's job is to make one failure impossible to miss: a test that the
required macOS gate excludes and that the build-free required lane does not
run. Each case below builds the smallest inventory that produces one way of
getting there and asserts ``check`` names it; the clean fixture proves the
same instrument returns nothing when the lanes agree.
"""

from __future__ import annotations

import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "scripts"))

import ctest_gate_args  # noqa: E402
import protected_merge_receipt  # noqa: E402
import source_selftests as lane  # noqa: E402

import atexit  # noqa: E402
import shutil  # noqa: E402

# A throwaway checkout: check() reads each entry's script to scan it.
REPO = pathlib.Path(tempfile.mkdtemp(prefix="source-selftest-repo-")).resolve()
atexit.register(shutil.rmtree, REPO, True)
(REPO / "tools" / "scripts").mkdir(parents=True)
(REPO / "tools" / "scripts" / "test_x.py").write_text("print('portable')\n")
BUILD = REPO / "build"
GATE = {"merge_group": "validation|slow|source-selftest"}
WIRED = "run: python3 tools/ci/source_selftests.py run --min-count 130\n"


def registration(name, argv, labels=("source-selftest",), **props):
    properties = [{"name": "LABELS", "value": list(labels)}]
    properties.append(
        {"name": "WORKING_DIRECTORY", "value": props.pop("cwd", str(BUILD / "test"))}
    )
    for key, value in props.items():
        properties.append({"name": key, "value": value})
    return {
        "name": name,
        "command": ["/usr/bin/python3"] + [str(a) for a in argv],
        "properties": properties,
    }


def entry(name, argv, **extra):
    return {"name": name, "argv": argv, **extra}


def run_check(entries, tests, *, gate=GATE, workflow=WIRED, require_all=True):
    return lane.check(
        entries,
        {"tests": tests},
        repo=REPO,
        build_dir=BUILD,
        require_all=require_all,
        gate_label_excludes=gate,
        lane_workflow_text=workflow,
    )


SCRIPT = f"{REPO}/tools/scripts/test_x.py"


class CheckTests(unittest.TestCase):
    def test_agreeing_lanes_report_nothing(self) -> None:
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py"], timeout=60.0)],
            [registration("x", [SCRIPT], TIMEOUT=60)],
        )
        self.assertEqual(errors, [])

    def test_labelled_but_unlisted_test_runs_nowhere(self) -> None:
        errors = run_check([], [registration("orphan", [SCRIPT])])
        self.assertTrue(any("orphan" in e and "neither lane" in e for e in errors), errors)

    def test_listed_but_unlabelled_test_is_reported(self) -> None:
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py"])],
            [registration("x", [SCRIPT], labels=("tools",))],
        )
        self.assertTrue(any("without the source-selftest label" in e for e in errors), errors)

    def test_missing_registration_fails_only_when_required(self) -> None:
        listed = [entry("gone", ["{repo}/tools/scripts/test_x.py"])]
        self.assertTrue(run_check(listed, [], require_all=True))
        self.assertEqual(run_check(listed, [], require_all=False), [])

    def test_command_drift_is_reported(self) -> None:
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py"])],
            [registration("x", [SCRIPT, "--strict"])],
        )
        self.assertTrue(any("does not match the registered command" in e for e in errors), errors)

    def test_build_tree_argument_is_rejected(self) -> None:
        argv = [SCRIPT, "--build-dir", str(BUILD)]
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py", "--build-dir", "{repo}/build"])],
            [registration("x", argv)],
        )
        self.assertTrue(any("reaches the build tree" in e for e in errors), errors)

    def test_absolute_path_outside_the_checkout_is_rejected(self) -> None:
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py", "--cmake", "/opt/bin/cmake"])],
            [registration("x", [SCRIPT, "--cmake", "/opt/bin/cmake"])],
        )
        self.assertTrue(any("outside the checkout" in e for e in errors), errors)

    def test_non_python_command_is_rejected(self) -> None:
        test = registration("x", [SCRIPT])
        test["command"][0] = str(BUILD / "test" / "pulp-test-x")
        errors = run_check([entry("x", ["{repo}/tools/scripts/test_x.py"])], [test])
        self.assertTrue(any("not a Python interpreter" in e for e in errors), errors)

    def test_pr_fast_member_cannot_move(self) -> None:
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py"])],
            [registration("x", [SCRIPT], labels=("pr-fast", "source-selftest"))],
        )
        self.assertTrue(any("pr-fast" in e for e in errors), errors)

    def test_unreproduced_property_is_rejected(self) -> None:
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py"])],
            [registration("x", [SCRIPT], FIXTURES_REQUIRED=["built"])],
        )
        self.assertTrue(any("FIXTURES_REQUIRED" in e for e in errors), errors)

    def test_timeout_env_cwd_and_lock_drift_are_reported(self) -> None:
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py"])],
            [
                registration(
                    "x",
                    [SCRIPT],
                    cwd=f"{REPO}/tools/scripts",
                    TIMEOUT=300,
                    ENVIRONMENT=["A=1"],
                    RESOURCE_LOCK=["lock"],
                )
            ],
        )
        for needle in ("WORKING_DIRECTORY", "TIMEOUT", "ENVIRONMENT", "RESOURCE_LOCK"):
            self.assertTrue(any(needle in e for e in errors), (needle, errors))
        errors = run_check(
            [entry("x", ["{repo}/tools/scripts/test_x.py"])],
            [registration("x", [SCRIPT], PROCESSORS=8)],
        )
        for needle in ("PROCESSORS",):
            self.assertTrue(any(needle in e for e in errors), (needle, errors))

    def test_gate_that_stops_excluding_the_label_is_reported(self) -> None:
        errors = run_check([], [], gate={"merge_group": "validation|slow"})
        self.assertTrue(any("does not exclude source-selftest" in e for e in errors), errors)

    def test_lane_that_stops_running_the_manifest_is_reported(self) -> None:
        errors = run_check([], [], workflow="run: echo nothing\n")
        self.assertTrue(any("would run on no required lane" in e for e in errors), errors)


class PortabilityTests(unittest.TestCase):
    """A test that skips its macOS half on Linux must not leave the gate."""

    CASES = {
        "skip_unless": "@unittest.skipUnless(sys.platform == 'darwin', 'mac')\n",
        "platform_system": "if platform.system() != 'Darwin':\n    raise SkipTest\n",
        "which_codesign": "if shutil.which('codesign') is None: return\n",
        "which_lipo": 'tool = shutil.which("lipo")\n',
        "mac_ver": "if platform.mac_ver()[0]: pass\n",
        "decorator": "@darwin_mutation_proof\ndef test_x(self): pass\n",
        "yaml": "try:\n    import yaml\nexcept ImportError:\n    yaml = None\n",
        "numpy": "import numpy as np\n",
    }

    def _errors(self, body: str) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "t.py").write_text("import sys, shutil, platform\n" + body)
            return lane.portability_errors(entry("t", ["{repo}/t.py"]), root)

    def test_each_marker_is_rejected(self) -> None:
        for case, body in self.CASES.items():
            with self.subTest(case=case):
                self.assertTrue(self._errors(body), case)

    def test_portable_source_passes(self) -> None:
        self.assertEqual(self._errors("print(sys.version)\n"), [])

    def test_unittest_module_entry_is_scanned(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "sub").mkdir()
            (root / "sub" / "test_m.py").write_text("X = 'darwin'\n")
            item = entry("m", ["-m", "unittest", "test_m"], cwd="{repo}/sub")
            self.assertTrue(lane.portability_errors(item, root))


class RunTests(unittest.TestCase):
    def _manifest(self, root: pathlib.Path, body: str, **extra) -> list[dict]:
        script = root / "t.py"
        script.write_text(body, encoding="utf-8")
        return [entry("t", ["{repo}/t.py"], **extra)]

    def test_pass_fail_and_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "ok.py").write_text("print('hi')\n")
            (root / "bad.py").write_text("raise SystemExit(3)\n")
            (root / "slow.py").write_text("import time; time.sleep(30)\n")
            entries = [
                entry("ok", ["{repo}/ok.py"]),
                entry("bad", ["{repo}/bad.py"]),
                entry("slow", ["{repo}/slow.py"], timeout=0.5),
            ]
            results = {
                r["name"]: r for r in lane.run(entries, repo=root, jobs=3, stream=io.StringIO())
            }
        self.assertEqual(results["ok"]["returncode"], 0)
        self.assertEqual(results["bad"]["returncode"], 3)
        self.assertEqual(results["bad"]["attempts"], lane.ATTEMPTS)
        self.assertIsNone(results["slow"]["returncode"])

    def test_whole_machine_entry_runs_alone(self) -> None:
        """PROCESSORS >= jobs must not overlap anything, as under ctest."""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            log = root / "log"
            body = (
                "import sys, time\n"
                f"log = open({str(log)!r}, 'a')\n"
                "log.write(f'start {sys.argv[1]}\\n'); log.flush(); time.sleep(0.3)\n"
                "log.write(f'end {sys.argv[1]}\\n')\n"
            )
            (root / "t.py").write_text(body)
            entries = [entry(f"s{i}", ["{repo}/t.py", f"s{i}"]) for i in range(3)]
            entries.append(entry("big", ["{repo}/t.py", "big"], processors=8))
            lane.run(entries, repo=root, jobs=4, stream=io.StringIO())
            events = log.read_text().split("\n")
        start = events.index("start big")
        self.assertEqual(events[start + 1], "end big", events)

    def test_retry_absorbs_a_single_flake(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            marker = root / "seen"
            entries = self._manifest(
                root,
                "import pathlib, sys\n"
                f"m = pathlib.Path({str(marker)!r})\n"
                "if not m.exists():\n    m.write_text('1'); sys.exit(1)\n",
            )
            (result,) = lane.run(entries, repo=root, stream=io.StringIO())
        self.assertEqual((result["returncode"], result["attempts"]), (0, 2))

    def test_default_cwd_is_empty_scratch_not_the_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            entries = self._manifest(
                root, "import os, sys\nsys.exit(0 if not os.listdir('.') else 1)\n"
            )
            (result,) = lane.run(entries, repo=root, stream=io.StringIO())
        self.assertEqual(result["returncode"], 0, result.get("output"))

    def test_env_and_explicit_cwd_expand_repo(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "sub").mkdir()
            entries = self._manifest(
                root,
                "import os, sys\n"
                "ok = os.environ['P'] == os.path.realpath(os.getcwd())\n"
                "sys.exit(0 if ok else 1)\n",
                cwd="{repo}/sub",
                env={"P": "{repo}/sub"},
            )
            real = pathlib.Path(tmp).resolve()
            (result,) = lane.run(entries, repo=real, stream=io.StringIO())
        self.assertEqual(result["returncode"], 0, result.get("output"))

    def test_min_count_rejects_a_shrunken_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "m.json"
            path.write_text(
                json.dumps({"schema_version": 1, "tests": [entry("a", ["{repo}/a.py"])]})
            )
            self.assertEqual(lane.main(["run", "--manifest", str(path), "--min-count", "2"]), 1)


class RepositoryTests(unittest.TestCase):
    """The checked-in manifest and its two consumers agree."""

    def test_manifest_is_valid_and_points_at_real_scripts(self) -> None:
        entries = lane.load_manifest()
        self.assertGreaterEqual(len(entries), 130)
        for item in entries:
            argv = item["argv"]
            if argv[:2] == ["-m", "unittest"]:
                cwd = pathlib.Path(lane.expand(item["cwd"], lane.REPO_ROOT))
                script = cwd / f"{argv[2]}.py"
            else:
                script = pathlib.Path(lane.expand(argv[0], lane.REPO_ROOT))
            self.assertTrue(script.is_file(), item["name"])

    def test_no_entry_is_platform_gated(self) -> None:
        errors = [
            err
            for item in lane.load_manifest()
            for err in lane.portability_errors(item, lane.REPO_ROOT)
        ]
        self.assertEqual(errors, [])

    def test_gate_and_receipt_exclude_the_same_labels(self) -> None:
        self.assertIn("source-selftest", ctest_gate_args.GATE_LABEL_EXCLUDE.split("|"))
        self.assertEqual(
            protected_merge_receipt.REQUIRED_LABEL_EXCLUDE,
            ctest_gate_args.GATE_LABEL_EXCLUDE,
        )

    def test_wiring_on_the_real_tree(self) -> None:
        errors = lane.check(
            [],
            {"tests": []},
            repo=lane.REPO_ROOT,
            build_dir=lane.REPO_ROOT / "build",
            require_all=False,
            gate_label_excludes=lane._gate_label_excludes(),
            lane_workflow_text=lane.LANE_WORKFLOW.read_text(encoding="utf-8"),
        )
        self.assertEqual(errors, [])

    def test_lane_workflow_reports_on_merge_group(self) -> None:
        text = lane.LANE_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("\n  merge_group:", text)
        self.assertIn("name: Enforce version & skill sync", text)


class DiffSelectionTests(unittest.TestCase):
    """--changed-from runs what a diff can reach, and all of it for a lane change."""

    def _tree(self, root: pathlib.Path) -> list[dict]:
        (root / "tools").mkdir()
        (root / "tools" / "test_hook.py").write_text(
            'HOOK = ROOT / ".githooks/pre-push"\n', encoding="utf-8")
        (root / "tools" / "test_helper_user.py").write_text(
            "import widget_math\n", encoding="utf-8")
        (root / "tools" / "test_unrelated.py").write_text(
            "print('nothing here')\n", encoding="utf-8")
        (root / "tools" / "test_self.py").write_text("pass\n", encoding="utf-8")
        return [
            entry("hook", ["{repo}/tools/test_hook.py"]),
            entry("helper", ["{repo}/tools/test_helper_user.py"]),
            entry("unrelated", ["{repo}/tools/test_unrelated.py"]),
            entry("self", ["{repo}/tools/test_self.py"]),
        ]

    def _names(self, changed: list[str]) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            entries = self._tree(root)
            return sorted(e["name"] for e in lane.select_for_changes(entries, changed, root))

    def test_a_referenced_file_selects_its_reader(self) -> None:
        self.assertEqual(self._names([".githooks/pre-push"]), ["hook"])

    def test_a_changed_module_selects_its_importer(self) -> None:
        self.assertEqual(self._names(["tools/scripts/widget_math.py"]), ["helper"])

    def test_a_changed_test_selects_itself(self) -> None:
        self.assertEqual(self._names(["tools/test_self.py"]), ["self"])

    def test_a_lane_change_selects_everything(self) -> None:
        self.assertEqual(len(self._names(["tools/ci/source_selftests.json"])), 4)

    def test_generic_basenames_do_not_select(self) -> None:
        self.assertEqual(self._names(["docs/unrelated/SKILL.md"]), [])

    def test_an_unrelated_change_selects_nothing(self) -> None:
        self.assertEqual(self._names(["core/audio/src/buffer.cpp"]), [])


class GatesWiringTests(unittest.TestCase):
    """gates.sh runs the diff-scoped lane and fails when it fails."""

    GATES = HERE.parent / "scripts" / "gates.sh"

    def _block(self) -> str:
        text = self.GATES.read_text(encoding="utf-8")
        start = text.index("# ── 7a-src.")
        end = text.index("# ── 7a-bis.", start)
        return text[start:end]

    def _run_block(self, stub_exit: int) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "tools" / "ci").mkdir(parents=True)
            (root / ".github" / "workflows").mkdir(parents=True)
            (root / ".github" / "workflows" / "workflow-lint.yml").write_text("", encoding="utf-8")
            calls = root / "calls"
            (root / "tools" / "ci" / "source_selftests.py").write_text(
                "import sys, pathlib\n"
                f"with open({str(calls)!r}, 'a') as f: f.write(' '.join(sys.argv[1:]) + '\\n')\n"
                "print('source-selftests: stub')\n"
                f"raise SystemExit({stub_exit})\n",
                encoding="utf-8",
            )
            script = (f'ROOT={str(root)!r}\nPYTHON={sys.executable!r}\nBASE=origin/main\nfail=0\n'
                      + self._block() + '\necho "fail=$fail"\n')
            env = {k: v for k, v in os.environ.items()
                   if k != "PULP_SKIP_SOURCE_SELFTESTS"}
            result = subprocess.run(
                ["bash", "-c", script], capture_output=True, text=True, env=env, timeout=60)
            argv = calls.read_text() if calls.exists() else ""
        return result.stdout.strip() + "|" + argv

    def test_a_failing_lane_fails_gates(self) -> None:
        out, argv = self._run_block(1).split("|", 1)
        self.assertEqual(out, "fail=1")
        calls = argv.splitlines()
        self.assertEqual(calls[0], "run --changed-from origin/main --label-base-failures")

    def test_both_lanes_run_and_a_passing_pair_stays_green(self) -> None:
        out, argv = self._run_block(0).split("|", 1)
        self.assertEqual(out, "fail=0")
        calls = argv.splitlines()
        self.assertEqual(len(calls), 4, calls)
        self.assertEqual(calls[0], "run --changed-from origin/main --label-base-failures")
        self.assertRegex(calls[1], r"^run --workflow \S+/\.github/workflows/workflow-lint\.yml "
                                   r"--changed-from origin/main --label-base-failures$")
        self.assertRegex(calls[2], r"^run --ctest-label pr-fast --build-dir \S+/build "
                                   r"--changed-from origin/main --label-base-failures$")
        self.assertEqual(calls[3], "run --ctest-python --changed-from origin/main "
                                   "--label-base-failures")


class WorkflowEntriesTests(unittest.TestCase):
    """The workflow-lint list is read from the workflow, and scoped like the lane."""

    def _workflow(self, root: pathlib.Path) -> pathlib.Path:
        for name in ("test_contract.py", "test_other.py", "portability.py"):
            (root / "tools" / "scripts").mkdir(parents=True, exist_ok=True)
            (root / "tools" / "scripts" / name).write_text("pass\n", encoding="utf-8")
        (root / "tools" / "scripts" / "test_contract.py").write_text(
            'MANIFEST = ROOT / "test" / "cmake" / "design_import_tool_cli_tests.cmake"\n',
            encoding="utf-8")
        (root / "tools" / "ci").mkdir(parents=True, exist_ok=True)
        workflow = root / "workflow-lint.yml"
        workflow.write_text(
            "      - run: |\n"
            "          python3 -m pip install --quiet 'pyyaml>=6'\n"
            "          python3 tools/scripts/test_contract.py\n"
            "          python3 tools/scripts/test_other.py\n"
            "          python3 tools/scripts/portability.py tools/ci tools/scripts\n"
            "          python3 tools/scripts/test_missing.py\n"
            "          python3 - <<'PY'\n",
            encoding="utf-8")
        return workflow

    def test_every_repo_script_line_becomes_an_entry(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            entries = lane.workflow_entries(self._workflow(root), root)
        self.assertEqual(
            [e["name"] for e in entries],
            ["tools/scripts/test_contract.py", "tools/scripts/test_other.py",
             "tools/scripts/portability.py tools/ci tools/scripts"])
        self.assertTrue(all(e["cwd"] == "{repo}" for e in entries))

    def test_a_ctest_property_change_selects_the_contract_that_pins_it(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            entries = lane.workflow_entries(self._workflow(root), root)
            picked = lane.select_for_changes(
                entries, ["test/cmake/design_import_tool_cli_tests.cmake"], root)
        self.assertEqual([e["name"] for e in picked], ["tools/scripts/test_contract.py"])

    def test_a_checker_handed_a_directory_is_selected_by_a_change_inside_it(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            entries = lane.workflow_entries(self._workflow(root), root)
            picked = lane.select_for_changes(entries, ["tools/ci/new_runner.sh"], root)
        self.assertEqual([e["name"] for e in picked],
                         ["tools/scripts/portability.py tools/ci tools/scripts"])

    def test_every_local_skip_names_a_real_workflow_suite(self) -> None:
        # A skip for a suite the workflow no longer runs would hide nothing and
        # rot silently; a skip must name a live entry.
        names = {e["name"] for e in lane.workflow_entries()}
        names |= {e["name"] for e in lane.python_ctest_entries()}
        for skipped in lane.WORKFLOW_LOCAL_SKIPS:
            self.assertIn(skipped, names)

    def test_the_real_workflow_yields_its_contract_suites(self) -> None:
        # Control: the parser sees the real workflow, not an empty list.
        entries = lane.workflow_entries()
        self.assertGreaterEqual(len(entries), 40)
        self.assertIn("tools/scripts/test_ci_throughput_workflows.py",
                      [e["name"] for e in entries])


class BaseFailureTests(unittest.TestCase):
    """A red main is labelled pre-existing; a branch's own new failure still fails."""

    UNITTEST_A = "FAIL: test_a (suite.T.test_a)\n"
    UNITTEST_AB = "FAIL: test_a (suite.T.test_a)\nERROR: test_b (suite.T.test_b)\n"

    def test_failing_test_names_reads_unittest_pytest_and_plain_output(self) -> None:
        self.assertEqual(lane.failing_test_names(self.UNITTEST_AB),
                         {"suite.T.test_a", "suite.T.test_b"})
        self.assertEqual(lane.failing_test_names("FAILED tests/x.py::test_y - boom\n"),
                         {"tests/x.py::test_y"})
        self.assertEqual(lane.failing_test_names("FAIL: slow output bypasses capture\n"),
                         {"slow output bypasses capture"})

    def _r(self, rc, out):
        return {"name": "s", "returncode": rc, "output": out}

    def test_same_failures_on_base_are_pre_existing(self) -> None:
        on_base, _ = lane.base_verdict(self._r(1, self.UNITTEST_A), self._r(1, self.UNITTEST_AB))
        self.assertTrue(on_base)

    def test_a_new_failing_test_is_the_branch(self) -> None:
        on_base, why = lane.base_verdict(self._r(1, self.UNITTEST_AB), self._r(1, self.UNITTEST_A))
        self.assertFalse(on_base)
        self.assertIn("suite.T.test_b", why)

    def test_unprovable_cases_stay_failures(self) -> None:
        for branch, base in (
            (self._r(1, self.UNITTEST_A), self._r(0, "")),          # passes on base
            (self._r(1, self.UNITTEST_A), None),                    # absent on base
            (self._r(1, "Traceback, no names"), self._r(1, "x")),   # unnamed
            (self._r(None, self.UNITTEST_A), self._r(1, self.UNITTEST_A)),  # timeout
        ):
            with self.subTest(branch=branch, base=base):
                self.assertFalse(lane.base_verdict(branch, base)[0])

    def test_an_unnamed_failure_identical_on_base_is_pre_existing(self) -> None:
        branch = self._r(1, "[Errno 2] No such file: /work/branch/planning/plan.md (0.08s)")
        base = self._r(1, "[Errno 2] No such file: /work/base/planning/plan.md (0.11s)")
        self.assertTrue(lane.base_verdict(branch, base, "/work/branch", "/work/base")[0])
        other = self._r(1, "[Errno 2] No such file: /work/base/planning/other.md (0.11s)")
        self.assertFalse(lane.base_verdict(branch, other, "/work/branch", "/work/base")[0])

    def test_a_shared_build_path_in_the_base_output_still_matches(self) -> None:
        out = "ERROR: 2 duplicates\n  /work/branch/build/test/a x\n"
        branch, base = self._r(1, out), self._r(1, out)
        self.assertTrue(lane.base_verdict(branch, base, "/work/branch", "/work/base")[0])

    def _git(self, repo: pathlib.Path, *args: str) -> str:
        return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True,
                              text=True).stdout.strip()

    def _suite(self, failing: list[str]) -> str:
        body = "import unittest\nclass T(unittest.TestCase):\n"
        for name in ("a", "b"):
            body += f"    def test_{name}(self):\n        self.assertTrue({name not in failing})\n"
        return body + "unittest.main()\n"

    def _repo(self, root: pathlib.Path, base_failing: list[str], head_failing: list[str]):
        repo = root / "repo"
        repo.mkdir()
        self._git(repo, "init", "-q", "-b", "main")
        self._git(repo, "config", "user.email", "t@example.com")
        self._git(repo, "config", "user.name", "t")
        self._git(repo, "config", "commit.gpgsign", "false")
        (repo / "suite.py").write_text(self._suite(base_failing), encoding="utf-8")
        self._git(repo, "add", "suite.py")
        self._git(repo, "commit", "-q", "-m", "base")
        base = self._git(repo, "rev-parse", "HEAD")
        (repo / "suite.py").write_text(self._suite(head_failing), encoding="utf-8")
        (repo / "other.txt").write_text("x", encoding="utf-8")
        self._git(repo, "add", "suite.py", "other.txt")
        self._git(repo, "commit", "-q", "-m", "head")
        entries = [entry("suite", ["{repo}/suite.py"])]
        failed = [r for r in lane.run(entries, repo=repo, stream=io.StringIO())
                  if r["returncode"] != 0]
        self.assertEqual(len(failed), 1)
        return lane.rerun_on_base(failed, entries, base, repo=repo)["suite"], repo

    def test_a_red_base_is_labelled_and_leaves_no_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (on_base, why), repo = self._repo(pathlib.Path(tmp), ["a"], ["a"])
            worktrees = self._git(repo, "worktree", "list")
        self.assertTrue(on_base, why)
        self.assertEqual(len(worktrees.splitlines()), 1, worktrees)

    def test_control_a_failure_the_branch_added_still_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (on_base, why), _ = self._repo(pathlib.Path(tmp), ["a"], ["a", "b"])
        self.assertFalse(on_base)
        self.assertIn("test_b", why)


class CtestLaneTests(unittest.TestCase):
    """The pr-fast and ctest-Python lanes run what ctest registered, as ctest would."""

    def test_verdict_honours_ctest_properties(self) -> None:
        self.assertEqual(lane.ctest_verdict({"skip_return_code": 77}, 77, ""), 0)
        self.assertEqual(lane.ctest_verdict({"pass_regex": ["a.*b"]}, 3, "a\nb"), 0)
        self.assertEqual(lane.ctest_verdict({"pass_regex": ["zzz"]}, 0, "ok"), 1)
        self.assertEqual(lane.ctest_verdict({"will_fail": True}, 1, ""), 0)
        self.assertEqual(lane.ctest_verdict({"will_fail": True}, 0, ""), 1)
        self.assertEqual(lane.ctest_verdict({}, 2, ""), 2)

    def test_build_paths_stay_absolute_for_a_base_rerun(self) -> None:
        repo = pathlib.Path("/work/branch")
        build = repo / "build"
        entry = lane._entry_from_command(
            "inv", ["/usr/bin/python3", "/work/branch/tools/inv.py", "--build-dir",
                    "/work/branch/build"], {"WORKING_DIRECTORY": "/work/branch/build/test"},
            repo, build)
        # Checkout paths follow the run to the base; the build tree does not.
        self.assertEqual(entry["argv"][1:], ["{repo}/tools/inv.py", "--build-dir", "/work/branch/build"])
        self.assertEqual(entry["cwd"], "/work/branch/build/test")

    def test_a_build_older_than_the_manifests_is_stale(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "test" / "cmake").mkdir(parents=True)
            (root / "build").mkdir()
            manifest = root / "test" / "cmake" / "x_tests.cmake"
            stamp = root / "build" / "CTestTestfile.cmake"
            manifest.write_text("")
            stamp.write_text("")
            os.utime(manifest, (1000, 1000))
            os.utime(stamp, (2000, 2000))
            self.assertFalse(lane.build_is_stale(root / "build", root))
            os.utime(manifest, (3000, 3000))
            self.assertTrue(lane.build_is_stale(root / "build", root))

    def test_a_missing_working_directory_is_a_failure_not_a_crash(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            results = lane.run([{"name": "x", "raw": True, "argv": ["true"],
                                 "cwd": "{repo}/absent"}],
                               repo=pathlib.Path(tmp), stream=io.StringIO())
        self.assertEqual(results[0]["returncode"], 1)
        self.assertIn("working directory does not exist", results[0]["output"])

    def _tree(self, root: pathlib.Path) -> None:
        (root / "test" / "cmake").mkdir(parents=True)
        (root / "tools" / "ci").mkdir(parents=True)
        (root / "tools" / "ci" / "source_selftests.json").write_text(
            json.dumps({"schema_version": 1, "tests": [{"name": "m", "argv": ["{repo}/tools/moved.py"]}]}))
        (root / "test" / "cmake" / "pr_fast_tests.cmake").write_text(
            "add_test(NAME fast-one COMMAND ${Python3_EXECUTABLE}\n"
            '    "${CMAKE_SOURCE_DIR}/tools/fast.py" --flag)\n'
            "set_tests_properties(fast-one PROPERTIES TIMEOUT 42 SKIP_RETURN_CODE 77)\n"
            "add_test(NAME fast-needs-build COMMAND ${Python3_EXECUTABLE}\n"
            '    "${CMAKE_SOURCE_DIR}/tools/inv.py" --build-dir "${CMAKE_BINARY_DIR}")\n'
            "set(PULP_PR_FAST_TESTS\n    fast-one\n    fast-needs-build  # comment\n)\n")
        (root / "test" / "cmake" / "other_tests.cmake").write_text(
            "add_test(NAME contract COMMAND ${Python3_EXECUTABLE}\n"
            '    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/contract.py")\n'
            "add_test(NAME moved COMMAND ${Python3_EXECUTABLE} \"${CMAKE_SOURCE_DIR}/tools/moved.py\")\n"
            "add_test(NAME binary COMMAND pulp-test-thing)\n")
        for name in ("fast.py", "inv.py", "contract.py", "moved.py"):
            (root / "tools" / name).write_text("pass\n")

    def test_pr_fast_members_are_read_from_the_manifests(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._tree(root)
            entries, not_checked = lane.ctest_label_entries_from_source("pr-fast", root)
        self.assertEqual([e["name"] for e in entries], ["fast-one"])
        self.assertEqual(entries[0]["argv"][1:], ["{repo}/tools/fast.py", "--flag"])
        self.assertEqual((entries[0]["timeout"], entries[0]["skip_return_code"]), (42.0, 77))
        self.assertIn("CMAKE_BINARY_DIR", not_checked["fast-needs-build"])

    def test_python_ctests_exclude_the_other_lanes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._tree(root)
            entries = lane.python_ctest_entries(root)
        # Not the pr-fast member, not the manifest's script, not a binary; the
        # current source dir resolves to test/, as the include() gives it.
        self.assertEqual([e["name"] for e in entries], ["contract"])
        self.assertEqual(entries[0]["argv"][1], "{repo}/test/../tools/contract.py")

    def test_a_directory_walking_contract_is_selected_by_a_change_inside(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            (root / "test" / "cmake").mkdir(parents=True)
            (root / "tools").mkdir()
            (root / "tools" / "walker.py").write_text(
                'for p in (REPO / "test").rglob("*.cmake"): pass\n')
            (root / "tools" / "reader.py").write_text('open("test/x.cmake")\n')
            entries = [entry("walker", ["{repo}/tools/walker.py"]),
                       entry("reader", ["{repo}/tools/reader.py"])]
            picked = lane.select_for_changes(entries, ["test/cmake/new_tests.cmake"], root)
        # The walker reads everything under test/; the reader names one other file.
        self.assertEqual([e["name"] for e in picked], ["walker"])


class ImportClosureSelectionTests(unittest.TestCase):
    """A changed module selects every suite that loads it through repo imports."""

    def _repo(self, root: pathlib.Path) -> list[dict]:
        scripts = root / "tools" / "scripts"
        scripts.mkdir(parents=True)
        (scripts / "census.py").write_text("def header_names(): return []\n")
        (scripts / "batch_attr.py").write_text("import census\n")
        (scripts / "test_batch.py").write_text(
            "import sys, pathlib\nsys.path.insert(0, str(pathlib.Path(__file__).parent))\n"
            "import batch_attr\n")
        (scripts / "test_ns.py").write_text("from tools.scripts import batch_attr\n")
        (scripts / "test_other.py").write_text("import json\n")
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        return [entry("batch", ["{repo}/tools/scripts/test_batch.py"]),
                entry("ns", ["{repo}/tools/scripts/test_ns.py"]),
                entry("other", ["{repo}/tools/scripts/test_other.py"])]

    def test_a_module_two_imports_away_selects_its_suites(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            entries = self._repo(root)
            picked = lane.select_for_changes(entries, ["tools/scripts/census.py"], root)
        # Neither suite names census; each loads it through batch_attr, one by a
        # sys.path sibling import, one through the repo-root namespace.
        self.assertEqual([e["name"] for e in picked], ["batch", "ns"])

    def test_control_an_unimported_module_selects_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            entries = self._repo(root)
            (root / "tools" / "scripts" / "unused.py").write_text("x = 1\n")
            picked = lane.select_for_changes(entries, ["tools/scripts/unused.py"], root)
        self.assertEqual(picked, [])


if __name__ == "__main__":
    unittest.main()
