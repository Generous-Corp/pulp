#!/usr/bin/env python3
"""Contract test for the eight-step sequencer's construction/reset order."""

from __future__ import annotations

import pathlib
import re
import unittest


VOICE = pathlib.Path(__file__).parents[1] / "src" / "voice.cpp"


class VoiceSequenceTests(unittest.TestCase):
    def test_first_clock_after_construction_and_reset_selects_step_one(self) -> None:
        source = VOICE.read_text()
        seq = source[source.index("struct SEQModule"):]
        seq = seq[:seq.index("// ── Widgets")]

        initial = int(re.search(r"int step_ = (-?\d+);", seq).group(1))
        reset = int(re.search(r"RESET_INPUT.*?step_ = (-?\d+);", seq,
                              re.DOTALL).group(1))
        steps = int(re.search(r"kSteps = (\d+);", seq).group(1))

        def clock(step: int) -> int:
            return (step + 1) % steps

        self.assertEqual([clock(initial)] + [clock(i) for i in range(steps - 1)],
                         list(range(steps)))
        self.assertEqual(clock(reset), 0)
        self.assertIn("step_ = (step_ + 1) % kSteps", seq)
        self.assertIn("active_step = std::max(step_, 0)", seq)
        self.assertIn("STEP1_PARAM + active_step", seq)


if __name__ == "__main__":
    unittest.main()
