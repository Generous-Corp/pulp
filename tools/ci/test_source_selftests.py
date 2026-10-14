#!/usr/bin/env python3
"""Tests for the source-only selftest lane (tools/ci/source_selftests.py).

The guard's job is to make one failure impossible to miss: a test that the
required macOS gate excludes and that the build-free required lane does not
run. Each case below builds the smallest inventory that produces one way of
getting there and asserts ``check`` names it; the clean fixture proves the
same instrument returns nothing when the lanes agree.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
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
WIRED = (
    "run: python3 tools/ci/source_selftests.py run --min-count 130 --part shared\n"
    "shard: [1, 2]\n"
    "run: python3 tools/ci/source_selftests.py run --min-count 130 --part exclusive "
    "--shard ${{ matrix.shard }}/2\n"
)


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


# Fixture manifests hold none of the real whole-machine entries.
NO_PLAN = ((),) * lane.EXCLUSIVE_SHARDS


def run_check(entries, tests, *, gate=GATE, workflow=WIRED, require_all=True):
    return lane.check(
        entries,
        {"tests": tests},
        repo=REPO,
        build_dir=BUILD,
        require_all=require_all,
        gate_label_excludes=gate,
        lane_workflow_text=workflow,
        shard_plan=NO_PLAN,
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

    def test_lane_that_drops_one_part_of_the_split_is_reported(self) -> None:
        for dropped in WIRED.splitlines():
            workflow = WIRED.replace(dropped + "\n", "")
            errors = run_check([], [], workflow=workflow)
            self.assertTrue(
                any("would run on no required lane" in e for e in errors), (dropped, errors)
            )


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


class SkipAccountingTests(unittest.TestCase):
    """A skip is reported as a skip: never counted as passed, never a green
    run on its own."""

    def _report(self, root: pathlib.Path, entries: list[dict]) -> tuple[int, str]:
        args = argparse.Namespace(jobs=2, label_base_failures=False, changed_from=None)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = lane._run_and_report(args, argparse.ArgumentParser(), entries, time.monotonic())
        return rc, out.getvalue()

    def _tree(self, root: pathlib.Path) -> None:
        (root / "ok.py").write_text("print('ran')\n", encoding="utf-8")
        (root / "exit77.py").write_text("raise SystemExit(77)\n", encoding="utf-8")
        (root / "says_skip.py").write_text(
            "print('thing_skipped=no plan')\nraise SystemExit(1)\n", encoding="utf-8")

    def test_a_skip_return_code_is_skipped_not_passed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._tree(root)
            (result,) = lane.run([entry("s", ["{repo}/exit77.py"], skip_return_code=77)],
                                 repo=root, stream=io.StringIO())
        self.assertEqual((result["returncode"], result["skipped"]), (0, True))

    def test_a_skip_regex_with_a_failing_exit_is_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._tree(root)
            stream = io.StringIO()
            (result,) = lane.run([entry("s", ["{repo}/says_skip.py"],
                                        skip_regex=["thing_skipped="])],
                                 repo=root, stream=stream)
        self.assertEqual((result["returncode"], result["skipped"]), (0, True))
        self.assertIn("... Skipped", stream.getvalue())
        self.assertNotIn("Passed", stream.getvalue())

    def test_an_executed_pass_is_not_marked_skipped(self) -> None:
        # The control: without it the checks above could pass with every
        # result marked skipped.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._tree(root)
            (result,) = lane.run([entry("p", ["{repo}/ok.py"], skip_return_code=77)],
                                 repo=root, stream=io.StringIO())
        self.assertEqual((result["returncode"], result["skipped"]), (0, False))

    def test_summary_counts_skips_apart_from_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._tree(root)
            rc, out = self._report(root, [
                entry("p", [str(root / "ok.py")]),
                entry("s", [str(root / "exit77.py")], skip_return_code=77),
            ])
        self.assertEqual(rc, 0, out)
        self.assertIn("1 passed, 1 skipped, 0 failed", out)

    def test_an_all_skipped_selection_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._tree(root)
            rc, out = self._report(root, [
                entry("s1", [str(root / "exit77.py")], skip_return_code=77),
                entry("s2", [str(root / "says_skip.py")], skip_regex=["thing_skipped="]),
            ])
        self.assertEqual(rc, 1, out)
        self.assertIn("0 passed, 2 skipped", out)
        self.assertIn("nothing was executed", out)


def _locked(name, lock, **extra):
    return entry(name, [f"{{repo}}/{name}.py"], resource_lock=[lock], **extra)


class LanePartTests(unittest.TestCase):
    """The split across the required lane's jobs drops and doubles nothing."""

    ENTRIES = [
        entry("a", ["{repo}/a.py"]),
        entry("b", ["{repo}/b.py"], timeout=300.0),
        entry("big1", ["{repo}/big1.py"], processors=8, timeout=300.0),
        entry("big2", ["{repo}/big2.py"], processors=8, timeout=300.0),
        _locked("big3", "shared-lock", processors=8),
        _locked("small-locked", "shared-lock"),
        entry("big4", ["{repo}/big4.py"], processors=8),
    ]
    PLAN = (("big1", "big4"), ("big2", "big3"))

    def test_every_entry_lands_in_exactly_one_part(self) -> None:
        parts = lane.lane_parts(self.ENTRIES, plan=self.PLAN)
        names = [e["name"] for members in parts.values() for e in members]
        self.assertEqual(sorted(names), sorted(e["name"] for e in self.ENTRIES))
        self.assertEqual(lane.part_errors(self.ENTRIES, plan=self.PLAN), [])

    def test_the_plan_decides_each_shard(self) -> None:
        parts = lane.lane_parts(self.ENTRIES, plan=self.PLAN)
        self.assertEqual(sorted(e["name"] for e in parts["exclusive-1/2"]), ["big1", "big4"])
        self.assertEqual(sorted(e["name"] for e in parts["exclusive-2/2"]),
                         ["big2", "big3", "small-locked"])

    def test_an_unplanned_whole_machine_entry_still_runs_but_is_reported(self) -> None:
        plan = (("big1",), ("big2", "big3"))
        parts = lane.lane_parts(self.ENTRIES, plan=plan)
        exclusive = {e["name"] for key, members in parts.items()
                     if key != lane.SHARED_PART for e in members}
        self.assertIn("big4", exclusive)
        errors = lane.part_errors(self.ENTRIES, plan=plan)
        self.assertTrue(any(e.startswith("big4:") and "does not place" in e
                            for e in errors), errors)

    def test_a_plan_naming_a_shared_entry_is_reported(self) -> None:
        plan = (("big1", "big4", "a"), ("big2", "big3"))
        errors = lane.part_errors(self.ENTRIES, plan=plan)
        self.assertTrue(any(e.startswith("a:") and "not a whole-machine" in e
                            for e in errors), errors)

    def test_a_plan_that_splits_a_lock_group_is_reported(self) -> None:
        locked = self.ENTRIES + [_locked("big5", "shared-lock", processors=8)]
        plan = (("big1", "big4", "big5"), ("big2", "big3"))
        errors = lane.part_errors(locked, plan=plan)
        self.assertTrue(any(e.startswith("big3:") and "resource lock" in e
                            for e in errors), errors)

    def test_exclusive_entries_never_run_in_the_shared_part(self) -> None:
        shared = lane.lane_parts(self.ENTRIES, plan=self.PLAN)[lane.SHARED_PART]
        self.assertEqual([e["name"] for e in shared], ["a", "b"])

    def test_a_resource_lock_never_spans_two_jobs(self) -> None:
        parts = lane.lane_parts(self.ENTRIES, plan=self.PLAN)
        holding = [key for key, members in parts.items()
                   if {"big3", "small-locked"} & {e["name"] for e in members}]
        self.assertEqual(len(holding), 1, parts)
        members = {e["name"] for e in parts[holding[0]]}
        self.assertLessEqual({"big3", "small-locked"}, members)

    def test_the_split_is_a_function_of_the_manifest(self) -> None:
        forward = lane.lane_parts(self.ENTRIES, plan=self.PLAN)
        backward = lane.lane_parts(list(reversed(self.ENTRIES)), plan=self.PLAN)
        self.assertEqual(
            {k: sorted(e["name"] for e in v) for k, v in forward.items()},
            {k: sorted(e["name"] for e in v) for k, v in backward.items()},
        )

    def test_a_doubled_entry_is_reported(self) -> None:
        doubled = self.ENTRIES + [dict(self.ENTRIES[0])]
        self.assertTrue(any("not exactly one" in e
                            for e in lane.part_errors(doubled, plan=self.PLAN)))

    def test_bad_selectors_are_refused(self) -> None:
        for part, shard in (("exclusive", None), ("exclusive", "3/3"),
                            ("exclusive", "0/2"), ("shared", "1/2"), ("all", "1/2")):
            with self.assertRaises(ValueError, msg=(part, shard)):
                lane.select_part(self.ENTRIES, part, shard)

    def _run_part(self, root: pathlib.Path, entries, *argv) -> tuple[int, str]:
        for item in entries:
            (root / f"{item['name']}.py").write_text(
                f"open({str(root / 'ran')!r}, 'a', encoding='utf-8').write({item['name']!r} + '\\n')\n",
                encoding="utf-8",
            )
        manifest = root / "m.json"
        manifest.write_text(json.dumps({"schema_version": 1, "tests": [
            {**item, "argv": [f"{root}/{item['name']}.py"]} for item in entries]}),
            encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = lane.main(["run", "--manifest", str(manifest), *argv])
        ran = (root / "ran").read_text(encoding="utf-8").split() if (root / "ran").exists() else []
        return code, sorted(ran)

    def test_the_parts_run_every_entry_between_them(self) -> None:
        ran: list[str] = []
        for argv in (["--part", "shared"],
                     ["--part", "exclusive", "--shard", "1/2"],
                     ["--part", "exclusive", "--shard", "2/2"]):
            with tempfile.TemporaryDirectory() as tmp:
                code, names = self._run_part(pathlib.Path(tmp), self.ENTRIES, *argv)
            self.assertEqual(code, 0, argv)
            ran.extend(names)
        self.assertEqual(sorted(ran), sorted(e["name"] for e in self.ENTRIES))

    def test_min_count_still_counts_the_whole_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            code, ran = self._run_part(
                pathlib.Path(tmp), self.ENTRIES,
                "--part", "shared", "--min-count", str(len(self.ENTRIES) + 1))
        self.assertEqual((code, ran), (1, []))

    def test_an_empty_part_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            code, ran = self._run_part(
                pathlib.Path(tmp), [entry("a", ["{repo}/a.py"])],
                "--part", "exclusive", "--shard", "1/2")
        self.assertEqual((code, ran), (1, []))


def _workflow_job(text: str, job: str) -> str:
    start = text.index(f"\n  {job}:\n")
    rest = text[start + 1:]
    following = [i for i in (rest.find(f"\n  {name}:\n") for name in _job_ids(text))
                 if i > 0]
    return rest[:min(following)] if following else rest


def _job_ids(text: str) -> list[str]:
    body = text[text.index("\njobs:\n"):]
    return [line[2:-1] for line in body.splitlines()
            if line.startswith("  ") and not line.startswith("   ")
            and line.endswith(":") and not line.lstrip().startswith("#")]


def _run_block(job_text: str) -> str:
    lines = job_text.splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip() == "run: |") + 1
    block = []
    for line in lines[start:]:
        if line.strip() and not line.startswith(" " * 10):
            break
        block.append(line[10:])
    return "\n".join(block) + "\n"


