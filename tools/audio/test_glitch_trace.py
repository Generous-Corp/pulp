#!/usr/bin/env python3
"""glitch_trace.py: the detectors find planted defects and stay quiet on clean
audio, and the timeline join maps a sample to the block that rendered it."""
from __future__ import annotations

import json
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import glitch_trace as gt  # noqa: E402

RATE = 48000.0


def tone(seconds: float) -> list[float]:
    n = int(seconds * RATE)
    return [0.3 * math.sin(2 * math.pi * 220.0 * i / RATE)
            + 0.2 * math.sin(2 * math.pi * 1375.0 * i / RATE + 0.3) for i in range(n)]


def write_float_wav(path: Path, x: list[float]) -> None:
    data = b"".join(struct.pack("<f", v) for v in x)
    header = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + b"fmt "
              + struct.pack("<IHHIIHH", 16, 3, 1, int(RATE), int(RATE) * 4, 4, 32)
              + b"data" + struct.pack("<I", len(data)))
    path.write_bytes(header + data)


class Detectors(unittest.TestCase):
    def test_clean_tone_has_no_events(self):
        # Negative control: a steady tone must not read as a glitch.
        x = tone(1.5)
        self.assertEqual(gt.detect_clicks(x, RATE, 30.0), [])
        self.assertEqual(gt.detect_dropouts(x, RATE, 20.0), [])

    def test_planted_step_is_a_click_at_its_sample(self):
        x = tone(1.5)
        at = 52000
        for i in range(at, len(x)):
            x[i] += 0.05  # a -26 dBFS step: a cut between two waveforms
        clicks = gt.detect_clicks(x, RATE, 30.0)
        self.assertTrue(clicks, "the planted step was not seen")
        self.assertLessEqual(abs(clicks[0].sample - at), 2)

    def test_planted_gap_is_a_dropout_of_its_length(self):
        x = tone(2.0)
        start, length = int(1.2 * RATE), int(0.1 * RATE)
        for i in range(start, start + length):
            x[i] = 0.0
        drops = gt.detect_dropouts(x, RATE, 20.0)
        self.assertEqual(len(drops), 1)
        self.assertLessEqual(abs(drops[0].sample - start), int(0.005 * RATE))
        self.assertLessEqual(abs(drops[0].length - length), int(0.01 * RATE))


class TimelineJoin(unittest.TestCase):
    def rows(self, block=256, n=20):
        return [{"ts": str(1000 + i * 5_333_333), "dur": str(200_000 if i != 7 else 9_000_000),
                 "stream_pos": str(i * block), "frames": str(block)} for i in range(n)]

    def test_sample_maps_to_its_block(self):
        blocks = gt.blocks_from_rows(self.rows())
        self.assertEqual(gt.locate(7 * 256 + 100, blocks).stream_pos, 7 * 256)
        self.assertEqual(gt.locate(0, blocks).stream_pos, 0)
        self.assertIsNone(gt.locate(20 * 256, blocks))

    def test_rows_without_args_give_no_blocks(self):
        # No stream_pos means no sample <-> time mapping; never guess one.
        rows = [{"ts": "1", "dur": "2", "stream_pos": "[NULL]", "frames": "[NULL]"}]
        self.assertEqual(gt.blocks_from_rows(rows), [])

    def test_deadline_miss_is_counted(self):
        stats = gt.block_stats(gt.blocks_from_rows(self.rows()), RATE)
        # 9 ms against a 256-frame (5.33 ms) deadline is the one miss.
        self.assertEqual(stats["deadline_misses"], 1)
        self.assertEqual(stats["worst_block_pos"], 7 * 256)


class EndToEnd(unittest.TestCase):
    """Through trace_processor, when one is installed (a Chrome JSON trace
    stands in for a .pftrace: same slice table, args under `args.`)."""

    def test_click_is_reported_with_its_block(self):
        processor = gt.resolve_processor()
        if not processor:
            self.skipTest("trace_processor not installed")
        with tempfile.TemporaryDirectory() as tmp:
            x = tone(1.5)
            at = 52000
            for i in range(at, len(x)):
                x[i] += 0.05
            wav = Path(tmp) / "out.wav"
            write_float_wav(wav, x)
            events = []
            for i in range(len(x) // 256):
                ts = 1000.0 + i * 5333.0
                events.append({"name": "process", "cat": "dsp", "ph": "X", "ts": ts, "dur": 150.0,
                               "pid": 1, "tid": 1, "args": {"stream_pos": i * 256, "frames": 256}})
                if i == at // 256:
                    events.append({"name": "render_switch", "cat": "dsp", "ph": "X", "ts": ts + 10,
                                   "dur": 100.0, "pid": 1, "tid": 1})
            trace = Path(tmp) / "out.json"
            trace.write_text(json.dumps({"traceEvents": events}))
            report = gt.analyse(wav, trace, 30.0, 20.0)
            self.assertEqual(report["block_time"]["deadline_misses"], 0)
            clicks = [e for e in report["events"] if e["kind"] == "click"]
            self.assertTrue(clicks)
            block = clicks[0]["block"]
            self.assertEqual(block["stream_pos"], (at // 256) * 256)
            self.assertIn("render_switch", [name for name, _ in block["children"]])


if __name__ == "__main__":
    unittest.main()
