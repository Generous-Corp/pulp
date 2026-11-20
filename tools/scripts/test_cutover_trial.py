import json
import os
import tempfile
import unittest
from unittest import mock
import subprocess
from pathlib import Path

import cutover_trial as ct

ROWS = [
    {"id": "A2", "capability": "swift bridge", "paths": ["pulp/view/motion.hpp"]},
    {"id": "H4", "capability": "mcp tools", "paths": ["tools/mcp/"]},
    {"id": "A13", "capability": "jsc", "paths": ["core/view/src/js_*.mm"]},
]
INPUTS = {"export_id": "x", "ledger_stranded_count": 3, "delete_paths": ["core/view/include/pulp/view/motion.hpp"],
          "stranded_rows": ROWS}

HEADER_LOG = """\
[1/9] Building CXX object apple/PulpBridge.cpp.o
FAILED: apple/CMakeFiles/bridge.dir/Sources/PulpSwift/PulpBridge.cpp.o
/usr/bin/clang++ -c ../apple/Sources/PulpSwift/PulpBridge.cpp
../apple/Sources/PulpSwift/PulpBridge.cpp:7:10: fatal error: 'pulp/view/motion.hpp' file not found
    7 | #include "pulp/view/motion.hpp"
ninja: build stopped: subcommand failed.
"""


class ParseTests(unittest.TestCase):
    def test_missing_header_is_the_first_error_of_its_target(self):
        failures = ct.parse_build_log(HEADER_LOG)
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["kind"], "missing-header")
        self.assertEqual(failures[0]["detail"], "pulp/view/motion.hpp")
        self.assertTrue(failures[0]["source"].endswith("PulpBridge.cpp"))

    def test_undefined_symbol_and_unknown(self):
        log = ("FAILED: bin/tool\nUndefined symbols for architecture arm64:\n"
               '  "_pulp_motion_new", referenced from:\n'
               "FAILED: bin/other\nsomething odd\n")
        failures = ct.parse_build_log(log)
        self.assertEqual([f["kind"] for f in failures], ["undefined-symbol", "unknown"])
        self.assertEqual(failures[0]["detail"], "_pulp_motion_new")

    def test_tool_error_is_first_error_of_its_target(self):
        failures = ct.parse_build_log(
            "FAILED: core/CMakeFiles/view.dir/view.cpp.o\n"
            "ccache: error: failed to create temporary file\n")
        self.assertEqual(failures[0]["kind"], "tool-error")
        self.assertEqual(failures[0]["detail"], "failed to create temporary file")

    def test_clean_log_has_no_failures(self):
        self.assertEqual(ct.parse_build_log("[1/1] Linking CXX executable a\n"), [])

    def test_configure_error_names_the_missing_source(self):
        log = ("CMake Error at core/view/CMakeLists.txt:42 (add_library):\n"
               "  Cannot find source file:\n\n    src/motion.cpp\n")
        errors = ct.parse_configure_log(log)
        self.assertEqual(errors[0]["where"], "core/view/CMakeLists.txt:42")
        self.assertIn("Cannot find source file", errors[0]["detail"])

    def test_configure_error_captures_following_source_path_for_grouping(self):
        log = ("CMake Error at cmake/other/CMakeLists.txt:42 (add_library):\n"
               "  Cannot find source file:\n\n    tools/mcp/server.cpp\n")
        errors = ct.parse_configure_log(log)
        self.assertIn("tools/mcp/server.cpp", errors[0]["detail"])
        report = ct.build_report(INPUTS, log, "", 1, [], False, False)
        self.assertEqual(list(report["by_row"]), ["H4"])