def aggregator_verdicts(script: str) -> dict[tuple[str, str], int]:
    """Exit code of the aggregator script for every pair of part results."""
    outcomes = ("success", "failure", "cancelled", "skipped", "")
    verdicts = {}
    for checks in outcomes:
        for exclusive in outcomes:
            env = {**os.environ, "CHECKS_RESULT": checks, "EXCLUSIVE_RESULT": exclusive}
            verdicts[(checks, exclusive)] = subprocess.run(
                ["bash", "-e", "-c", script], env=env, capture_output=True
            ).returncode
    return verdicts


class RequiredAggregatorTests(unittest.TestCase):
    """The required context passes only when every part of the lane passed."""

    TEXT = lane.LANE_WORKFLOW.read_text(encoding="utf-8")

    def test_the_required_name_sits_on_an_always_run_job_needing_every_other(self) -> None:
        job = _workflow_job(self.TEXT, "enforce")
        self.assertIn("    name: Enforce version & skill sync\n", job)
        self.assertEqual(self.TEXT.count("name: Enforce version & skill sync\n"), 1)
        self.assertIn("    if: always()\n", job)
        others = [name for name in _job_ids(self.TEXT) if name != "enforce"]
        self.assertEqual(sorted(others), ["exclusive-selftests", "version-skill-check"])
        self.assertIn(f"    needs: [{', '.join(others)}]\n", job)
        self.assertIn("CHECKS_RESULT: ${{ needs.version-skill-check.result }}", job)
        self.assertIn("EXCLUSIVE_RESULT: ${{ needs.exclusive-selftests.result }}", job)

    def test_only_two_successes_pass(self) -> None:
        verdicts = aggregator_verdicts(_run_block(_workflow_job(self.TEXT, "enforce")))
        passing = [pair for pair, code in verdicts.items() if code == 0]
        self.assertEqual(passing, [("success", "success")])

    def test_control_a_script_that_lets_a_skipped_part_pass_is_caught(self) -> None:
        script = _run_block(_workflow_job(self.TEXT, "enforce"))
        guard = 'if [ "$result" = "success" ]; then'
        self.assertIn(guard, script)
        mutant = script.replace(
            guard, 'if [ "$result" = "success" ] || [ "$result" = "skipped" ]; then')
        verdicts = aggregator_verdicts(mutant)
        self.assertEqual(verdicts[("success", "skipped")], 0)
        self.assertNotEqual(
            [pair for pair, code in verdicts.items() if code == 0], [("success", "success")])


