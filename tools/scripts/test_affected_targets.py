#!/usr/bin/env python3
"""Unit and shell-out tests for the build-target projection behind `pulp affected`.

A synthetic CMake file-API reply, Makefile depfiles, and CTest inventory stand
in for a configured build directory, so every rule of the selector is exercised
without CMake: source→target, header→targets through the dependency database,
companion tests by name, `add_dependencies` edges, fixture edges, the empty
diff, and the fallback-to-all threshold.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import changed_surface_inventory as at  # noqa: E402

SCRIPT = SCRIPT_DIR / "affected_targets.py"


def _target(name: str, ttype: str, source_dir: str, sources: list[str],
            deps: list[str] = (), artifacts: list[str] = ()) -> dict:
    return {
        "name": name,
        "type": ttype,
        "source_dir": source_dir,
        "sources": list(sources),
        "deps": list(deps),
        "artifacts": list(artifacts),
    }


GRAPH = [
    _target("pulp-runtime", "STATIC_LIBRARY", "core/runtime", ["core/runtime/src/log.cpp"]),
    _target("pulp-view-core", "STATIC_LIBRARY", "core/view",
            ["core/view/src/widgets.cpp", "core/view/src/knob.cpp"], ["pulp-runtime"]),
    _target("pulp-cli", "EXECUTABLE", "tools/cli", ["tools/cli/cmd_build.cpp"],
            ["pulp-runtime"], ["tools/cli/pulp-cli"]),
    _target("Gain_CLAP", "MODULE_LIBRARY", "examples/gain", ["examples/gain/gain.cpp"],
            ["pulp-view-core"], ["CLAP/Gain.clap/Contents/MacOS/Gain"]),
    _target("pulp-test-widgets", "EXECUTABLE", "test", ["test/test_widgets.cpp"],
            ["pulp-view-core"], ["test/pulp-test-widgets"]),
    _target("pulp-test-widgets-label", "EXECUTABLE", "test", ["test/test_widgets_label.cpp"],
            ["pulp-view-core"], ["test/pulp-test-widgets-label"]),
    _target("pulp-test-widget-bridge", "EXECUTABLE", "test", ["test/test_widget_bridge.cpp"],
            ["pulp-view-core"], ["test/pulp-test-widget-bridge"]),
    _target("pulp-test-knob", "EXECUTABLE", "test", ["test/test_knob.cpp"],
            ["pulp-view-core"], ["test/pulp-test-knob"]),
    _target("pulp-test-runtime", "EXECUTABLE", "test", ["test/test_runtime.cpp"],
            ["pulp-runtime"], ["test/pulp-test-runtime"]),
    _target("pulp-test-cli-shellout", "EXECUTABLE", "test", ["test/test_cli_shellout.cpp"],
            ["pulp-runtime", "pulp-cli"], ["test/pulp-test-cli-shellout"]),
    _target("pulp-test-gain-dlopen", "EXECUTABLE", "test", ["test/test_gain_dlopen.cpp"],
            ["pulp-runtime"], ["test/pulp-test-gain-dlopen"]),
    _target("pulp-example-validation-all", "UTILITY", "examples", [], ["pulp-cli"]),
]


def write_reply(build_dir: Path, source_root: Path, graph: list[dict]) -> None:
    reply = build_dir / at.CODEMODEL_REPLY_RELATIVE
    reply.mkdir(parents=True, exist_ok=True)
    ids = {t["name"]: f"{t['name']}::@id" for t in graph}
    refs = []
    for t in graph:
        json_file = f"target-{t['name']}.json"
        refs.append({"id": ids[t["name"]], "jsonFile": json_file, "name": t["name"]})
        (reply / json_file).write_text(json.dumps({
            "id": ids[t["name"]],
            "name": t["name"],
            "type": t["type"],
            "paths": {"source": t["source_dir"], "build": t["source_dir"]},
            "sources": [{"path": s} for s in t["sources"]],
            "dependencies": [{"id": ids[d]} for d in t["deps"]],
            "artifacts": [{"path": a} for a in t["artifacts"]],
        }))
    (reply / "codemodel-v2.json").write_text(json.dumps({
        "kind": "codemodel",
        "paths": {"source": str(source_root), "build": str(build_dir)},
        "configurations": [{"name": "Release", "targets": refs}],
    }))
    (reply / "index-2026-09-23.json").write_text(json.dumps({
        "objects": [{"kind": "codemodel", "jsonFile": "codemodel-v2.json"}],
        "reply": {"codemodel-v2": {"jsonFile": "codemodel-v2.json"}},
    }))


def write_depfile(build_dir: Path, target: dict, source: str, headers: list[str]) -> None:
    obj_dir = build_dir / target["source_dir"] / "CMakeFiles" / f"{target['name']}.dir"
    obj_dir.mkdir(parents=True, exist_ok=True)
    # Depfile paths are relative to the object's build directory, one level
    # below the build root, so they climb the target depth plus one.
    depth = len(Path(target["source_dir"]).parts)
    up = "/".join([".."] * (depth + 1))
    lines = [f"{target['source_dir']}/CMakeFiles/{target['name']}.dir/{source}.o: \\",
             f"  {up}/{source} \\"]
    lines += [f"  {up}/{h} \\" for h in headers]
    lines.append("  /usr/include/c++/v1/algorithm")
    (obj_dir / f"{Path(source).name}.o.d").write_text("\n".join(lines) + "\n")


def inventory(build_dir: Path, source_root: Path) -> list[at.CTestEntry]:
    def exe(rel: str) -> str:
        return str(build_dir / rel)

    payload = {"tests": [
        {"name": "Knob value clamping", "command": [exe("test/pulp-test-widgets"), "Knob value clamping"]},
        {"name": "Knob renders arcs", "command": [exe("test/pulp-test-widgets"), "Knob renders arcs"]},
        {"name": "Label wraps", "command": [exe("test/pulp-test-widgets-label"), "Label wraps"]},
        {"name": "Bridge binds", "command": [exe("test/pulp-test-widget-bridge"), "Bridge binds"]},
        {"name": "Knob sprite", "command": [exe("test/pulp-test-knob"), "Knob sprite"]},
        {"name": "Runtime log", "command": [exe("test/pulp-test-runtime"), "Runtime log"]},
        {"name": "CLI create", "command": [exe("test/pulp-test-cli-shellout"), "CLI create"]},
        {"name": "gain-dlopen", "command": [exe("test/pulp-test-gain-dlopen")],
         "properties": [{"name": "FIXTURES_REQUIRED", "value": ["gain_bundle"]}]},
        {"name": "gain-bundle-setup", "command": [exe("CLAP/Gain.clap/Contents/MacOS/Gain")],
         "properties": [{"name": "FIXTURES_SETUP", "value": ["gain_bundle"]}]},
        {"name": "policy-selftest",
         "command": ["/usr/bin/python3", str(source_root / "tools/scripts/test_policy.py")]},
    ]}
    return at.projection_ctest_entries(payload["tests"])


class Fixture:
    def __init__(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "src"
        self.build = self.root / "build"
        self.root.mkdir()
        self.build.mkdir()
        write_reply(self.build, self.root, GRAPH)
        self.model = at.load_codemodel_targets(self.build)
        self.inventory = inventory(self.build, self.root)

    def touch(self, rel: str, content: str = "// changed\n") -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def close(self) -> None:
        self.tmp.cleanup()


class SelectorRules(unittest.TestCase):
    def setUp(self) -> None:
        self.fx = Fixture()
        self.addCleanup(self.fx.close)

    def select(self, changed: list[str], deleted: list[str] = (), deps_db=None,
               with_inventory: bool = True, threshold: float = at.DEFAULT_PROJECTION_THRESHOLD) -> at.Selection:
        return at.project_affected(self.fx.model, changed, list(deleted), deps_db,
                         self.fx.inventory if with_inventory else None, threshold)

    def test_codemodel_loads_graph(self) -> None:
        self.assertEqual(self.fx.model.total, len(GRAPH))
        widgets = self.fx.model.targets["pulp-test-widgets"]
        self.assertTrue(widgets.is_test)
        self.assertEqual(widgets.dependencies, ["pulp-view-core"])
        self.assertEqual(widgets.artifacts, [str(self.fx.build / "test/pulp-test-widgets")])
        self.assertTrue(self.fx.model.targets["pulp-view-core"].is_library)

    def test_source_selects_owner_and_companion_tests(self) -> None:
        sel = self.select(["core/view/src/widgets.cpp"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, ["pulp-test-widgets", "pulp-test-widgets-label", "pulp-view-core"])
        self.assertEqual(sel.tests, ["Knob renders arcs", "Knob value clamping", "Label wraps"])
        self.assertIn("3/12", sel.banner)
        self.assertIn("pulp build --all", sel.banner)

    def test_test_source_selects_only_that_program(self) -> None:
        sel = self.select(["test/test_knob.cpp"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, ["pulp-test-knob"])
        self.assertEqual(sel.tests, ["Knob sprite"])

    def test_header_maps_through_dependency_database(self) -> None:
        deps_db = {"core/view/include/pulp/view/knob.hpp": {"pulp-view-core", "pulp-test-knob"}}
        sel = self.select(["core/view/include/pulp/view/knob.hpp"], deps_db=deps_db)
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, ["pulp-test-knob", "pulp-view-core"])

    def test_header_without_database_falls_back_to_directory_owner_and_dependents(self) -> None:
        sel = self.select(["core/runtime/include/pulp/runtime/log.hpp"], deps_db=None,
                          threshold=1.0)
        self.assertEqual(sel.mode, "focused")
        self.assertIn("pulp-runtime", sel.targets)
        self.assertIn("pulp-test-runtime", sel.targets)
        self.assertIn("pulp-cli", sel.targets)
        self.assertIn("pulp-view-core", sel.targets)
        self.assertNotIn("pulp-test-knob", sel.targets)

    def test_header_included_by_nothing_is_inert(self) -> None:
        sel = self.select(["core/view/src/knob_private.hpp"], deps_db={})
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, [])
        self.assertIn("nothing to build", sel.banner)

    def test_executable_change_follows_add_dependencies_to_tests_only(self) -> None:
        sel = self.select(["tools/cli/cmd_build.cpp"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, ["pulp-cli", "pulp-test-cli-shellout"])
        self.assertNotIn("pulp-example-validation-all", sel.targets)
        self.assertEqual(sel.tests, ["CLI create"])

    def test_fixture_edge_pulls_setup_test_and_its_target(self) -> None:
        sel = self.select(["test/test_gain_dlopen.cpp"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, ["Gain_CLAP", "pulp-test-gain-dlopen"])
        self.assertEqual(sel.tests, ["gain-bundle-setup", "gain-dlopen"])

    def test_script_change_selects_test_that_runs_it_without_a_build(self) -> None:
        sel = self.select(["tools/scripts/test_policy.py"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, [])
        self.assertEqual(sel.tests, ["policy-selftest"])
        self.assertEqual(sel.unmapped, ["tools/scripts/test_policy.py"])
        self.assertIn("nothing to build", sel.banner)

    def test_object_library_source_relinks_every_dependent(self) -> None:
        graph = GRAPH + [
            _target("pulp-test-rt-allocation-probe", "OBJECT_LIBRARY", "test",
                    ["test/harness/rt_allocation_probe.cpp"]),
            _target("pulp-test-host", "EXECUTABLE", "test", ["test/test_host.cpp"],
                    ["pulp-runtime", "pulp-test-rt-allocation-probe"],
                    ["test/pulp-test-host"]),
            _target("pulp-test-state", "EXECUTABLE", "test", ["test/test_state.cpp"],
                    ["pulp-runtime", "pulp-test-rt-allocation-probe"],
                    ["test/pulp-test-state"]),
        ]
        write_reply(self.fx.build, self.fx.root, graph)
        model = at.load_codemodel_targets(self.fx.build)
        sel = at.project_affected(model, ["test/harness/rt_allocation_probe.cpp"], [],
                                  None, None, 1.0)
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, ["pulp-test-host", "pulp-test-rt-allocation-probe",
                                       "pulp-test-state"])

    def test_static_library_source_does_not_pull_every_dependent(self) -> None:
        sel = self.select(["core/runtime/src/log.cpp"], threshold=1.0)
        self.assertNotIn("pulp-cli", sel.targets)
        self.assertNotIn("pulp-view-core", sel.targets)

    def test_empty_diff_falls_back_to_all(self) -> None:
        sel = self.select([])
        self.assertEqual(sel.mode, "all")
        self.assertIn("no changes", sel.reason)
        self.assertEqual(sel.targets, [])
        self.assertTrue(sel.banner.startswith("FULL:"))

    def test_docs_only_diff_falls_back_to_all(self) -> None:
        sel = self.select(["docs/reference/cli.md"])
        self.assertEqual(sel.mode, "all")
        self.assertIn("no build targets", sel.reason)

    def test_build_system_change_falls_back_to_all(self) -> None:
        for path in ("CMakeLists.txt", "test/cmake/view_tests.cmake", "tools/cmake/PulpTest.cmake"):
            sel = self.select(["core/view/src/widgets.cpp", path])
            self.assertEqual(sel.mode, "all", path)
            self.assertIn("build system", sel.reason)

    def test_build_system_change_older_than_the_reply_maps_normally(self) -> None:
        sel = at.project_affected(self.fx.model, ["core/view/src/widgets.cpp", "CMakeLists.txt"], [],
                        None, self.fx.inventory, stale_build_system=[])
        self.assertEqual(sel.mode, "focused")
        self.assertIn("pulp-view-core", sel.targets)

    def test_stale_build_system_files_compare_against_the_reply(self) -> None:
        self.fx.touch("CMakeLists.txt", "project(pulp)\n")
        self.fx.touch("test/cmake/view_tests.cmake", "# tests\n")
        reply_time = at.codemodel_reply_mtime(self.fx.build)
        self.assertIsNotNone(reply_time)
        old = reply_time - 100
        new = reply_time + 100
        os.utime(self.fx.root / "CMakeLists.txt", (old, old))
        os.utime(self.fx.root / "test/cmake/view_tests.cmake", (new, new))
        changed = ["CMakeLists.txt", "test/cmake/view_tests.cmake", "core/view/src/widgets.cpp"]
        self.assertEqual(at.stale_build_system_files(self.fx.root, changed, [], reply_time),
                         ["test/cmake/view_tests.cmake"])
        self.assertEqual(at.stale_build_system_files(self.fx.root, changed, ["x.cmake"], reply_time),
                         ["x.cmake", "test/cmake/view_tests.cmake"])
        self.assertEqual(len(at.stale_build_system_files(self.fx.root, changed, [], None)), 2)

    def test_unowned_source_falls_back_to_all(self) -> None:
        sel = self.select(["core/view/src/brand_new.cpp"])
        self.assertEqual(sel.mode, "all")
        self.assertIn("not owned", sel.reason)

    def test_deleted_source_falls_back_to_all(self) -> None:
        sel = self.select(["core/view/src/widgets.cpp"], deleted=["core/view/src/knob.cpp"])
        self.assertEqual(sel.mode, "all")
        self.assertIn("deleted", sel.reason)

    def test_selection_above_threshold_falls_back_to_all(self) -> None:
        changed = ["core/view/src/widgets.cpp", "tools/cli/cmd_build.cpp", "test/test_knob.cpp"]
        wide = self.select(changed, threshold=1.0)
        self.assertEqual(wide.mode, "focused")
        self.assertEqual(len(wide.targets), 6)
        narrow = self.select(changed, threshold=0.4)
        self.assertEqual(narrow.mode, "all")
        self.assertIn("exceed 40%", narrow.reason)

    def test_policy_families_add_their_tests_and_targets(self) -> None:
        families = [{"name": "cli", "paths": ["tools/cli/cmd_*.cpp"],
                     "tests": ["Runtime log", "not-in-inventory"], "build_targets": ["pulp-test-runtime"]}]
        sel = at.project_affected(self.fx.model, ["tools/cli/cmd_build.cpp"], [], None,
                                  self.fx.inventory, families=families)
        self.assertEqual(sel.mode, "focused")
        self.assertIn("pulp-test-runtime", sel.targets)
        self.assertIn("Runtime log", sel.tests)
        self.assertNotIn("not-in-inventory", sel.tests)
        self.assertEqual(at.family_projection(families, ["docs/x.md"]), (set(), set()))

    def test_projection_corpus_applies_the_gate_exclusions(self) -> None:
        raw = [
            {"name": "keep", "command": ["/x"]},
            {"name": "slow one", "command": ["/y"], "properties": [{"name": "LABELS", "value": ["slow"]}]},
            {"name": "AudioWorkgroup probe", "command": ["/z"]},
            {"name": "malformed", "command": ["/w"], "properties": "nope"},
        ]
        self.assertEqual([e.name for e in at.projection_ctest_entries(raw)], ["keep"])

    def test_no_inventory_still_focuses_the_build(self) -> None:
        sel = self.select(["core/view/src/widgets.cpp"], with_inventory=False)
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.tests, [])
        self.assertEqual(sel.total_tests, 0)


class DependencyDatabase(unittest.TestCase):
    def test_ninja_deps_resolve_relative_to_build_dir(self) -> None:
        text = (
            "core/view/CMakeFiles/pulp-view-core.dir/src/widgets.cpp.o: #deps 3, deps mtime 1 (VALID)\n"
            "    ../core/view/src/widgets.cpp\n"
            "    ../core/view/include/pulp/view/widgets.hpp\n"
            "    /usr/include/stdio.h\n"
            "\n"
            "test/CMakeFiles/pulp-test-knob.dir/test_knob.cpp.o: #deps 2, deps mtime 1 (VALID)\n"
            "    ../test/test_knob.cpp\n"
            "    /src/core/view/include/pulp/view/widgets.hpp\n"
            "\n"
        )
        wanted = {"core/view/include/pulp/view/widgets.hpp"}
        owners = at.parse_ninja_deps(text, "/src", "/src/build", wanted)
        self.assertEqual(owners, {"core/view/include/pulp/view/widgets.hpp": {"pulp-view-core", "pulp-test-knob"}})

    def test_makefile_depfiles_resolve_relative_to_object_dir(self) -> None:
        fx = Fixture()
        self.addCleanup(fx.close)
        header = "core/view/include/pulp/view/widgets.hpp"
        write_depfile(fx.build, GRAPH[1], "core/view/src/widgets.cpp", [header])
        write_depfile(fx.build, GRAPH[7], "test/test_knob.cpp", [header])
        write_depfile(fx.build, GRAPH[8], "test/test_runtime.cpp", [])
        owners = at.header_owners(fx.build, fx.model, {header})
        self.assertEqual(owners, {header: {"pulp-view-core", "pulp-test-knob"}})
        self.assertEqual(at.header_owners(fx.build, fx.model, {"core/view/src/nothing.hpp"}), {})

    def test_no_depfiles_means_no_database(self) -> None:
        fx = Fixture()
        self.addCleanup(fx.close)
        self.assertIsNone(at.header_owners(fx.build, fx.model, {"core/view/src/knob.hpp"}))


class CommandLine(unittest.TestCase):
    def run_script(self, fx: Fixture, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--build-dir", str(fx.build), "--source-root", str(fx.root),
             "--no-tests", *args],
            capture_output=True, text=True, check=False,
        )

    def test_json_and_write_dir_for_a_source_edit(self) -> None:
        fx = Fixture()
        self.addCleanup(fx.close)
        fx.touch("core/view/src/widgets.cpp")
        out_dir = fx.build / ".pulp" / "affected"
        proc = self.run_script(fx, "--file", "core/view/src/widgets.cpp", "--json",
                               "--write-dir", str(out_dir))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        payload = json.loads(proc.stdout)
        self.assertEqual(payload["schema"], at.PROJECTION_SCHEMA)
        self.assertEqual(payload["mode"], "focused")
        self.assertEqual(payload["targets"], ["pulp-test-widgets", "pulp-test-widgets-label", "pulp-view-core"])
        self.assertEqual(json.loads((out_dir / "selection.json").read_text()), payload)
        self.assertEqual((out_dir / "targets.txt").read_text().split(),
                         ["pulp-test-widgets", "pulp-test-widgets-label", "pulp-view-core"])
        self.assertTrue((out_dir / "tests.txt").exists())
        self.assertTrue((out_dir / "banner.txt").read_text().startswith("FOCUSED:"))
        self.assertTrue((fx.build / at.CODEMODEL_QUERY_RELATIVE).exists())

    def test_all_mode_removes_target_lists(self) -> None:
        fx = Fixture()
        self.addCleanup(fx.close)
        out_dir = fx.build / ".pulp" / "affected"
        out_dir.mkdir(parents=True)
        (out_dir / "targets.txt").write_text("stale\n")
        (out_dir / "tests.txt").write_text("stale\n")
        fx.touch("CMakeLists.txt")
        newer = at.codemodel_reply_mtime(fx.build) + 100
        os.utime(fx.root / "CMakeLists.txt", (newer, newer))
        proc = self.run_script(fx, "--file", "CMakeLists.txt", "--write-dir", str(out_dir))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(proc.stdout.startswith("FULL:"), proc.stdout)
        self.assertFalse((out_dir / "targets.txt").exists())
        self.assertFalse((out_dir / "tests.txt").exists())
        self.assertEqual(json.loads((out_dir / "selection.json").read_text())["mode"], "all")

    def test_missing_reply_writes_query_and_falls_back(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            build.mkdir()
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), "--build-dir", str(build), "--json"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            payload = json.loads(proc.stdout)
            self.assertEqual(payload["mode"], "all")
            self.assertIn("codemodel reply missing", payload["reason"])
            self.assertTrue((build / at.CODEMODEL_QUERY_RELATIVE).exists())

    def test_git_diff_drives_selection_when_no_files_given(self) -> None:
        fx = Fixture()
        self.addCleanup(fx.close)
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@t")

        def git(*args: str) -> None:
            subprocess.run(["git", *args], cwd=fx.root, check=True, capture_output=True, env=env)

        git("init", "-q", "-b", "main")
        (fx.root / ".gitignore").write_text("build/\n")
        fx.touch("core/view/src/widgets.cpp")
        git("add", ".")
        git("commit", "-q", "-m", "base")
        git("checkout", "-q", "-b", "feature")
        fx.touch("test/test_knob.cpp")
        git("add", "test/test_knob.cpp")
        git("commit", "-q", "-m", "knob")
        fx.touch("core/view/src/widgets.cpp", "// edited after the branch point\n")
        changed, deleted, warning = at.changed_files(fx.root, "main")
        self.assertEqual(changed, ["core/view/src/widgets.cpp", "test/test_knob.cpp"])
        self.assertEqual(deleted, [])
        self.assertIsNone(warning)
        proc = self.run_script(fx, "--base", "main", "--json")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        payload = json.loads(proc.stdout)
        self.assertEqual(payload["mode"], "focused")
        self.assertEqual(payload["targets"],
                         ["pulp-test-knob", "pulp-test-widgets", "pulp-test-widgets-label", "pulp-view-core"])


if __name__ == "__main__":
    unittest.main()