class GroupingTests(unittest.TestCase):
    def report(self, build_text):
        return ct.build_report(INPUTS, "", build_text, 1, [], True, True)

    def test_planted_missing_header_lands_in_its_row(self):
        report = self.report(HEADER_LOG)
        self.assertEqual(list(report["by_row"]), ["A2"])
        self.assertEqual(report["stranded_rows_hit"], 1)
        self.assertEqual(report["build"]["failed_targets"][0]["rows"], ["A2"])
        self.assertEqual(len(report["row_results"]), 3)
        self.assertEqual(report["row_results"][0]["result"], "fails")

    def test_control_without_the_planted_header_hits_no_row(self):
        report = self.report(HEADER_LOG.replace("pulp/view/motion.hpp", "pulp/other/clean.hpp")
                             .replace("apple/Sources/PulpSwift/PulpBridge.cpp", "docs/x.cpp")
                             .replace("apple/CMakeFiles/bridge.dir/Sources/PulpSwift/PulpBridge.cpp.o", "x.o"))
        self.assertEqual(report["by_row"], {})
        self.assertEqual(report["ungrouped_targets"], ["x.o"])
        self.assertEqual(self.report("[1/1] ok\n")["by_row"], {})

    def test_path_forms(self):
        self.assertTrue(ct.path_matches("tools/mcp/", "tools/mcp/server.cpp"))
        self.assertTrue(ct.path_matches("core/view/src/js_*.mm", "core/view/src/js_jsc_engine.mm"))
        self.assertTrue(ct.path_matches("pulp/view/motion.hpp", "../core/view/include/pulp/view/motion.hpp"))
        self.assertFalse(ct.path_matches("tools/mcp/", "tools/mcpx/a.cpp"))
        self.assertFalse(ct.path_matches("pulp/view/motion.hpp", "pulp/view/motion_extra.hpp"))

    def test_configure_failure_is_grouped_and_skips_the_build(self):
        log = ("CMake Error at tools/mcp/CMakeLists.txt:3 (add_executable):\n"
               "  Cannot find source file:\n\n    tools/mcp/server.cpp\n")
        report = ct.build_report(INPUTS, log, "", 1, [], False, False)
        self.assertFalse(report["build"]["ran"])
        self.assertEqual(list(report["by_row"]), ["H4"])

    def test_build_returncode_distinguishes_success_from_unparsed_failure(self):
        success = ct.build_report(INPUTS, "", "ninja: no work to do\n", 1, [], True,
                                  True, 0)
        failure = ct.build_report(INPUTS, "", "ninja: error: missing and no known rule\n",
                                  1, [], True, True, 1)
        self.assertTrue(success["build"]["ok"])
        self.assertEqual(success["build"]["returncode"], 0)
        self.assertFalse(failure["build"]["ok"])
        self.assertEqual(failure["build"]["returncode"], 1)
        self.assertEqual(failure["build"]["failed_targets"], [])


class EditClassTests(unittest.TestCase):
    def test_classes(self):
        c = ct.classify_edit
        self.assertEqual(c('Target "pulp-cli" links to: pulp::render but the target was not found.', set()),
                         "b-stranded-consumer")
        self.assertEqual(c('Cannot specify sources for target "pulp-render" which is not built by this project.', set()),
                         "cascade")
        self.assertEqual(c("Cannot find source file: src/data_display.cpp", set()), "unclassified")
        self.assertEqual(c("something unforeseen", set()), "unclassified")

    def test_disable_statement_comments_the_whole_call_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "CMakeLists.txt").write_text("a()\nfoo(x\n  y)\nb()\n", encoding="utf-8")
            edit = ct.disable_statement(root, "CMakeLists.txt", 2, "why")
            text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertEqual(text.count("# cutover-trial: "), 2)
            self.assertIn("b()", text)
            self.assertEqual(edit["line"], 2)
            self.assertIsNone(ct.disable_statement(root, "CMakeLists.txt", 2, "why"))