class NeutralizerTests(unittest.TestCase):
    """A wired part must not be skippable or softened in place."""

    TEXT = lane.LANE_WORKFLOW.read_text(encoding="utf-8")
    EXCLUSIVE_RUN = "      - name: Exclusive source selftests\n"
    SHARED_RUN = "--part shared"

    def test_real_workflow_is_clean(self) -> None:
        self.assertEqual(lane.wiring_errors(self.TEXT), [])

    def test_step_level_if_false_on_the_exclusive_run_is_caught(self) -> None:
        self.assertIn(self.EXCLUSIVE_RUN, self.TEXT)
        for inserted in ("        if: false\n", "        continue-on-error: true\n"):
            with self.subTest(inserted=inserted):
                mutant = self.TEXT.replace(self.EXCLUSIVE_RUN, self.EXCLUSIVE_RUN + inserted, 1)
                self.assertNotEqual(mutant, self.TEXT)
                errors = lane.wiring_errors(mutant)
                self.assertTrue(any("--part exclusive" in e and inserted.strip() in e
                                    for e in errors), errors)

    def test_if_as_the_first_key_of_the_step_is_caught(self) -> None:
        mutant = self.TEXT.replace(self.EXCLUSIVE_RUN,
                                   "      - if: false\n        name: Exclusive source selftests\n", 1)
        self.assertNotEqual(mutant, self.TEXT)
        self.assertTrue(any("if: false" in e for e in lane.wiring_errors(mutant)))

    def test_step_level_if_on_the_shared_run_is_caught(self) -> None:
        lines = self.TEXT.splitlines(keepends=True)
        index = next(i for i, line in enumerate(lines) if self.SHARED_RUN in line)
        while not lines[index].lstrip().startswith("- "):
            index -= 1
        mutant = "".join(lines[:index + 1] + ["        if: github.event_name == 'push'\n"]
                         + lines[index + 1:])
        errors = lane.wiring_errors(mutant)
        self.assertTrue(any("--part shared" in e for e in errors), errors)

    def test_continue_on_error_on_a_lane_job_is_caught(self) -> None:
        for job, anchor in (("enforce", "    needs: [version-skill-check, exclusive-selftests]\n"),
                            ("exclusive-selftests", "    name: Exclusive source selftests ${{ matrix.shard }}/2\n")):
            with self.subTest(job=job):
                self.assertIn(anchor, self.TEXT)
                mutant = self.TEXT.replace(anchor, anchor + "    continue-on-error: true\n", 1)
                errors = lane.wiring_errors(mutant)
                self.assertTrue(any(f"job {job} sets" in e for e in errors), errors)

    def test_an_unrelated_step_may_still_use_continue_on_error(self) -> None:
        self.assertIn("continue-on-error: true", self.TEXT)


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
            shard_plan=NO_PLAN,
        )
        self.assertEqual(errors, [])

    def test_the_real_manifest_splits_into_nonempty_parts(self) -> None:
        entries = lane.load_manifest()
        parts = lane.lane_parts(entries)
        self.assertEqual(lane.part_errors(entries), [])
        self.assertEqual(
            sum(len(members) for members in parts.values()), len(entries))
        self.assertEqual(len(parts), lane.EXCLUSIVE_SHARDS + 1)
        for key, members in parts.items():
            self.assertTrue(members, key)
        self.assertFalse(any(lane.is_exclusive(e) for e in parts[lane.SHARED_PART]))
        self.assertEqual(
            {key: sorted(e["name"] for e in members)
             for key, members in parts.items() if key != lane.SHARED_PART},
            {"exclusive-1/2": ["gpu-dpr-runner-selftest",
                               "gpu-first-visible-external-adapter-selftest"],
             "exclusive-2/2": ["agent-capability-manifest-selftest",
                               "agent-capability-rederive-selftest",
                               "gpu-dpr-v2-evidence-selftest"]},
        )

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

    def _run_block(self, stub_exit: int, gate_host: int = 0) -> str:
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
                "print('source-selftests: NOT CHECKED locally: needs-build (needs a configured build)')\n"
                f"raise SystemExit({stub_exit})\n",
                encoding="utf-8",
            )
            log = root / "not-checked.log"
            self.not_checked = ""
            # gates.sh sets gate_host_builds_skipped before this block (1 on a
            # tartci gate host, which refuses these suites).
            script = (f'ROOT={str(root)!r}\nPYTHON={sys.executable!r}\nBASE=origin/main\nfail=0\n'
                      f'gate_host_builds_skipped={gate_host}\n'
                      f'not_checked_log={str(log)!r}\n: > "$not_checked_log"\n'
                      + self._block() + '\necho "fail=$fail"\n')
            env = {k: v for k, v in os.environ.items()
                   if k != "PULP_SKIP_SOURCE_SELFTESTS"
                   and k != "PULP_GATES_BUILD_DIR"}  # the caller's build-dir override must not reach the fixture
            result = subprocess.run(
                ["bash", "-c", script], capture_output=True, text=True, env=env, timeout=60)
            argv = calls.read_text() if calls.exists() else ""
            self.not_checked = log.read_text() if log.exists() else ""
        return result.stdout.strip() + "|" + argv

    def test_a_failing_lane_fails_gates(self) -> None:
        out, argv = self._run_block(1).split("|", 1)
        self.assertEqual(out, "fail=1")
        calls = argv.splitlines()
        self.assertEqual(calls[0], "run --changed-from origin/main --label-base-failures")

    def test_a_gate_host_runs_no_lane_and_records_nothing_here(self) -> None:
        # gates.sh records the refusal itself, once, before this block.
        out, argv = self._run_block(1, gate_host=1).split("|", 1)
        self.assertEqual(out, "fail=0")
        self.assertEqual(argv, "")
        self.assertEqual(self.not_checked, "")

    def test_both_lanes_run_and_a_passing_pair_stays_green(self) -> None:
        out, argv = self._run_block(0).split("|", 1)
        self.assertEqual(out, "fail=0")
        calls = argv.splitlines()
        self.assertEqual(len(calls), 4, calls)
        self.assertEqual(calls[0], "run --changed-from origin/main --label-base-failures")
        self.assertRegex(calls[1], r"^run --workflow \S+/\.github/workflows/workflow-lint\.yml "
                                   r"--changed-from origin/main --label-base-failures$")
        self.assertEqual(calls[2], "run --ctest-label pr-fast --build-dir auto "
                                   "--changed-from origin/main --label-base-failures")
        # Every lane's NOT CHECKED lines reach the summary's list, tagged by lane.
        self.assertEqual(self.not_checked.count("NOT CHECKED locally: needs-build"), 4)
        self.assertIn("[pr-fast] NOT CHECKED", self.not_checked)
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
        # SKIP_REGULAR_EXPRESSION is a skip whatever the exit code or pass regex.
        skip = {"skip_regex": ["thing_skipped="], "pass_regex": ["thing_verified="]}
        self.assertEqual(lane.ctest_verdict(skip, 1, "thing_skipped=no plan"), 0)
        self.assertEqual(lane.ctest_verdict(skip, 1, "thing_failed"), 1)
        self.assertEqual(lane.ctest_verdict(skip, 0, "thing_verified=1"), 0)

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

    def test_any_cmake_input_newer_than_the_configure_makes_it_stale(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            (root / "inspect").mkdir()
            (root / "build").mkdir()
            cmake = root / "inspect" / "CMakeLists.txt"
            cmake.write_text("")
            outside = pathlib.Path(tmp).resolve().parent / "not-in-checkout.cmake"
            (root / "build" / "build.ninja").write_text(
                "build build.ninja /x/y: RERUN_CMAKE | "
                f"{cmake} {root}/build/CMakeCache.txt {outside} || cmake_object_order\n")
            stamp = root / "build" / "CTestTestfile.cmake"
            stamp.write_text("")
            os.utime(cmake, (1000, 1000))
            os.utime(stamp, (2000, 2000))
            self.assertEqual(lane.cmake_inputs(root / "build", root),
                             [cmake, root / "build" / "CMakeCache.txt"])
            self.assertFalse(lane.build_is_stale(root / "build", root))
            # A checkout switch rewrites the file after the configure: the
            # build now describes another tree (not a test manifest at all).
            os.utime(cmake, (3000, 3000))
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
            "set_tests_properties(fast-one PROPERTIES TIMEOUT 42 SKIP_RETURN_CODE 77\n"
            "    # A host without the plan skips.\n"
            '    SKIP_REGULAR_EXPRESSION "fast_skipped=")\n'
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
        self.assertEqual(entries[0]["skip_regex"], ["fast_skipped="])
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


class BuildDirChoiceTests(unittest.TestCase):
    """The pr-fast lane reads the build that is current, not the one named build/."""

    def _build(self, root: pathlib.Path, name: str, generator: str, mtime: float) -> pathlib.Path:
        build = root / name
        build.mkdir()
        (build / "CMakeCache.txt").write_text(f"CMAKE_GENERATOR:INTERNAL={generator}\n")
        stamp = build / "CTestTestfile.cmake"
        stamp.write_text("")
        os.utime(stamp, (mtime, mtime))
        return build

    def _tree(self, root: pathlib.Path) -> None:
        (root / "test" / "cmake").mkdir(parents=True)
        manifest = root / "test" / "cmake" / "x_tests.cmake"
        manifest.write_text("")
        os.utime(manifest, (2000, 2000))

    def test_a_stale_makefiles_build_loses_to_a_current_build_gate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._tree(root)
            # build/ is even the NEWER configure; only its generator disqualifies it.
            self._build(root, "build", "Unix Makefiles", 3500)
            gate = self._build(root, "build-gate", "Ninja", 3000)
            chosen, why = lane.choose_build_dir(root, {})
        self.assertEqual(chosen, gate)
        self.assertIn("build: generator is Unix Makefiles, not Ninja", why)

    def test_a_stale_ninja_build_is_passed_over(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._tree(root)
            self._build(root, "build", "Ninja", 1000)
            chosen, why = lane.choose_build_dir(root, {})
        self.assertIsNone(chosen)
        self.assertIn("configured before its CMake inputs last changed", why)

    def test_the_newest_of_two_current_builds_wins(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._tree(root)
            self._build(root, "build", "Ninja", 3000)
            newer = self._build(root, "build-macos", "Ninja", 4000)
            self.assertEqual(lane.choose_build_dir(root, {})[0], newer)

    def test_the_declared_build_dir_wins(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._tree(root)
            self._build(root, "build-gate", "Ninja", 4000)
            mine = self._build(root, "elsewhere", "Unix Makefiles", 1000)
            env = {lane.GATES_BUILD_DIR_ENV: str(mine)}
            self.assertEqual(lane.choose_build_dir(root, env), (mine, "from PULP_GATES_BUILD_DIR"))

    def test_no_build_says_so(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            chosen, why = lane.choose_build_dir(pathlib.Path(tmp), {})
        self.assertIsNone(chosen)
        self.assertIn("no build*/CMakeCache.txt", why)


class GatesVerdictTests(unittest.TestCase):
    """A NOT CHECKED suite can never read as a plain green verdict."""

    GATES = HERE.parent / "scripts" / "gates.sh"

    def _verdict(self, fail: int, not_checked: str, gate_host: int = 0) -> subprocess.CompletedProcess:
        text = self.GATES.read_text(encoding="utf-8")
        summary = text[text.index("# ── Summary"):]
        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp) / "nc.log"
            log.write_text(not_checked, encoding="utf-8")
            script = (f"fail={fail}\ngate_host_builds_skipped={gate_host}\n"
                      f"not_checked_log={str(log)!r}\n" + summary)
            return subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                                  timeout=60)

    def test_control_nothing_skipped_is_a_plain_pass(self) -> None:
        r = self._verdict(0, "")
        self.assertEqual(r.returncode, 0)
        self.assertIn("gates: ✓ all gates pass", r.stderr)

    def test_diff_coverage_hint_follows_the_gate_host_flag(self) -> None:
        plain = self._verdict(0, "")
        self.assertIn("Diff-coverage NOT run (slow)", plain.stderr)
        refused = self._verdict(0, "[gate-host] NOT CHECKED: suites\n", gate_host=1)
        self.assertIn("refused on this gate host", refused.stderr)
        self.assertNotIn("Diff-coverage NOT run (slow)", refused.stderr)

    def test_not_checked_items_are_listed_and_qualify_the_verdict(self) -> None:
        r = self._verdict(0, "[pr-fast] NOT CHECKED locally: a (x)\n"
                             "[pr-fast] NOT CHECKED locally: b (y)\n")
        self.assertEqual(r.returncode, 0)
        self.assertIn("NOT CHECKED here (2)", r.stderr)
        self.assertIn("[pr-fast] NOT CHECKED locally: b (y)", r.stderr)
        self.assertIn("gates: ✓ PASSED WITH 2 NOT CHECKED", r.stderr)
        self.assertNotIn("all gates pass", r.stderr)

    def test_no_usable_build_is_named_in_the_verdict(self) -> None:
        r = self._verdict(0, "[pr-fast] NO USABLE BUILD: build: generator is Unix Makefiles, "
                             "not Ninja; members that need a configured build are not checked\n")
        self.assertIn("gates: no usable configured build: build: generator is Unix Makefiles", r.stderr)
        self.assertNotIn("all gates pass", r.stderr)

    def test_a_failure_still_fails(self) -> None:
        r = self._verdict(1, "[pr-fast] NOT CHECKED locally: a (x)\n")
        self.assertEqual(r.returncode, 1)
        self.assertIn("one or more gates failed", r.stderr)


class CensusProfileTests(unittest.TestCase):
    """Census checks run only against a build of the scope the census measured."""

    FEATURES = {"PULP_HAS_SKIA": "ON", "PULP_ENABLE_JS": "ON"}

    def _repo(self, root: pathlib.Path) -> None:
        scripts = root / "tools" / "scripts"
        scripts.mkdir(parents=True)
        for name in ("consumption_census.py", "json_schema_lite.py"):
            (scripts / name).write_text((HERE.parent / "scripts" / name).read_text())
        (root / "docs" / "status").mkdir(parents=True)
        key = "darwin-arm64-js+skia"
        (root / lane.CENSUS_FILE).write_text(json.dumps({"profiles": {key: {
            "features": self.FEATURES,
            "build_scope": {"PULP_BUILD_TESTS": "ON", "PULP_BUILD_EXAMPLES": "OFF"}}}}))
        (root / "test" / "cmake").mkdir(parents=True)
        manifest = root / "test" / "cmake" / "x_tests.cmake"
        manifest.write_text("")
        os.utime(manifest, (1000, 1000))

    def _build(self, root: pathlib.Path, name: str, examples: str, mtime: float,
               system: str = "Darwin") -> pathlib.Path:
        build = root / name
        build.mkdir()
        (build / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Ninja\n")
        stamp = build / "CTestTestfile.cmake"
        stamp.write_text("")
        os.utime(stamp, (mtime, mtime))
        (build / lane.CENSUS_FACTS).write_text(json.dumps({
            "system_name": system, "system_processor": "arm64",
            "features": {**self.FEATURES, "PULP_BUILD_TESTS": "ON",
                         "PULP_BUILD_EXAMPLES": examples}}))
        return build

    def test_examples_on_is_named_as_the_difference(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._repo(root)
            build = self._build(root, "build-gate", "ON", 2000)
            profile, diffs = lane.census_mismatch(build, root)
        self.assertEqual(profile, "darwin-arm64-js+skia")
        self.assertEqual(diffs, ["PULP_BUILD_EXAMPLES=ON (profile OFF)"])

    def test_control_a_profile_build_matches(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._repo(root)
            self.assertIsNone(lane.census_mismatch(self._build(root, "b", "OFF", 2000), root))

    def test_an_unrecorded_profile_is_left_to_the_census(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._repo(root)
            build = self._build(root, "b", "ON", 2000, system="Linux")
            self.assertIsNone(lane.census_mismatch(build, root))

    def test_census_tests_are_set_aside_and_the_rest_kept(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._repo(root)
            build = self._build(root, "b", "ON", 2000)
            entries = [
                {"name": "consumption-census-drift", "argv": [
                    "py", "{repo}/tools/scripts/consumption_census.py", "--check",
                    "--build-dir", str(build)]},
                {"name": "consumption-census-schema", "argv": [
                    "py", "{repo}/tools/scripts/consumption_census.py", "--validate-only"]},
                {"name": "other", "argv": ["py", "{repo}/x.py", "--build-dir", str(build)]},
            ]
            aside = lane.set_aside_census_tests(entries, build, root)
        self.assertEqual(list(aside), ["consumption-census-drift"])
        self.assertIn("PULP_BUILD_EXAMPLES=ON (profile OFF)", aside["consumption-census-drift"])
        self.assertEqual([e["name"] for e in entries], ["consumption-census-schema", "other"])

    def test_auto_prefers_the_build_the_census_measured(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            self._repo(root)
            gate = self._build(root, "build-gate", "OFF", 2000)
            self._build(root, "build-examples", "ON", 3000)
            chosen, why = lane.choose_build_dir(root, {})
        self.assertEqual(chosen, gate)
        self.assertIn("matches the census profile", why)


if __name__ == "__main__":
    unittest.main()
