import copy
import json
import unittest
from pathlib import Path

from check_trace import check


class TraceOracleTest(unittest.TestCase):
    def setUp(self):
        self.positive = json.loads(Path(__file__).with_name("expected.json").read_text())

    def test_frozen_positive(self):
        self.assertEqual(check(self.positive), [])

    def test_planted_one_sample_shift(self):
        observed = copy.deepcopy(self.positive)
        observed["partitions"][1]["events"][0]["sample"] = 1
        self.assertIn("partition 1: events differs from frozen oracle", check(observed))

    def test_planted_reversed_tie(self):
        observed = copy.deepcopy(self.positive)
        events = observed["partitions"][0]["events"]
        events[0], events[1] = events[1], events[0]
        self.assertIn("partition 0: events differs from frozen oracle", check(observed))

    def test_planted_ninth_valid_accepted(self):
        observed = copy.deepcopy(self.positive)
        observed["partitions"][0]["sample_zero"]["accepted"] = 9
        self.assertIn("partition 0: sample_zero differs from frozen oracle", check(observed))


if __name__ == "__main__":
    unittest.main()
