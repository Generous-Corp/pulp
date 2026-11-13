import json
import tempfile
import unittest
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

    def test_clean_log_has_no_failures(self):
        self.assertEqual(ct.parse_build_log("[1/1] Linking CXX executable a\n"), [])

    def test_configure_error_names_the_missing_source(self):
        log = ("CMake Error at core/view/CMakeLists.txt:42 (add_library):\n"
               "  Cannot find source file:\n\n    src/motion.cpp\n")
        errors = ct.parse_configure_log(log)
        self.assertEqual(errors[0]["where"], "core/view/CMakeLists.txt:42")
        self.assertIn("Cannot find source file", errors[0]["detail"])


class GroupingTests(unittest.TestCase):
    def report(self, build_text):
        return ct.build_report(INPUTS, "", build_text, 1, [], True, True)

    def test_planted_missing_header_lands_in_its_row(self):
        report = self.report(HEADER_LOG)
        self.assertEqual(list(report["by_row"]), ["A2"])
        self.assertEqual(report["stranded_rows_hit"], 1)
        self.assertEqual(report["build"]["failed_targets"][0]["rows"], ["A2"])

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
