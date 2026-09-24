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
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "scripts"))

import ctest_gate_args  # noqa: E402
import protected_merge_receipt  # noqa: E402
import source_selftests as lane  # noqa: E402

REPO = pathlib.Path("/src/pulp")
BUILD = REPO / "build"
GATE = {"merge_group": "validation|slow|source-selftest"}
WIRED = "run: python3 tools/ci/source_selftests.py run --min-count 150\n"


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
        self.assertGreaterEqual(len(entries), 150)
        for item in entries:
            argv = item["argv"]
            if argv[:2] == ["-m", "unittest"]:
                cwd = pathlib.Path(lane.expand(item["cwd"], lane.REPO_ROOT))
                script = cwd / f"{argv[2]}.py"
            else:
                script = pathlib.Path(lane.expand(argv[0], lane.REPO_ROOT))
            self.assertTrue(script.is_file(), item["name"])

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


if __name__ == "__main__":
    unittest.main()
