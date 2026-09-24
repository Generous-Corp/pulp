#!/usr/bin/env python3
"""Tests for tools/scripts/build_time_report.py.

The blast-radius cases drive the real code against a stub `ninja` that plans
work only when a watched source is newer than its recorded mtime, so they
prove the touch → dry-run → restore loop and its refusal to report zeros
without a real build tree.

Run:  python3 -m unittest test_build_time_report   (from tools/scripts)
"""
from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "build_time_report.py"
spec = importlib.util.spec_from_file_location("build_time_report", SCRIPT)
assert spec and spec.loader
btr = importlib.util.module_from_spec(spec)
sys.modules["build_time_report"] = btr
spec.loader.exec_module(btr)


class NinjaLogTests(unittest.TestCase):
    LOG = textwrap.dedent("""\
        # ninja log v7
        0\t1000\t1\tcore/view/CMakeFiles/v.dir/src/widgets.cpp.o\taaa
        0\t2000\t1\tgen/a.cpp\tbbb
        0\t2000\t1\tgen/a.hpp\tbbb
        0\t2000\t1\tgen/a.json\tbbb
        1000\t3000\t1\tAUv3/PulpGain.appex/Contents/MacOS/PulpGain\tddd
        1000\t4000\t1\ttest/pulp-test-widgets\tccc
        4000\t4100\t1\tcore/view/libpulp-view-core.a\tfff
        3000\t4500\t1\texamples/pulp-gain/pulp-control-shipping-stamps/PulpGain.stamp\teee
        4000\t20000\t1\texperimental/pulp-rs/CMakeFiles/pulp-rust-cli\tggg
        0\t500\t1\tcore/view/CMakeFiles/v.dir/src/widgets.cpp.o\taaa
        0\t700\t1\ttest/pulp-test-widgets\tccc
        """)

    def test_multi_output_edge_counted_once(self) -> None:
        runs = btr.parse_ninja_log(self.LOG)
        s = btr.summarize_edges(runs[0])
        # three outputs of edge bbb share one execution: 2 s, not 6 s
        self.assertEqual(s["categories"]["other"]["count"], 1)
        self.assertEqual(s["categories"]["other"]["edge_seconds"], 2.0)
        self.assertEqual(s["edges"], 7)
        self.assertEqual(s["log_lines"], 9)

    def test_categories(self) -> None:
        s = btr.summarize_edges(btr.parse_ninja_log(self.LOG)[0])["categories"]
        self.assertEqual(s["compile"]["edge_seconds"], 1.0)
        self.assertEqual(s["test-exe link"]["edge_seconds"], 3.0)
        self.assertEqual(s["bundle link"]["count"], 1)
        self.assertEqual(s["codesign/stamps"]["count"], 1)
        self.assertEqual(s["archive"]["count"], 1)
        self.assertEqual(s["cargo"]["edge_seconds"], 16.0)

    def test_categorize_output(self) -> None:
        cases = {
            "test/CMakeFiles/pulp-test-x.dir/test_x.cpp.o": "compile",
            "test/web-compat/pulp-web-compat-grid": "test-exe link",
            "examples/pulp-gain/PulpGain.clap/Contents/MacOS/PulpGain": "bundle link",
            "core/audio/libpulp-audio.a": "archive",
            "tools/cli/pulp": "other",
        }
        for out, cat in cases.items():
            self.assertEqual(btr.categorize_output(out), cat, out)

    def test_runs_split_when_clock_restarts(self) -> None:
        runs = btr.parse_ninja_log(self.LOG)
        self.assertEqual(len(runs), 2)
        self.assertEqual(len(runs[1]), 2)
        self.assertEqual(btr.select_runs(runs, "last"), [1])
        self.assertEqual(btr.select_runs(runs, "0-1"), [0, 1])
        with self.assertRaises(ValueError):
            btr.select_runs(runs, "5")

    def test_report_from_build_dir(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            Path(d, ".ninja_log").write_text(self.LOG)
            rep = btr.ninja_log_report(Path(d), "0")
            self.assertEqual(rep["runs_total"], 2)
            self.assertEqual(rep["selected"]["edge_seconds"], 1 + 2 + 3 + 2 + 1.5 + 0.1 + 16)
            self.assertIn("| compile |", btr.render_log_markdown(rep))


class TimeTraceTests(unittest.TestCase):
    def _ev(self, ph: str, ts: int, detail: str) -> dict:
        return {"name": "Source", "ph": ph, "ts": ts, "args": {"detail": detail}}

    def test_nested_source_pairs_match_by_stack(self) -> None:
        events = [self._ev("b", 0, "a.hpp"), self._ev("b", 10, "<vector>"),
                  self._ev("e", 30, "<vector>"), self._ev("e", 50, "a.hpp"),
                  {"name": "Source", "ph": "e", "ts": 60}]  # unmatched: ignored
        self.assertEqual(btr.source_intervals(events), [("<vector>", 20), ("a.hpp", 50)])

    def test_phase_totals_from_total_events(self) -> None:
        events = [{"name": "Total Frontend", "ph": "X", "dur": 7},
                  {"name": "Frontend", "ph": "X", "dur": 99},
                  {"name": "Total ExecuteCompiler", "ph": "X", "dur": 9}]
        self.assertEqual(btr.phase_totals(events), {"Frontend": 7, "ExecuteCompiler": 9})

    def test_aggregate_over_build_dir(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            src = Path(d, "src")
            tdir = Path(d, "build/core/view/CMakeFiles/v.dir/src")
            tdir.mkdir(parents=True)
            trace = {"traceEvents": [
                self._ev("b", 0, f"{src}/core/view/include/pulp/view/view.hpp"),
                self._ev("e", 2_000_000, "x"),
                {"name": "InstantiateClass", "ph": "X", "dur": 500_000,
                 "args": {"detail": "std::vector<int>"}},
                {"name": "Total ExecuteCompiler", "ph": "X", "dur": 3_000_000},
                {"name": "Total Frontend", "ph": "X", "dur": 2_500_000},
                {"name": "Total Backend", "ph": "X", "dur": 500_000}]}
            (tdir / "widgets.cpp.json").write_text(json.dumps(trace))
            (tdir / "not-a-trace.json").write_text("{}")
            rep = btr.aggregate_traces(Path(d, "build"), src)
            self.assertEqual(rep["tus"], 1)
            self.assertEqual(rep["areas"], [{"area": "core/view", "cpu_s": 3.0, "tus": 1}])
            self.assertEqual(rep["headers"][0]["header"], "core/view/include/pulp/view/view.hpp")
            self.assertEqual(rep["headers"][0]["cpu_s"], 2.0)
            self.assertEqual(rep["templates"][0]["template"], "std::vector<…>")


MANIFEST = textwrap.dedent("""\
    ninja_required_version = 1.5
    rule CXX_COMPILER__lib_unscanned_Release
      command = true
    rule CXX_EXECUTABLE_LINKER__app_Release
      command = true
    build CMakeFiles/lib.dir/b.cpp.o: CXX_COMPILER__lib_unscanned_Release {src}/b.cpp || order
    build CMakeFiles/lib.dir/a.cpp.o: CXX_COMPILER__lib_unscanned_Release {src}/a.cpp || order
    build test/pulp-test-a: CXX_EXECUTABLE_LINKER__app_Release CMakeFiles/lib.dir/a.cpp.o $
        CMakeFiles/lib.dir/b.cpp.o
    build CMakeFiles/cmake.verify_globs | CMakeFiles/VerifyGlobs.cmake_force: VERIFY_GLOBS | $
        CMakeFiles/VerifyGlobs.cmake_force
      pool = console
      restat = 1
    build build.ninja {bd}/cmake_install.cmake: RERUN_CMAKE $
        CMakeFiles/cmake.verify_globs
      pool = console
    build CMakeFiles/VerifyGlobs.cmake_force: phony
    build all: phony test/pulp-test-a
    """)

# A stand-in for ninja: plans a source's compile + the link when that source is
# newer than STUB_BASE_MTIME_NS. STUB_MODE simulates a failure: "blind" sees no
# change at all, "blind-a" misses only a.cpp (so the built-in control on b.cpp
# still passes), "short" prints fewer lines than it plans, "regen" re-runs CMake.
STUB_NINJA = textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, sys
    mode = os.environ.get("STUB_MODE", "")
    args = sys.argv[1:]
    if "-t" in args:
        sys.exit(0)
    text = open(args[args.index("-f") + 1]).read()
    if mode == "regen" or "RERUN_CMAKE" in text:
        print("[1/1] Re-running CMake...")
        sys.exit(0)
    print("ninja: Entering directory `x'")
    base = int(os.environ["STUB_BASE_MTIME_NS"])
    src = os.environ["STUB_SRC"]
    steps = ["Building pulp-rs (Rust CLI) via cargo (release profile)"]
    for name in ("b.cpp", "a.cpp"):
        blind = mode == "blind" or (mode == "blind-a" and name == "a.cpp")
        if not blind and os.stat(os.path.join(src, name)).st_mtime_ns > base:
            steps.append(f"Building CXX object CMakeFiles/lib.dir/{name}.o")
    if len(steps) > 1:
        steps.append("Linking CXX executable test/pulp-test-a")
    shown = steps[:-1] if mode == "short" else steps
    for i, s in enumerate(shown, 1):
        print(f"[{i}/{len(steps)}] {s}")
    """)


class BlastRadiusTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.src = root / "src"
        self.bd = root / "build"
        self.src.mkdir()
        self.bd.mkdir()
        self.cpp = self.src / "a.cpp"
        self.cpp.write_text("int a;\n")
        self.probe = self.src / "b.cpp"
        self.probe.write_text("int b;\n")
        self.hdr = self.src / "unused.hpp"
        self.hdr.write_text("\n")
        (self.bd / "build.ninja").write_text(MANIFEST.format(src=self.src, bd=self.bd))
        self.ninja = root / "ninja"
        self.ninja.write_text(STUB_NINJA)
        self.ninja.chmod(0o755)
        old = os.stat(self.cpp).st_mtime_ns - 10_000_000_000
        for f in (self.cpp, self.probe):
            os.utime(f, ns=(old, old))
        self.saved = old
        os.environ.update(STUB_BASE_MTIME_NS=str(old), STUB_SRC=str(self.src))
        os.environ.pop("STUB_MODE", None)

    def tearDown(self) -> None:
        for k in ("STUB_BASE_MTIME_NS", "STUB_SRC", "STUB_MODE"):
            os.environ.pop(k, None)
        self._tmp.cleanup()

    def test_strip_removes_only_the_regeneration_edges(self) -> None:
        text, n = btr.strip_regeneration_edges((self.bd / "build.ninja").read_text(), str(self.bd))
        self.assertEqual(n, 3)
        self.assertNotIn("RERUN_CMAKE", text)
        self.assertNotIn("VERIFY_GLOBS", text)
        self.assertNotIn("pool = console", text)
        self.assertIn("build test/pulp-test-a:", text)
        self.assertIn("build all: phony", text)

    def test_touched_source_reports_compile_and_link_and_restores_mtime(self) -> None:
        rep = btr.blast_radius(self.bd, ["a.cpp", "unused.hpp", "gone.cpp"], self.src,
                               str(self.ninja))
        self.assertEqual(rep["control"]["total"], 1)  # the always-run cargo step
        a, unused, gone = rep["files"]
        self.assertEqual((a["compiles"], a["exe_links"], a["test_exe_links"]), (1, 1, 1))
        self.assertEqual(unused["status"], "no-dependents")
        self.assertEqual(gone["status"], "missing")
        self.assertEqual(rep["positive_control"]["file"], "b.cpp")
        self.assertEqual(rep["positive_control"]["compiles"], 1)
        self.assertEqual(os.stat(self.cpp).st_mtime_ns, self.saved)

    def test_blind_dry_run_refuses_instead_of_reporting_zero(self) -> None:
        os.environ["STUB_MODE"] = "blind"
        # Only a header is requested, so nothing in the list itself could expose
        # the blind dry run; the built-in positive control has to.
        with self.assertRaises(btr.InstrumentBroken) as ctx:
            btr.blast_radius(self.bd, ["unused.hpp"], self.src, str(self.ninja))
        self.assertIn("positive control", str(ctx.exception))
        self.assertEqual(os.stat(self.cpp).st_mtime_ns, self.saved)
        proc = subprocess.run([sys.executable, str(SCRIPT), "blast-radius", str(self.bd), "a.cpp",
                               "--src-root", str(self.src), "--ninja", str(self.ninja)],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
        self.assertIn("instrument broken", proc.stderr)
        self.assertEqual(proc.stdout, "")

    def test_touched_compiled_source_seen_as_unchanged_is_refused(self) -> None:
        # The built-in control (b.cpp) passes; only the requested file is
        # invisible to the dry run, which must still refuse rather than print 0.
        os.environ["STUB_MODE"] = "blind-a"
        with self.assertRaises(btr.InstrumentBroken) as ctx:
            btr.blast_radius(self.bd, ["a.cpp"], self.src, str(self.ninja))
        self.assertIn("a.cpp", str(ctx.exception))
        self.assertEqual(os.stat(self.cpp).st_mtime_ns, self.saved)

    def test_short_plan_is_refused(self) -> None:
        os.environ["STUB_MODE"] = "short"
        with self.assertRaises(btr.InstrumentBroken):
            btr.blast_radius(self.bd, ["a.cpp"], self.src, str(self.ninja))

    def test_regeneration_left_in_is_refused(self) -> None:
        os.environ["STUB_MODE"] = "regen"
        with self.assertRaises(btr.InstrumentBroken):
            btr.blast_radius(self.bd, ["a.cpp"], self.src, str(self.ninja))

    def test_classify_description(self) -> None:
        cases = {
            "Building CXX object core/x/CMakeFiles/x.dir/a.cpp.o": "compile",
            "Building OBJCXX object core/x/CMakeFiles/x.dir/a.mm.o": "compile",
            "Linking CXX executable test/pulp-test-a": "test-exe link",
            "Linking CXX executable tools/cli/pulp-test-cli-bake": "exe link",
            "Linking CXX executable AUv3/PulpGain.appex/Contents/MacOS/PulpGain": "bundle link",
            "Linking CXX shared module examples/x/X.vst3/Contents/MacOS/X": "bundle link",
            "Linking CXX static library core/view/libpulp-view-core.a": "archive",
            "Ad-hoc signing raw trusted host fixture": "codesign/stamps",
            "Building pulp-rs (Rust CLI) via cargo (release profile)": "cargo",
            "Generating core/x/gen.cpp": "other",
        }
        for desc, cat in cases.items():
            self.assertEqual(btr.classify_description(desc), cat, desc)

    def test_parse_plan_accepts_no_work(self) -> None:
        self.assertEqual(sum(btr.parse_plan("ninja: no work to do.\n").values()), 0)


if __name__ == "__main__":
    unittest.main()
