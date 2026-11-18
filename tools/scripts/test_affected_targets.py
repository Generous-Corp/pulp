#!/usr/bin/env python3
"""Unit and shell-out tests for the build-target projection behind `pulp affected`.

A synthetic CMake file-API reply, Makefile depfiles, and CTest inventory stand
in for a configured build directory, so every rule of the selector is exercised
without CMake: source→target, header→targets through the dependency database,
companion tests by name, `add_dependencies` edges, fixture edges, the empty
diff, and the fallback-to-all threshold. Each fixture is written to its own
temporary directory, so cases share no state.
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


def _stack(source_root: Path, files: list[str]) -> dict:
    """A CMake backtraceGraph for one call stack, innermost file first."""
    nodes = [{"file": i, **({"parent": i + 1} if i + 1 < len(files) else {})}
             for i in range(len(files))]
    return {"files": [str(source_root / f) for f in files], "nodes": nodes}


# The file API records a target's call-stack files relative to the source root.
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
            **({"backtrace": 0, "backtraceGraph": _stack(Path(), t["defined_in"])}
               if t.get("defined_in") else {}),
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


class CommandPathReferences(unittest.TestCase):
    """A test names a build output in more forms than a bare argument; each
    must tie the test to the target that produces it."""

    def setUp(self) -> None:
        self.fx = Fixture()
        self.addCleanup(self.fx.close)
        build, root = self.fx.build, self.fx.root
        extra = [
            {"name": "cli-help", "command": ["/usr/bin/cmake", f"-DCLI={build}/tools/cli/pulp-cli",
                                             "-P", str(root / "test/cmake/run_cli.cmake")]},
            {"name": "cli-typed", "command": ["/usr/bin/cmake",
                                              f"-DPULP_CLI:FILEPATH={build}/tools/cli/pulp-cli"]},
            {"name": "gain-validator", "command": ["/opt/pluginval", "--validate",
                                                   f"{build}/CLAP/Gain.clap"]},
            {"name": "gain-flag", "command": ["/usr/bin/probe",
                                              f"--plugin={build}/CLAP/Gain.clap"]},
            {"name": "gain-image-list", "command": [
                "/usr/bin/cmake",
                f"-DIMAGES={build}/CLAP/Gain.clap/Contents/MacOS/Gain|{build}/test/pulp-test-knob"]},
            {"name": "env-target-dir", "command": ["/usr/bin/cmake", "-E", "env",
                                                   f"PULP_CLI_PATH={build}/tools/cli/pulp-cli",
                                                   "/usr/bin/true"]},
            # Negative control: the build directory itself is no target's output.
            {"name": "whole-build-reader", "command": ["/usr/bin/cmake", f"-DPULP_BUILD_DIR={build}",
                                                       "-P", str(root / "test/cmake/scan.cmake")]},
        ]
        self.inventory = self.fx.inventory + at.projection_ctest_entries(extra)

    def select(self, changed: list[str]) -> at.Selection:
        return at.project_affected(self.fx.model, changed, [], None, self.inventory, 1.0)

    def test_assigned_values_and_lists_are_path_references(self) -> None:
        refs = at.command_path_refs(["cmake", "-DA=/b/x", "--flag=/b/y", "NAME=/b/z",
                                     "-DL=/b/p|/b/q;/b/r", "-DT:FILEPATH=/b/t"])
        for path in ("/b/x", "/b/y", "/b/z", "/b/p", "/b/q", "/b/r", "/b/t"):
            self.assertIn(path, refs)

    def test_a_cli_passed_as_a_cmake_define_is_selected_by_its_source(self) -> None:
        sel = self.select(["tools/cli/cmd_build.cpp"])
        for name in ("cli-help", "cli-typed", "env-target-dir"):
            self.assertIn(name, sel.tests)

    def test_a_bundle_directory_or_a_listed_image_selects_its_plugin_tests(self) -> None:
        sel = self.select(["examples/gain/gain.cpp"])
        for name in ("gain-validator", "gain-flag", "gain-image-list"):
            self.assertIn(name, sel.tests)

    def test_control_the_build_directory_reaches_no_target(self) -> None:
        for changed in (["tools/cli/cmd_build.cpp"], ["examples/gain/gain.cpp"],
                        ["core/view/src/widgets.cpp"]):
            with self.subTest(changed=changed):
                self.assertNotIn("whole-build-reader", self.select(changed).tests)

    def test_control_an_unrelated_source_reaches_none_of_them(self) -> None:
        sel = self.select(["core/runtime/src/log.cpp"])
        for name in ("cli-help", "gain-validator", "gain-flag", "gain-image-list"):
            self.assertNotIn(name, sel.tests)

    def test_a_source_file_passed_as_a_define_selects_the_test(self) -> None:
        extra = at.projection_ctest_entries([{
            "name": "script-define", "command": [
                "/usr/bin/cmake", f"-DMANIFEST={self.fx.root}/tools/ci/guard.json",
                "-P", str(self.fx.root / "test/cmake/run.cmake")]}])
        sel = at.project_affected(self.fx.model, ["tools/ci/guard.json"], [], None,
                                  self.inventory + extra, 1.0)
        self.assertIn("script-define", sel.tests)


class DeclaredTargets(unittest.TestCase):
    """A test that reads what CMake records about a target declares it."""

    def setUp(self) -> None:
        self.fx = Fixture()
        self.addCleanup(self.fx.close)
        self.inventory = self.fx.inventory + at.projection_ctest_entries([
            {"name": "view-link-closure", "command": [
                "/usr/bin/cmake", "-P", str(self.fx.build / "core/view/closure.cmake")],
             "properties": [{"name": "PULP_SELECT_WITH_TARGETS",
                             "value": ["pulp-view-core", "pulp-runtime"]}]},
            {"name": "undeclared-closure", "command": [
                "/usr/bin/cmake", "-P", str(self.fx.build / "core/view/closure.cmake")]},
        ])

    def select(self, changed: list[str]) -> at.Selection:
        return at.project_affected(self.fx.model, changed, [], None, self.inventory, 1.0)

    def test_an_edit_to_a_declared_target_selects_the_test(self) -> None:
        for changed in (["core/view/src/knob.cpp"], ["core/runtime/src/log.cpp"]):
            with self.subTest(changed=changed):
                self.assertIn("view-link-closure", self.select(changed).tests)

    def test_control_without_the_declaration_nothing_selects_it(self) -> None:
        self.assertNotIn("undeclared-closure", self.select(["core/view/src/knob.cpp"]).tests)

    def test_control_an_undeclared_target_does_not_select_it(self) -> None:
        self.assertNotIn("view-link-closure", self.select(["tools/cli/cmd_build.cpp"]).tests)

    def test_a_semicolon_string_value_is_a_list(self) -> None:
        entry = at.CTestEntry("t", [], {"PULP_SELECT_WITH_TARGETS": "a;b"})
        self.assertEqual(at.declared_targets(entry), {"a", "b"})


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


FRAGMENT_STACK = ["test/cmake/{}.cmake", "test/CMakeLists.txt"]


class FragmentFixture:
    """Two test programs, each defined and registered by its own test/cmake file."""

    def __init__(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "src"
        self.build = self.root / "build"
        self.build.mkdir(parents=True)
        graph = [
            _target("pulp-runtime", "STATIC_LIBRARY", "core/runtime", ["core/runtime/src/log.cpp"]),
            _target("pulp-test-knob", "EXECUTABLE", "test", ["test/test_knob.cpp"],
                    ["pulp-runtime"], ["test/pulp-test-knob"]),
            _target("pulp-test-label", "EXECUTABLE", "test", ["test/test_label.cpp"],
                    ["pulp-runtime"], ["test/pulp-test-label"]),
            _target("pulp-fixture-lib", "STATIC_LIBRARY", "test", ["test/harness/lib.cpp"]),
            _target("pulp-test-uses-lib", "EXECUTABLE", "test", ["test/test_uses_lib.cpp"],
                    ["pulp-fixture-lib"], ["test/pulp-test-uses-lib"]),
        ]
        for t in graph:
            if t["name"] == "pulp-test-knob":
                t["defined_in"] = [f.format("knob_tests") for f in FRAGMENT_STACK]
            elif t["name"] == "pulp-test-label":
                t["defined_in"] = [f.format("label_tests") for f in FRAGMENT_STACK]
            elif t["name"] == "pulp-fixture-lib":
                t["defined_in"] = [f.format("lib_tests") for f in FRAGMENT_STACK]
        write_reply(self.build, self.root, graph)
        self.model = at.load_codemodel_targets(self.build)

        def exe(rel: str) -> str:
            return str(self.build / rel)

        nodes, files = [], []
        for owner in ("knob_tests", "label_tests", "lib_tests"):
            stack = _stack(self.root, [f.format(owner) for f in FRAGMENT_STACK])
            base = len(files)
            files += stack["files"]
            nodes += [{"file": base + n["file"], **({"parent": len(nodes) + 1}
                                                    if "parent" in n else {})}
                      for n in stack["nodes"]]
        self.tests = [
            {"name": "Knob sprite", "backtrace": 0,
             "command": [exe("test/pulp-test-knob"), "Knob sprite"]},
            {"name": "Label wraps", "backtrace": 2,
             "command": [exe("test/pulp-test-label"), "Label wraps"]},
            {"name": "uses-lib", "backtrace": 4,
             "command": [exe("test/pulp-test-uses-lib")]},
            {"name": "knob-script", "command": [
                "cmake", f"-DCHECK={self.root / 'test/cmake/check_knob.cmake'}", "-P",
                str(self.root / "test/cmake/check_knob.cmake")]},
        ]
        # Each test stack is two nodes: the file that called add_test, then its includer.
        self.inventory = at.projection_ctest_entries(
            self.tests, {"files": files, "nodes": nodes})
        for rel, text in {
            "test/cmake/knob_tests.cmake": "add_executable(pulp-test-knob test/test_knob.cpp)\n"
                                           "add_test(NAME knob COMMAND pulp-test-knob)\n",
            "test/cmake/label_tests.cmake": "add_executable(pulp-test-label test/test_label.cpp)\n",
            "test/cmake/lib_tests.cmake": "add_library(pulp-fixture-lib STATIC test/harness/lib.cpp)\n",
            "test/cmake/check_knob.cmake": "message(STATUS ok)\n",
            "test/CMakeLists.txt": "include(cmake/knob_tests.cmake)\n",
        }.items():
            self.touch(rel, text)

    def touch(self, rel: str, content: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    @staticmethod
    def stale_files(changed: list[str], deleted: list[str], stale: tuple[str, ...]) -> list[str]:
        """The build files a real staleness check reports when the codemodel was
        configured after every fragment: deleted ones, non-fragment ones, and
        whatever the test declares stale."""
        return [f for f in list(changed) + list(deleted) if at.is_build_system_file(f)
                and (f in deleted or f in stale or not at.REGISTRATION_FRAGMENT_RE.match(f))]

    def select(self, changed: list[str], deleted: list[str] = (),
               base_texts: dict[str, str] | None = None,
               stale: tuple[str, ...] = ()) -> at.Selection:
        kwargs = {"base_texts": base_texts} if base_texts is not None else {}
        return at.project_affected(self.model, changed, list(deleted), None, self.inventory, 1.0,
                                   stale_build_system=self.stale_files(changed, deleted, stale),
                                   **kwargs)


class RegistrationFragments(unittest.TestCase):
    def setUp(self) -> None:
        self.fx = FragmentFixture()
        self.addCleanup(self.fx.tmp.cleanup)

    def test_a_fragment_selects_only_its_own_tests_and_targets(self) -> None:
        sel = self.fx.select(["test/cmake/knob_tests.cmake"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.tests, ["Knob sprite"])
        self.assertEqual(sel.targets, ["pulp-test-knob"])

    def test_a_target_defined_in_a_fragment_selects_what_links_it(self) -> None:
        sel = self.fx.select(["test/cmake/lib_tests.cmake"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.targets, ["pulp-fixture-lib", "pulp-test-uses-lib"])
        self.assertEqual(sel.tests, ["uses-lib"])

    def test_a_cmake_script_selects_the_tests_that_run_it(self) -> None:
        sel = self.fx.select(["test/cmake/check_knob.cmake"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.tests, ["knob-script"])

    def test_a_fragment_and_a_source_select_both(self) -> None:
        sel = self.fx.select(["test/cmake/label_tests.cmake", "test/test_knob.cpp"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.tests, ["Knob sprite", "Label wraps"])

    def test_shared_and_unknown_cmake_files_stay_global(self) -> None:
        self.fx.touch("test/cmake/other.cmake", "message(STATUS x)\n")
        for path in ("test/CMakeLists.txt", "test/cmake/fragments.cmake", "test/cmake/other.cmake",
                     "tools/cmake/PulpTest.cmake", "CMakeLists.txt"):
            sel = self.fx.select(["test/cmake/knob_tests.cmake", path])
            self.assertEqual(sel.mode, "all", path)
            self.assertIn("build system", sel.reason)

    def test_a_deleted_fragment_stays_global(self) -> None:
        sel = self.fx.select(["test/cmake/knob_tests.cmake"], deleted=["test/cmake/label_tests.cmake"])
        self.assertEqual(sel.mode, "all")

    def test_a_fragment_other_files_depend_on_stays_global(self) -> None:
        cases = {
            "function": "function(knob_helper)\nendfunction()\n",
            "variable": "set(KNOB_FLAGS -DX)\n",
            "cache": "set(KNOB_OPT ON CACHE BOOL \"\")\n",
            "directory": "include_directories(test/include)\n",
            "option": "option(KNOB_WIDE \"\" OFF)\n",
        }
        self.fx.touch("test/cmake/label_tests.cmake", "knob_helper()\nmessage(${KNOB_FLAGS})\n")
        for name, text in cases.items():
            self.fx.touch("test/cmake/knob_tests.cmake", text)
            sel = self.fx.select(["test/cmake/knob_tests.cmake"])
            self.assertEqual(sel.mode, "all", name)

    def test_cross_target_mutations_and_includes_stay_global(self) -> None:
        cases = {
            "target": "target_compile_definitions(pulp-test-label PRIVATE FLAG)\n",
            "test-properties": "set_tests_properties(Label wraps PROPERTIES TIMEOUT 5)\n",
            "target-property": "set_property(TARGET pulp-test-label PROPERTY FOO bar)\n",
            "include": "include(shared-options.cmake)\n",
            "unknown": "custom-project-command.with-dot()\n",
        }
        for name, text in cases.items():
            self.fx.touch("test/cmake/knob_tests.cmake", text)
            sel = self.fx.select(["test/cmake/knob_tests.cmake"])
            self.assertEqual(sel.mode, "all", name)

    def test_scoped_set_and_unset_stay_global(self) -> None:
        for name, text in {
            "set-parent": "set(KNOB_FLAGS -DX PARENT_SCOPE)\n",
            "set-environment": "set(ENV{KNOB_FLAGS} -DX)\n",
            "set-cache": "set(KNOB_FLAGS -DX CACHE STRING \"\")\n",
            "unset-parent": "unset(KNOB_FLAGS PARENT_SCOPE)\n",
            "unset-environment": "unset(ENV{KNOB_FLAGS})\n",
        }.items():
            self.fx.touch("test/cmake/knob_tests.cmake", text)
            sel = self.fx.select(["test/cmake/knob_tests.cmake"])
            self.assertEqual(sel.mode, "all", name)

    def test_same_line_command_stream_stays_global(self) -> None:
        for name, text in {
            "include": "message(STATUS local) include(shared-options.cmake)\n",
            "target": "message(STATUS local) target_compile_definitions(pulp-test-label PRIVATE FLAG)\n",
            "unknown": "message(STATUS local) custom-project-command()\n",
        }.items():
            self.fx.touch("test/cmake/knob_tests.cmake", text)
            sel = self.fx.select(["test/cmake/knob_tests.cmake"])
            self.assertEqual(sel.mode, "all", name)

    def _assert_all(self, cases: dict[str, str]) -> None:
        for name, text in cases.items():
            self.fx.touch("test/cmake/knob_tests.cmake", text)
            sel = self.fx.select(["test/cmake/knob_tests.cmake"])
            self.assertEqual(sel.mode, "all", name)

    def test_commands_that_rewrite_shared_variables_or_policy_stay_global(self) -> None:
        self.fx.touch("test/cmake/label_tests.cmake", "message(${SHARED_FLAGS})\n")
        self._assert_all({
            "list-append": "list(APPEND SHARED_FLAGS -DX)\n",
            "list-length": "list(LENGTH SHARED_FLAGS n)\n",
            "parse-arguments": "cmake_parse_arguments(KNOB \"\" \"\" \"\" ${ARGN})\n",
            "minimum-required": "cmake_minimum_required(VERSION 3.20)\n",
        })

    def test_unset_of_a_variable_another_file_reads_stays_global(self) -> None:
        self.fx.touch("test/cmake/label_tests.cmake", "message(${KNOB_FLAGS})\n")
        self._assert_all({
            "unset": "unset(KNOB_FLAGS)\n",
            "unset-quoted": "unset(\"KNOB_FLAGS\")\n",
        })
        self.fx.touch("test/cmake/knob_tests.cmake",
                      "add_executable(pulp-test-knob test/test_knob.cpp)\nunset(KNOB_UNREAD)\n")
        self.assertEqual(self.fx.select(["test/cmake/knob_tests.cmake"]).mode, "all")

    def test_comment_markers_inside_arguments_do_not_hide_commands(self) -> None:
        self._assert_all({
            "quoted": "message(\"URL #x\")\ninclude(shared-options.cmake)\n",
            "bracket": "message([[URL #x]])\ninclude(shared-options.cmake)\n",
            "bracket-level": "message([==[a ]] #x]==])\ninclude(shared-options.cmake)\n",
            "apostrophe": "message('URL #x')\ninclude(shared-options.cmake)\n",
            "balanced-after-comment-strip":
                "message([[a #]])\ninclude(shared-options.cmake)\nmessage([[b]])\n",
            "unterminated": "message(\"URL #x)\ninclude(shared-options.cmake)\n",
        })

    def test_real_comments_are_still_ignored(self) -> None:
        self.fx.touch(
            "test/cmake/knob_tests.cmake",
            "# include(shared-options.cmake)\n"
            "#[[ target_compile_definitions(pulp-test-label PRIVATE X) ]]\n"
            "add_executable(pulp-test-knob test/test_knob.cpp) # include(more.cmake)\n"
            "add_test(NAME knob COMMAND pulp-test-knob)\n")
        sel = self.fx.select(["test/cmake/knob_tests.cmake"])
        self.assertEqual(sel.mode, "focused")
        self.assertEqual(sel.tests, ["Knob sprite"])

    def test_quoted_arguments_are_read_without_their_delimiters(self) -> None:
        self.fx.touch("test/cmake/label_tests.cmake", "message(${KNOB_FLAGS})\n")
        self._assert_all({
            "quoted-cmake-variable": "set(\"CMAKE_CXX_FLAGS\" -DX)\n",
            "quoted-environment": "set(\"ENV{KNOB_FLAGS}\" -DX)\n",
            "quoted-parent-scope": "set(KNOB_OUT -DX \"PARENT_SCOPE\")\n",
            "quoted-cache": "set(KNOB_OUT -DX \"CACHE\" STRING \"\")\n",
            "quoted-shared-name": "set(\"KNOB_FLAGS\" -DX)\n",
            "single-quoted": "set('CMAKE_CXX_FLAGS' -DX)\n",
            "dynamic-name": "set(${KNOB_NAME} -DX)\n",
        })
        self.fx.touch("test/cmake/label_tests.cmake", "message(\"${KNOB_FLAGS}\")\n")
        self._assert_all({"set-read-in-quotes": "set(KNOB_FLAGS -DX)\n"})

    def test_namespaced_and_unparseable_commands_stay_global(self) -> None:
        self._assert_all({
            "namespaced": "project::mutate(pulp-test-label)\n",
            "namespaced-after-local": "message(STATUS ok)\nproject::mutate()\n",
            "stray-token": "message(STATUS ok)\n)\n",
            "no-arguments": "message\n",
        })

    def test_removed_shared_assignment_stays_global(self) -> None:
        self.fx.touch("test/cmake/label_tests.cmake", "message(${KNOB_FLAGS.with-dot})\n")
        self.fx.touch("test/cmake/knob_tests.cmake",
                      "add_executable(pulp-test-knob test/test_knob.cpp)\n")
        base_texts = {
            "test/cmake/knob_tests.cmake":
                "set(KNOB_FLAGS.with-dot -DX)\n"
                "add_executable(pulp-test-knob test/test_knob.cpp)\n",
            "test/cmake/label_tests.cmake": "message(${KNOB_FLAGS.with-dot})\n",
        }
        sel = self.fx.select(["test/cmake/knob_tests.cmake"], base_texts=base_texts)
        self.assertEqual(sel.mode, "all")

    def test_dual_use_script_with_configure_side_effect_stays_global(self) -> None:
        self.fx.touch("test/cmake/check_knob.cmake",
                      "set(CMAKE_CXX_FLAGS \"${CMAKE_CXX_FLAGS} -DGLOBAL\")\n"
                      "message(STATUS ok)\n")
        sel = self.fx.select(["test/cmake/check_knob.cmake"])
        self.assertEqual(sel.mode, "all")

    def test_a_duplicate_test_name_selects_every_same_name_owner(self) -> None:
        duplicate = at.CTestEntry(
            "Knob sprite", [str(self.fx.build / "test/pulp-test-label"), "Knob sprite"], {},
            frozenset({str(self.fx.root / "test/cmake/label_tests.cmake")}))
        sel = at.project_affected(self.fx.model, ["test/cmake/knob_tests.cmake"], [], None,
                                  self.fx.inventory + [duplicate], 1.0, [])
        self.assertEqual(sel.mode, "focused")
        self.assertIn("pulp-test-knob", sel.targets)
        self.assertIn("pulp-test-label", sel.targets)

    def test_a_script_test_duplicate_selects_the_same_name_owner(self) -> None:
        duplicate = at.CTestEntry(
            "knob-script", [str(self.fx.build / "test/pulp-test-label")], {},
            frozenset({str(self.fx.root / "test/cmake/label_tests.cmake")}))
        sel = at.project_affected(self.fx.model, ["test/cmake/check_knob.cmake"], [], None,
                                  self.fx.inventory + [duplicate], 1.0, [])
        self.assertEqual(sel.mode, "focused")
        self.assertIn("pulp-test-label", sel.targets)

    def test_a_foreach_loop_variable_stays_global(self) -> None:
        # A foreach variable keeps its last value in the including scope after endforeach.
        self.fx.touch("test/cmake/label_tests.cmake", "message(${item})\n")
        self._assert_all({
            "foreach": "foreach(item a b)\n  message(${item})\nendforeach()\n",
            "foreach-in-lists": "foreach(item IN LISTS names)\nendforeach()\n",
            "while": "while(FALSE)\nendwhile()\n",
            "if": "if(TRUE)\n  message(STATUS x)\nendif()\n",
        })

    def test_only_pure_registration_commands_stay_local(self) -> None:
        self.fx.touch(
            "test/cmake/knob_tests.cmake",
            "add_executable(pulp-test-knob test/test_knob.cpp)\n"
            "add_test(NAME knob COMMAND pulp-test-knob)\n"
            "set_tests_properties(knob PROPERTIES TIMEOUT 5)\n")
        self.assertEqual(self.fx.select(["test/cmake/knob_tests.cmake"]).mode, "focused")
        self._assert_all({
            "foreign-test-properties": "set_tests_properties(\"Label wraps\" PROPERTIES TIMEOUT 5)\n",
            "empty-test-list": "set_tests_properties(PROPERTIES TIMEOUT 5)\n",
            "mixed-owned-and-foreign":
                "add_test(NAME knob COMMAND x)\nset_tests_properties(knob other PROPERTIES TIMEOUT 5)\n",
            "variable": "set(LOCAL_ONLY x)\n",
            "function": "function(f)\nendfunction()\n",
            "include": "include(x.cmake)\n",
            "list": "list(APPEND X a)\n",
            "unset": "unset(X)\n",
            "unlisted-project-command": "pulp_test_data(knob data)\n",
            "namespaced": "project::register()\n",
        })

    def _assert_local(self, cases: dict[str, str]) -> None:
        for name, text in cases.items():
            self.fx.touch("test/cmake/knob_tests.cmake", text)
            sel = self.fx.select(["test/cmake/knob_tests.cmake"])
            self.assertEqual(sel.mode, "focused", name)

    def test_only_safe_test_properties_stay_local(self) -> None:
        reg = "add_executable(pulp-test-knob test/test_knob.cpp)\nadd_test(NAME knob COMMAND pulp-test-knob)\n"
        safe = ("LABELS a TIMEOUT 5 ENVIRONMENT A=1 WORKING_DIRECTORY /x SKIP_RETURN_CODE 4 "
                "WILL_FAIL TRUE PASS_REGULAR_EXPRESSION ok FAIL_REGULAR_EXPRESSION bad")
        self._assert_local({"safe": f"{reg}set_tests_properties(knob PROPERTIES {safe})\n"})
        self._assert_all({
            name: f"{reg}set_tests_properties(knob PROPERTIES {name} x)\n"
            for name in ("FIXTURES_SETUP", "FIXTURES_CLEANUP", "FIXTURES_REQUIRED", "DEPENDS",
                         "RESOURCE_LOCK", "RUN_SERIAL", "DISABLED", "PROCESSORS", "NO_SUCH_PROPERTY")
        } | {
            "variable-value": f"{reg}set_tests_properties(knob PROPERTIES TIMEOUT ${{T}})\n",
            "odd-pairs": f"{reg}set_tests_properties(knob PROPERTIES TIMEOUT)\n",
            "mixed": f"{reg}set_tests_properties(knob PROPERTIES TIMEOUT 5 DEPENDS other)\n",
        })

    def test_only_non_fatal_message_modes_stay_local(self) -> None:
        self._assert_local({mode: f"message({mode} hello)\n" for mode in
                            ("STATUS", "VERBOSE", "DEBUG", "TRACE", "NOTICE", "CHECK_START",
                             "CHECK_PASS", "CHECK_FAIL")} | {"no-mode": "message(hello)\n"})
        self._assert_all({mode: f"message({mode} hello)\n" for mode in
                          ("FATAL_ERROR", "SEND_ERROR", "WARNING", "AUTHOR_WARNING",
                           "DEPRECATION", "CONFIGURE_LOG", "NO_SUCH_MODE")})

    def test_registration_options_that_reach_beyond_the_target_stay_global(self) -> None:
        self._assert_local({
            "suite": "pulp_add_test_suite(pulp-test-knob SOURCES test/test_knob.cpp LIBRARIES a "
                     "TIMEOUT 5 LABELS x PROPERTIES LABELS y TEST_PREFIX p NO_PCH)\n",
            "discover": "add_executable(pulp-test-knob test/test_knob.cpp)\n"
                        "catch_discover_tests(pulp-test-knob TEST_SPEC a PROPERTIES TIMEOUT 5 LABELS x)\n",
            "library": "add_library(pulp-fixture-knob STATIC test/a.cpp)\n",
            "legacy-free": "add_executable(pulp-test-knob WIN32 test/test_knob.cpp)\n"
                           "add_test(NAME knob COMMAND pulp-test-knob WORKING_DIRECTORY /x)\n",
        })
        self._assert_all({
            "suite-group": "pulp_add_test_suite(pulp-test-knob GROUP shared SOURCES test/a.cpp)\n",
            "suite-may-be-empty": "pulp_add_test_suite(pulp-test-knob MAY_BE_EMPTY)\n",
            "suite-discovery-args": "pulp_add_test_suite(pulp-test-knob DISCOVERY_ARGS EXTRA)\n",
            "suite-fixture-property":
                "pulp_add_test_suite(pulp-test-knob PROPERTIES FIXTURES_SETUP s)\n",
            "suite-depends-before-timeout":
                "pulp_add_test_suite(pulp-test-knob PROPERTIES DEPENDS x TIMEOUT 5)\n",
            "discover-fixture": "catch_discover_tests(pulp-test-knob PROPERTIES FIXTURES_REQUIRED s)\n",
            "discover-test-list": "catch_discover_tests(pulp-test-knob TEST_LIST SHARED_LIST)\n",
            "test-group": "pulp_add_test_group(shared LIBRARIES a)\n",
            "add-test-configurations":
                "add_test(NAME knob COMMAND x CONFIGURATIONS Debug)\n",
            "add-test-expand-lists": "add_test(NAME knob COMMAND x COMMAND_EXPAND_LISTS)\n",
            "add-test-legacy": "add_test(knob x)\n",
            "interface-library": "add_library(pulp-fixture-knob INTERFACE)\n",
            "object-library": "add_library(pulp-fixture-knob OBJECT test/a.cpp)\n",
            "untyped-library": "add_library(pulp-fixture-knob test/a.cpp)\n",
        })

    def test_scope_flow_commands_stay_global(self) -> None:
        for name, text in {
            "break": "foreach(x a b)\n  break()\nendforeach()\n",
            "continue": "foreach(x a b)\n  continue()\nendforeach()\n",
            "return": "return()\n",
            "return-propagate": "set(X 1)\nreturn(PROPAGATE X)\n",
        }.items():
            self.fx.touch("test/cmake/knob_tests.cmake", text)
            sel = self.fx.select(["test/cmake/knob_tests.cmake"])
            self.assertEqual(sel.mode, "all", name)

    def test_state_read_by_a_cmake_file_outside_test_stays_global(self) -> None:
        self.fx.touch("tools/cmake/PulpMacros.cmake", "macro(m)\n  message(${KNOB_FLAGS})\nendmacro()\n")
        self.fx.touch("test/cmake/knob_tests.cmake", "set(KNOB_FLAGS -DX)\n")
        self.assertEqual(self.fx.select(["test/cmake/knob_tests.cmake"]).mode, "all")
        self.fx.touch("tools/cmake/PulpMacros.cmake", "function(helper_fn)\nendfunction()\n")
        self.fx.touch("test/cmake/knob_tests.cmake", "helper_fn()\n")
        self.assertEqual(self.fx.select(["test/cmake/knob_tests.cmake"]).mode, "all")

    def test_a_function_defined_in_a_fragment_and_called_outside_test_stays_global(self) -> None:
        self.fx.touch("tools/cmake/PulpMacros.cmake", "knob_helper()\n")
        self.fx.touch("test/cmake/knob_tests.cmake", "function(knob_helper)\nendfunction()\n")
        self.assertEqual(self.fx.select(["test/cmake/knob_tests.cmake"]).mode, "all")

    def test_dynamically_named_variable_reads_stay_global(self) -> None:
        self.fx.touch("test/cmake/label_tests.cmake", "message(${PREFIX}_KNOB_FLAGS ${A_${B}})\n")
        self.fx.touch("test/cmake/knob_tests.cmake", "set(LIB_KNOB_FLAGS -DX)\n")
        self.assertEqual(self.fx.select(["test/cmake/knob_tests.cmake"]).mode, "all")
        self.fx.touch("test/cmake/label_tests.cmake", "message(${DYN_PREFIX_}${X})\nmessage(KNOB_${X})\n")
        self.fx.touch("test/cmake/knob_tests.cmake", "set(KNOB_FLAGS -DX)\n")
        self.assertEqual(self.fx.select(["test/cmake/knob_tests.cmake"]).mode, "all")

    def test_imported_and_alias_targets_stay_global(self) -> None:
        for text in ("add_library(knob_alias ALIAS pulp-test-knob)\n",
                     "add_library(ext SHARED IMPORTED GLOBAL)\n"):
            self.fx.touch("test/cmake/knob_tests.cmake", text)
            self.assertEqual(self.fx.select(["test/cmake/knob_tests.cmake"]).mode, "all", text)

    def test_without_an_inventory_a_fragment_stays_global(self) -> None:
        sel = at.project_affected(self.fx.model, ["test/cmake/knob_tests.cmake"], [], None, None, 1.0)
        self.assertEqual(sel.mode, "all")

    def test_a_fragment_newer_than_the_codemodel_stays_global_with_an_inventory(self) -> None:
        for stale in (["test/cmake/knob_tests.cmake"], None):
            sel = at.project_affected(self.fx.model, ["test/cmake/knob_tests.cmake"], [], None,
                                      self.fx.inventory, 1.0, stale)
            self.assertEqual(sel.mode, "all", stale)
            self.assertIn("build system", sel.reason)
        fresh = self.fx.select(["test/cmake/knob_tests.cmake"])
        self.assertEqual(fresh.mode, "focused")

    def test_a_stale_fragment_is_not_trusted(self) -> None:
        sel = at.project_affected(self.fx.model, ["test/cmake/knob_tests.cmake"], [], None, None,
                                  1.0, ["test/cmake/knob_tests.cmake"])
        self.assertEqual(sel.mode, "all")


if __name__ == "__main__":
    unittest.main()
