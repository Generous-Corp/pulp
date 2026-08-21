import importlib.util
from pathlib import Path
import sys
import unittest


MODULE_PATH = Path(__file__).with_name("check_transient_interaction_trace.py")
SPEC = importlib.util.spec_from_file_location("check_transient_interaction_trace", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class TransientInteractionTraceBudgetTests(unittest.TestCase):
    def test_query_escapes_slice_names_and_emits_machine_marker(self):
        query = MODULE.build_query("input's", "layout_root")
        self.assertIn("name = 'input''s'", query)
        self.assertIn(MODULE.MARKER, query)

    def test_parse_and_green_budget(self):
        metrics = MODULE.parse_metrics(
            f"box | {MODULE.MARKER}|177|0.641000|238|1.344633 |")
        self.assertEqual(metrics.input_count, 177)
        self.assertEqual(MODULE.budget_failures(metrics, 4.0, 2.0), [])

    def test_planted_slow_and_redundant_work_fails(self):
        metrics = MODULE.Metrics(
            input_count=177,
            input_p95_ms=18.557,
            work_count=641,
            work_per_input=3.621,
        )
        failures = MODULE.budget_failures(metrics, 4.0, 2.0)
        self.assertEqual(len(failures), 2)
        self.assertIn("input p95", failures[0])
        self.assertIn("work/input", failures[1])

    def test_empty_trace_never_passes(self):
        metrics = MODULE.Metrics(0, 0.0, 0, 0.0)
        self.assertEqual(
            MODULE.budget_failures(metrics, 4.0, 2.0),
            ["trace contains no matching input slices"],
        )


if __name__ == "__main__":
    unittest.main()
