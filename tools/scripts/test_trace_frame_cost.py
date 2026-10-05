"""trace_frame_cost.py: parsing, percentiles, grouping, budgets and the
refusals that keep an empty or truncated capture from reading as a pass."""

import importlib.util
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest

MODULE_PATH = Path(__file__).with_name("trace_frame_cost.py")
SPEC = importlib.util.spec_from_file_location("trace_frame_cost", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)
REPO = MODULE_PATH.parents[2]
FIXTURE = REPO / "test/fixtures/perfetto-gpu/render-only.pftrace"


def row(group, frame_ms, layout=0.0, repaints=0, layouts=0, area=0.0):
    stages = [layout] + [0.0] * (len(MODULE.STAGES) - 1)
    return "|".join([MODULE.MARKER, "frame", str(group), f"{frame_ms:.6f}"]
                    + [f"{v:.6f}" for v in stages] + [str(repaints), str(layouts), f"{area:.1f}"])


class FakeProcessor:
    """A trace_processor stand-in that prints canned rows."""

    def __init__(self, lines):
        self.dir = tempfile.TemporaryDirectory()
        self.path = Path(self.dir.name) / "tp"
        self.path.write_text("#!/bin/sh\ncat <<'X'\n\"col\"\n"
                             + "\n".join(f'"{l}"' for l in lines) + "\nX\n")
        self.path.chmod(self.path.stat().st_mode | stat.S_IEXEC)
        self.trace = Path(self.dir.name) / "t.pftrace"
        self.trace.write_bytes(b"x")

    def run(self, *extra):
        return MODULE.main(["--trace", str(self.trace), "--processor", str(self.path),
                            "--frame", "f", *extra])


class TraceFrameCostTests(unittest.TestCase):
    def test_query_escapes_names_and_counts_self_time(self):
        query = MODULE.build_query("o'frame", "scen")
        self.assertIn("name = 'o''frame'", query)
        self.assertIn("s.dur - COALESCE(cs.dur, 0)", query)
        self.assertIn("view_repaint_request", query)

    def test_parse_rows_and_truncation_stats(self):
        frames, stats = MODULE.parse("\n".join([
            '"' + row(0, 1.5, layout=0.25, repaints=2, layouts=1) + '"',
            f'"{MODULE.MARKER}|stat|traced_buf_write_wrap_count|3"']))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].stages["layout"], 0.25)
        self.assertEqual(frames[0].repaints, 2)
        self.assertEqual(stats, {"traced_buf_write_wrap_count": 3})

    def test_nearest_rank_percentile(self):
        values = [float(v) for v in range(1, 101)]
        self.assertEqual(MODULE.percentile(values, 50), 50.0)
        self.assertEqual(MODULE.percentile(values, 95), 95.0)
        self.assertEqual(MODULE.percentile([7.0], 95), 7.0)

    def test_green_budget_relative_to_baseline(self):
        lines = [row(0, 0.1)] * 20 + [row(1, 0.2, repaints=0)] * 20
        self.assertEqual(FakeProcessor(lines).run(
            "--group", "g", "--labels", "idle,knobs", "--baseline", "idle",
            "--max-p95-ms", "0.5", "--max-layout-frames", "0"), 0)

    def test_planted_layout_and_slow_frames_fail(self):
        # The negative control: a scenario that re-lays the tree and costs
        # 20 ms per frame must breach both budgets.
        lines = [row(0, 0.1)] * 20 + [row(1, 20.0, layout=5.0, layouts=1)] * 20
        self.assertEqual(FakeProcessor(lines).run(
            "--group", "g", "--labels", "idle,knobs", "--baseline", "idle",
            "--max-p95-ms", "0.5", "--max-layout-frames", "0"), 1)

    def test_damage_budget(self):
        # Knobs repainting their own 30 x 30 boxes pass a 1 % budget on a
        # 400 x 300 surface; the negative control -- the same frames asking
        # for the whole surface once each -- fails it.
        bounded = [row(0, 0.1)] * 20 + [row(1, 0.1, area=4 * 900.0)] * 20
        self.assertEqual(FakeProcessor(bounded).run(
            "--group", "g", "--labels", "idle,knobs", "--surface", "400x300",
            "--max-damage-frac", "0.05"), 0)
        whole = [row(0, 0.1)] * 20 + [row(1, 0.1, repaints=1, area=4 * 900.0)] * 20
        self.assertEqual(FakeProcessor(whole).run(
            "--group", "g", "--labels", "idle,knobs", "--surface", "400x300",
            "--max-damage-frac", "0.05"), 1)
        # A damage budget with no surface to measure against is refused.
        self.assertEqual(FakeProcessor(bounded).run("--max-damage-frac", "0.05"), 1)

    def test_empty_or_truncated_capture_never_passes(self):
        self.assertEqual(FakeProcessor([]).run(), 2)
        self.assertEqual(FakeProcessor(
            [row(0, 0.1), f"{MODULE.MARKER}|stat|traced_buf_write_wrap_count|1"]).run(), 2)

    def test_real_trace_processor_on_a_fixture(self):
        processor = MODULE.resolve_processor("")
        if not processor or not FIXTURE.is_file():
            self.skipTest("trace_processor not installed (pulp trace fetch)")
        frames, stats = MODULE.parse(MODULE.run_processor(
            processor, FIXTURE, MODULE.build_query("frame", "")))
        self.assertEqual(stats, {})
        self.assertEqual(len(frames), 1)
        self.assertGreater(frames[0].frame_ms, 0.0)


if __name__ == "__main__":
    unittest.main()