class RepairRegressionTests(unittest.TestCase):
    def test_restore_deleted_referenced_js_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "core/view/js").mkdir(parents=True)
            (root / "core/view/cmake").mkdir(parents=True)
            source = root / "core/view/js/css-colors.js"
            source.write_text("var __cssColors__ = {};\n", encoding="utf-8")
            (root / "core/view/cmake/PulpViewJsPreludes.cmake").write_text(
                "js/css-colors.js\n", encoding="utf-8")
            for args in (["init", "-q"], ["add", "."],
                         ["-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                          "commit", "-qm", "fixture"]):
                done = ct.run(["git", "-C", str(root), *args])
                self.assertEqual(done.returncode, 0, done.stderr)
            source.unlink()
            restored = ct.restore_deleted_inputs(root, ["core/view/js/css-colors.js"])
            self.assertEqual(restored[0]["restored"], "core/view/js/css-colors.js")
            self.assertEqual(source.read_text(encoding="utf-8"), "var __cssColors__ = {};\n")

    def test_external_and_symlink_diagnostics_cannot_write_outside_scratch(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp)
            root = parent / "scratch"
            root.mkdir()
            external = parent / "installed.cmake"
            original = 'include("missing.cmake")\n'
            external.write_text(original, encoding="utf-8")
            (root / "linked.cmake").symlink_to(external)
            for diagnostic in (str(external), "../installed.cmake", "linked.cmake"):
                with self.subTest(diagnostic=diagnostic):
                    self.assertIsNone(ct.disable_statement(root, diagnostic, 1, "missing"))
                    self.assertEqual(external.read_text(encoding="utf-8"), original)
            local = root / "local.cmake"
            local.write_text(original, encoding="utf-8")
            self.assertIsNotNone(ct.disable_statement(root, str(local), 1, "missing"))
            self.assertTrue(local.read_text(encoding="utf-8").startswith(ct.CUTOVER_MARK))

    def test_statement_lexing_preserves_adjacent_calls(self):
        statements = ['include("missing#module.cmake")',
                      'include("missing(module.cmake")',
                      'include("missing)module.cmake")',
                      r'include(missing\(module.cmake)',
                      'include([=[missing#(module.cmake]=])',
                      'include(#[=[ comment ) #\n still a comment ]=]\n "missing.cmake")',
                      r'include("missing\"#(module.cmake")']
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "CMakeLists.txt"
            for statement in statements:
                with self.subTest(statement=statement):
                    path.write_text(statement + '\nmessage(STATUS "survives")\n', encoding="utf-8")
                    self.assertIsNotNone(ct.disable_statement(root, "CMakeLists.txt", 1, "missing"))
                    self.assertEqual(path.read_text(encoding="utf-8").splitlines()[-1], 'message(STATUS "survives")')
                    self.assertEqual(path.read_text(encoding="utf-8").count(ct.CUTOVER_MARK), len(statement.splitlines()))
                    result = ct.run(["cmake", "-P", str(path)])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("survives", result.stdout)

    def test_ambiguous_or_invalid_span_is_not_edited(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "CMakeLists.txt"
            for original, line in [('include("unterminated)\nmessage(ok)\n', 1),
                                   ('include(x) message(ok)\n', 1),
                                   ('include(missing[[.cmake)\nset(untouched x]])\n', 1),
                                   ('include(missing\\ [[.cmake)\nset(untouched x]])\n', 1),
                                   ('include(missing\\([[.cmake)\nset(untouched x]])\n', 1),
                                   ('include(x)\n', 0), ('include(x)\n', 99)]:
                with self.subTest(original=original, line=line):
                    path.write_text(original, encoding="utf-8")
                    self.assertIsNone(ct.disable_statement(root, "CMakeLists.txt", line, "missing"))
                    self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_relative_deleted_module_is_restored_for_its_cmake_caller(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            module = root / "sub/tools/cmake/Removed.cmake"
            module.parent.mkdir(parents=True)
            module.write_text('set(RESTORED yes)\n', encoding="utf-8")
            (root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.20)\nproject(fixture NONE)\nadd_subdirectory(sub)\n', encoding="utf-8")
            (root / "sub/CMakeLists.txt").write_text('include(tools/cmake/Removed.cmake)\nif(NOT RESTORED)\nmessage(FATAL_ERROR "module not restored")\nendif()\n', encoding="utf-8")
            for args in (["init", "-q"], ["add", "."],
                         ["-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"]):
                done = ct.run(["git", "-C", str(root), *args])
                self.assertEqual(done.returncode, 0, done.stderr)
            module.unlink()
            def configure():
                done = ct.run(["cmake", "-S", str(root), "-B", str(root / "build")])
                return done.returncode == 0, done.stdout + done.stderr
            edits, log, ok = ct.repair_configure(root, configure, deleted={"sub/tools/cmake/Removed.cmake"})
            self.assertTrue(ok, log)
            self.assertEqual([e.get("restored") for e in edits], ["sub/tools/cmake/Removed.cmake"])
            self.assertEqual(module.read_text(encoding="utf-8"), 'set(RESTORED yes)\n')

    def test_two_deleted_modules_sharing_one_caller_are_both_restored(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "cmake").mkdir()
            names = ("cmake/A.cmake", "cmake/B.cmake")
            (root / "cmake/A.cmake").write_text("set(HAVE_A yes)\n", encoding="utf-8")
            (root / "cmake/B.cmake").write_text("set(HAVE_B yes)\n", encoding="utf-8")
            (root / "cmake/helper.cmake").write_text(
                "include(cmake/A.cmake)\ninclude(cmake/B.cmake)\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(fixture NONE)\n"
                "include(cmake/helper.cmake)\n"
                "if(NOT HAVE_A OR NOT HAVE_B)\nmessage(FATAL_ERROR \"module not restored\")\nendif()\n",
                encoding="utf-8")
            for args in (["init", "-q"], ["add", "."],
                         ["-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"]):
                done = ct.run(["git", "-C", str(root), *args])
                self.assertEqual(done.returncode, 0, done.stderr)
            for name in names:
                (root / name).unlink()
            def configure():
                done = ct.run(["cmake", "-S", str(root), "-B", str(root / "build")])
                return done.returncode == 0, done.stdout + done.stderr
            edits, log, ok = ct.repair_configure(root, configure, deleted=set(names))
            self.assertTrue(ok, log)
            self.assertEqual(sorted(e.get("restored") for e in edits), sorted(names))
            for name in names:
                self.assertTrue((root / name).exists())

    def test_classification_requires_actual_deleted_path_in_source_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "sub").mkdir()
            messages = [('Cannot find source file: src/removed.cpp', 'sub/src/removed.cpp'),
                        (f'The source directory "{root}/sub/gone" does not contain a CMakeLists.txt file.', 'sub/gone/CMakeLists.txt')]
            for message, deleted_path in messages:
                for deleted, expected in [({deleted_path}, "a-expected"), (set(), "unclassified"),
                                          ({"other/" + deleted_path}, "unclassified")]:
                    with self.subTest(message=message, deleted=deleted):
                        cmake = root / "sub/CMakeLists.txt"
                        cmake.write_text('add_library(gone src/removed.cpp)\n', encoding="utf-8")
                        configure = mock.Mock(side_effect=[(False, f'CMake Error at sub/CMakeLists.txt:1 (add_library):\n  {message}\n'), (True, '')])
                        edits, _, ok = ct.repair_configure(root, configure, deleted=deleted)
                        self.assertTrue(ok)
                        self.assertEqual(edits[0]["class"], expected)

    def test_nested_include_uses_cmake_source_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "sub").mkdir()
            (root / "cmake").mkdir()
            (root / "cmake/helper.cmake").write_text('include(src/Removed.cmake)\n', encoding="utf-8")
            (root / "sub/src").mkdir()
            module = root / "sub/src/Removed.cmake"
            module.write_text("set(RESTORED yes)\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.20)\nproject(fixture NONE)\nadd_subdirectory(sub)\n', encoding="utf-8")
            (root / "sub/CMakeLists.txt").write_text('include(../cmake/helper.cmake)\n', encoding="utf-8")
            for args in (["init", "-q"], ["add", "."],
                         ["-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"]):
                done = ct.run(["git", "-C", str(root), *args])
                self.assertEqual(done.returncode, 0, done.stderr)
            module.unlink()
            def configure():
                done = ct.run(["cmake", "-S", str(root), "-B", str(root / "build")])
                return done.returncode == 0, done.stdout + done.stderr
            edits, log, ok = ct.repair_configure(root, configure, deleted={"sub/src/Removed.cmake"})
            self.assertTrue(ok, log)
            self.assertEqual(edits[0]["restored"], "sub/src/Removed.cmake")

    def test_function_uses_callers_source_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "sub/src").mkdir(parents=True)
            module = root / "sub/src/Removed.cmake"
            module.write_text("set(RESTORED yes)\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.20)\nproject(fixture NONE)\nfunction(load_module)\ninclude(src/Removed.cmake)\nendfunction()\nadd_subdirectory(sub)\n', encoding="utf-8")
            (root / "sub/CMakeLists.txt").write_text('load_module()\n', encoding="utf-8")
            for args in (["init", "-q"], ["add", "."],
                         ["-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"]):
                done = ct.run(["git", "-C", str(root), *args])
                self.assertEqual(done.returncode, 0, done.stderr)
            module.unlink()
            def configure():
                done = ct.run(["cmake", "-S", str(root), "-B", str(root / "build")])
                return done.returncode == 0, done.stdout + done.stderr
            edits, log, ok = ct.repair_configure(root, configure, deleted={"sub/src/Removed.cmake"})
            self.assertTrue(ok, log)
            self.assertEqual(edits[0]["restored"], "sub/src/Removed.cmake")

    def test_nested_functions_use_outer_callers_source_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "sub/src").mkdir(parents=True)
            module = root / "sub/src/Removed.cmake"
            module.write_text("set(RESTORED yes)\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.20)\nproject(fixture NONE)\nfunction(load_inner)\ninclude(src/Removed.cmake)\nendfunction()\nfunction(load_module)\nload_inner()\nendfunction()\nadd_subdirectory(sub)\n', encoding="utf-8")
            (root / "sub/CMakeLists.txt").write_text('load_module()\n', encoding="utf-8")
            for args in (["init", "-q"], ["add", "."],
                         ["-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"]):
                done = ct.run(["git", "-C", str(root), *args])
                self.assertEqual(done.returncode, 0, done.stderr)
            module.unlink()
            def configure():
                done = ct.run(["cmake", "-S", str(root), "-B", str(root / "build")])
                return done.returncode == 0, done.stdout + done.stderr
            edits, log, ok = ct.repair_configure(root, configure, deleted={"sub/src/Removed.cmake"})
            self.assertTrue(ok, log)
            self.assertEqual(edits[0]["restored"], "sub/src/Removed.cmake")

    def test_unknown_module_search_path_is_unresolved_and_symlinks_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "scratch"
            root.mkdir()
            module = root / "modules/Removed.cmake"
            module.parent.mkdir()
            module.write_text('set(RESTORED yes)\n', encoding="utf-8")
            (root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.20)\nproject(fixture NONE)\ninclude(Removed)\n', encoding="utf-8")
            for args in (["init", "-q"], ["add", "."],
                         ["-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture"]):
                done = ct.run(["git", "-C", str(root), *args])
                self.assertEqual(done.returncode, 0, done.stderr)
            module.unlink()
            def configure():
                done = ct.run(["cmake", "-S", str(root), "-B", str(root / "build"),
                               "-DCMAKE_MODULE_PATH=" + str(module.parent)])
                return done.returncode == 0, done.stdout + done.stderr
            edits, log, ok = ct.repair_configure(root, configure, deleted={"modules/Removed.cmake"})
            self.assertFalse(ok, log)
            self.assertEqual(edits, [])
            self.assertFalse(module.exists())
            self.assertIn("include(Removed)", (root / "CMakeLists.txt").read_text(encoding="utf-8"))
            module.parent.rmdir()
            external = Path(tmp) / "external"
            external.mkdir()
            (root / "modules").symlink_to(external, target_is_directory=True)
            self.assertIsNone(ct.restore_deleted_module(root, 'include could not find requested file: modules/Removed.cmake',
                                                       {"modules/Removed.cmake"}, root))
            self.assertEqual(list(external.iterdir()), [])
            outside = external / "Removed.cmake"
            outside.write_text("preserve me", encoding="utf-8")
            with self.assertRaises(ct.InputError):
                ct.delete_paths(root, ["modules/Removed.cmake"])
            self.assertEqual(outside.read_text(encoding="utf-8"), "preserve me")
            previous = Path.cwd()
            try:
                os.chdir(root.parent)
                with self.assertRaises(ct.InputError):
                    ct.delete_paths(Path("scratch"), ["modules/Removed.cmake"])
                self.assertEqual(outside.read_text(encoding="utf-8"), "preserve me")
            finally:
                os.chdir(previous)

    def test_no_repair_reports_configure_success_and_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for ok in (True, False):
                with self.subTest(ok=ok):
                    report = root / "report.json"
                    done = subprocess.CompletedProcess([], 0 if ok else 1, 'configure output', '')
                    built = subprocess.CompletedProcess([], 0, 'build output', '')
                    with mock.patch.object(ct, "make_scratch"), mock.patch.object(ct, "delete_paths", return_value=(0, [])), \
                         mock.patch.object(ct, "deleted_data_still_referenced", return_value=[]), \
                 mock.patch.object(ct, "restore_deleted_inputs", return_value=[]), \
                         mock.patch.object(ct, "run", side_effect=[done, built] if ok else [done]), \
                         mock.patch.object(ct, "repair_configure", side_effect=AssertionError("repair must not run")):
                        self.assertEqual(ct.main(["--scratch", str(root / "source"), "--report", str(report), "--no-repair"]), 0)
                    data = json.loads(report.read_text(encoding="utf-8"))
                    self.assertEqual(data["configure"]["ok"], ok)
                    self.assertEqual(data["build"]["ran"], ok)
                    self.assertEqual((root / "cutover-configure.log").read_text(encoding="utf-8"), 'configure output')

    def test_build_wrapper_is_used_for_trial_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            report = root / "report.json"
            configure = subprocess.CompletedProcess([], 0, 'configure output', '')
            built = subprocess.CompletedProcess([], 0, 'build output', '')
            with mock.patch.object(ct, "make_scratch"), mock.patch.object(ct, "delete_paths", return_value=(0, [])), \
                 mock.patch.object(ct, "deleted_data_still_referenced", return_value=[]), \
                 mock.patch.object(ct, "restore_deleted_inputs", return_value=[]), \
                 mock.patch.object(ct, "run", side_effect=[configure, built]) as run_mock:
                self.assertEqual(ct.main(["--scratch", str(root / "source"), "--report", str(report),
                                          "--no-repair", "--build-wrapper", "/usr/local/bin/leased-build"]), 0)
            build_call = run_mock.call_args_list[1]
            self.assertEqual(build_call.args[0][0], "/usr/local/bin/leased-build")
            self.assertEqual(build_call.args[0][1:3], ["cmake", "--build"])
            self.assertTrue(build_call.kwargs["env"]["CCACHE_TEMPDIR"].endswith("build/cutover-ccache-tmp"))


class InputTests(unittest.TestCase):
    def test_shipped_inputs_hold_the_687_paths(self):
        data = ct.load_inputs()
        self.assertEqual(len(data["delete_paths"]), 687)
        self.assertEqual(len(set(data["delete_paths"])), 687)

    def test_row_count_equals_the_ledger_stranded_count(self):
        data = ct.load_inputs()
        self.assertEqual(data["ledger_stranded_count"], 42)
        self.assertEqual(len(data["stranded_rows"]), 42)
        self.assertEqual(len({r["id"] for r in data["stranded_rows"]}), 42)

    def test_control_a_dropped_row_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = ct.load_inputs()
            data["stranded_rows"].pop()
            short = Path(tmp) / "short.json"
            short.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(ct.InputError):
                ct.load_inputs(short)

    def test_unreadable_or_malformed_inputs_exit_2(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.json"
            bad.write_text(json.dumps({"delete_paths": ["../x"], "stranded_rows": []}), encoding="utf-8")
            self.assertEqual(ct.main(["--inputs", str(bad), "--report", f"{tmp}/r.json",
                                      "--analyze-log", str(bad)]), 2)
            self.assertEqual(ct.main(["--inputs", f"{tmp}/none.json", "--report",
                                      f"{tmp}/r.json", "--analyze-log", str(bad)]), 2)

    def test_malformed_json_shapes_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            for index, value in enumerate(
                    ([], {"delete_paths": [], "stranded_rows": None},
                     {"delete_paths": [], "stranded_rows": 3})):
                bad = Path(tmp) / f"bad-{index}.json"
                bad.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaises(ct.InputError):
                    ct.load_inputs(bad)

    def test_a_non_string_row_path_exits_2_without_a_traceback(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "b.log"
            log.write_text(HEADER_LOG, encoding="utf-8")
            out = f"{tmp}/r.json"
            for bad_path in (None, 3, ""):
                data = json.loads(json.dumps(INPUTS))
                data["stranded_rows"][0]["paths"] = [bad_path]
                bad = Path(tmp) / "bad.json"
                bad.write_text(json.dumps(data), encoding="utf-8")
                with self.assertRaises(ct.InputError):
                    ct.load_inputs(bad)
                self.assertEqual(ct.main(["--inputs", str(bad), "--analyze-log", str(log),
                                          "--report", out]), 2)
            good = Path(tmp) / "good.json"
            good.write_text(json.dumps(INPUTS), encoding="utf-8")
            self.assertEqual(ct.main(["--inputs", str(good), "--analyze-log", str(log),
                                      "--report", out]), 0)

    def test_scratch_refusals(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)
            with self.assertRaises(ct.InputError):
                ct.make_scratch(src, src / "inner", "HEAD")
            with self.assertRaises(ct.InputError):
                ct.make_scratch(src, src, "HEAD")

    def test_delete_paths_removes_only_listed_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a").mkdir()
            (root / "a/gone.hpp").write_text("x", encoding="utf-8")
            (root / "a/stay.hpp").write_text("x", encoding="utf-8")
            deleted, missing = ct.delete_paths(root, ["a/gone.hpp", "a/none.hpp"])
            self.assertEqual((deleted, missing), (1, ["a/none.hpp"]))
            self.assertTrue((root / "a/stay.hpp").exists())

    def test_analyze_log_writes_a_report_and_exits_0_on_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "b.log"
            log.write_text(HEADER_LOG, encoding="utf-8")
            inputs = Path(tmp) / "in.json"
            inputs.write_text(json.dumps(INPUTS), encoding="utf-8")
            out = Path(tmp) / "r.json"
            self.assertEqual(ct.main(["--inputs", str(inputs), "--analyze-log", str(log),
                                      "--report", str(out)]), 0)
            self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["schema"], ct.SCHEMA)


if __name__ == "__main__":
    unittest.main()
