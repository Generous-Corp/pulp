#!/usr/bin/env python3
"""Contract for the configuration-neutral ctest registration projection."""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import changed_surface_inventory as inventory


def payload(tests: list[dict], files: list[str] | None = None) -> dict:
    """A ctest json-v1 document; each test's `registered` index names its file."""
    nodes = [{"file": index} for index in range(len(files or []))]
    out = []
    for test in tests:
        test = dict(test)
        registered = test.pop("registered", None)
        if registered is not None:
            test["backtrace"] = registered
        out.append(test)
    return {"tests": out, "backtraceGraph": {"nodes": nodes, "files": files or []}}


class ProjectionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def tree(self, name: str, build_type: str) -> Path:
        # Each configuration is its own checkout with its build inside, as on
        # the gate VM and the lane host.
        build = Path(self.tmp.name) / name / "build"
        build.mkdir(parents=True)
        (build / "CMakeCache.txt").write_text(f"CMAKE_BUILD_TYPE:STRING={build_type}\n")
        return build

    def lane_tests(self, build: Path, python: str, build_type: str, examples: bool) -> list[dict]:
        src, bld = str(build.parent), str(build)
        tests = [
            {"name": "policy-selftest",
             "command": [python, f"{src}/tools/scripts/test_policy.py", "--config", build_type],
             "properties": [{"name": "WORKING_DIRECTORY", "value": bld},
                            {"name": "LABELS", "value": ["source-selftest"]}],
             "registered": 0},
            {"name": "emit-drift",
             "command": ["/usr/bin/python3", f"{src}/tools/scripts/drift.py", "--emit-cmd",
                         f"{python} {src}/core/emit.py", f"-DPULP_PARENT_BUILD_TYPE={build_type}"],
             "properties": [{"name": "ENVIRONMENT", "value": [f"PULP_HOME={bld}/test-tmp/home"]}],
             "registered": 0},
            {"name": "widget renders", "command": [f"{bld}/test/pulp-test-widgets", "widget renders"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": f"{bld}/test"}], "registered": 1},
            {"name": "widget resizes", "command": [f"{bld}/test/pulp-test-widgets", "widget resizes"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": f"{bld}/test"}], "registered": 1},
        ]
        if examples:
            tests += [
                {"name": "gain processes", "command": [f"{bld}/examples/gain/pulp-gain-test", "gain"],
                 "properties": [{"name": "WORKING_DIRECTORY", "value": f"{bld}/examples/gain"}]},
                {"name": "plugin-lab-roundtrip",
                 "command": ["cmake", f"-DEFFECT={bld}/CLAP/PulpGain.clap", "-P", f"{src}/test/x.cmake"],
                 "properties": [], "registered": 0},
            ]
        return tests

    def project(self, build: Path, tests: list[dict]) -> dict:
        files = [str(build.parent / "test" / "cmake" / "tests.cmake"),
                 str(build / "test" / "pulp-test-widgets_include-1.cmake")]
        return inventory.project_registrations(payload(tests, files), build.parent, build)

    def test_gate_and_lane_configurations_project_equal(self) -> None:
        gate = self.tree("build-gate", "Release")
        lane = self.tree("build-lane", "Debug")
        a = self.project(gate, self.lane_tests(gate, "/usr/local/bin/python3.13", "Release", False))
        b = self.project(lane, self.lane_tests(lane, "/opt/homebrew/bin/python3.14", "Debug", True))
        self.assertEqual(a["digest"], b["digest"], (a["rows"], b["rows"]))
        self.assertEqual(a["row_count"], 3)  # two registrations + one executable row
        self.assertTrue(a["recordable"])
        self.assertEqual(b["config_gated"], ["plugin-lab-roundtrip"])
        rendered = str(a["rows"])
        for token in ("${CMAKE_SOURCE_DIR}", "${CMAKE_BINARY_DIR}", "${CMAKE_BUILD_TYPE}", "${PYTHON}"):
            self.assertIn(token, rendered)
        self.assertNotIn(str(gate), rendered)

    def test_each_normalization_is_load_bearing(self) -> None:
        # Equal digests prove nothing unless the raw listings really differ
        # and each normalization is what makes them meet: remove one and the
        # gate and lane projections must part again.
        gate = self.tree("build-gate", "Release")
        lane = self.tree("build-lane", "Debug")
        gate_tests = self.lane_tests(gate, "/usr/local/bin/python3.13", "Release", False)
        lane_tests = self.lane_tests(lane, "/opt/homebrew/bin/python3.14", "Debug", True)
        lane_shared = {t["name"]: t for t in lane_tests}
        for test in gate_tests:
            with self.subTest(raw=test["name"]):
                self.assertNotEqual(test["command"], lane_shared[test["name"]]["command"])
        self.assertEqual(self.project(gate, gate_tests)["digest"],
                         self.project(lane, lane_tests)["digest"])
        never = re.compile(r"(?!x)x")
        for label, patch in (
            ("python path", mock.patch.object(inventory, "PYTHON_PATH", never)),
            ("build type", mock.patch.object(inventory, "_cache_value", lambda *_: "")),
        ):
            with self.subTest(without=label), patch:
                self.assertNotEqual(self.project(gate, gate_tests)["digest"],
                                    self.project(lane, lane_tests)["digest"])
        with self.subTest(without="source root"):
            elsewhere = Path(self.tmp.name) / "elsewhere"
            unrooted = [inventory.project_registrations(
                payload(tests, [str(build.parent / "test" / "cmake" / "tests.cmake")]), elsewhere, build)
                for build, tests in ((gate, gate_tests), (lane, lane_tests))]
            self.assertNotEqual(unrooted[0]["digest"], unrooted[1]["digest"])

    def test_a_real_difference_survives_the_projection(self) -> None:
        build = self.tree("build", "Release")
        base = self.lane_tests(build, "python3", "Release", False)
        changed = [dict(t) for t in base]
        changed[0] = dict(changed[0], command=changed[0]["command"] + ["--strict"])
        self.assertNotEqual(self.project(build, base)["digest"], self.project(build, changed)["digest"])
        dropped = base[1:]
        self.assertNotEqual(self.project(build, base)["digest"], self.project(build, dropped)["digest"])

    def test_a_test_named_like_a_build_type_keeps_its_name(self) -> None:
        build = self.tree("build", "Release")
        rows = self.project(build, [{"name": "Release", "command": ["python3", "x.py", "Release notes"],
                                     "properties": [], "registered": 0}])["rows"]
        self.assertEqual(rows[0]["row"]["name"], "Release")
        self.assertEqual(rows[0]["row"]["command"], ["${PYTHON}", "x.py", "Release notes"])

    def test_unbuilt_registrations_are_incomplete_and_never_recordable(self) -> None:
        build = self.tree("build", "Release")
        placeholder = {"name": "pulp-test-widgets_NOT_BUILT-1a2b3c4", "command": [],
                       "properties": [{"name": "WORKING_DIRECTORY", "value": f"{build}/test"}]}
        empty = {"name": "cli-help", "command": [""], "properties": [], "registered": 0}
        for tests in ([placeholder], [empty]):
            with self.subTest(test=tests[0]["name"]):
                projected = self.project(build, tests)
                self.assertFalse(projected["recordable"])
                self.assertEqual(projected["incomplete"], [tests[0]["name"]])


if __name__ == "__main__":
    unittest.main()
