#!/usr/bin/env python3
"""Self-check for frame_stamp_decode.py.

Without arguments it runs the stdlib-only checks: synthetic stamps encoded
here decode to their index, a corrupted cell or a bit equal to its
complement decodes to -1, and the sequence scoring reports exact, held,
skipped, unknown, judder and regressed on hand-built index sequences,
including the frozen-scene negative (every capture shows the same update).

With `--cpp-frames DIR` it also decodes the PNG sequences that the C++
frame-stamp test rendered through pulp::view (`live/` and `frozen/`), so a
layout drift between the painter and this decoder fails here. That part
needs Pillow and exits 3 (ctest SKIP_RETURN_CODE) without it.

Returns 0 on success, 1 on a failed check, 3 when Pillow is missing.
"""

from __future__ import annotations

import argparse
import sys
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR.parent.parent.parent) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR.parent.parent.parent))

from tools.motion.visual import frame_stamp_decode as fsd  # noqa: E402

CPP_FRAMES: Path | None = None


def encode(index: int, scale: int = 2, layout: fsd.StampLayout = fsd.StampLayout(),
           width: int | None = None, height: int | None = None) -> tuple[bytearray, int, int]:
    """Paint a stamp the way test/support/frame_stamp.cpp does, as RGBA8."""
    side = int(layout.cell * scale)
    width = width or int((layout.x + layout.bits * layout.cell) * scale)
    height = height or int((layout.y + 2 * layout.cell) * scale)
    rgba = bytearray([40, 40, 40, 255] * (width * height))
    value = index & ((1 << layout.bits) - 1)
    for col in range(layout.bits):
        bit = (value >> (layout.bits - 1 - col)) & 1
        for row in (0, 1):
            lit = bit if row == 0 else 1 - bit
            fill_cell(rgba, width, layout, scale, col, row, 255 if lit else 0, side)
    return rgba, width, height


def fill_cell(rgba: bytearray, width: int, layout: fsd.StampLayout, scale: int,
              col: int, row: int, level: int, side: int) -> None:
    x0 = int((layout.x + col * layout.cell) * scale)
    y0 = int((layout.y + row * layout.cell) * scale)
    for y in range(y0, y0 + side):
        for x in range(x0, x0 + side):
            p = (y * width + x) * 4
            rgba[p:p + 3] = bytes([level, level, level])


class DecodeTests(unittest.TestCase):
    def test_round_trip(self):
        for index in (0, 1, 2, 255, 0xABCD, 0xFFFF):
            rgba, w, h = encode(index)
            self.assertEqual(fsd.decode_rgba(bytes(rgba), w, h, 2), index)

    def test_offset_layout_and_scale_one(self):
        layout = fsd.StampLayout(x=5, y=3, cell=6, bits=12)
        rgba, w, h = encode(0x5A5, scale=1, layout=layout)
        self.assertEqual(fsd.decode_rgba(bytes(rgba), w, h, 1, layout), 0x5A5)

    def test_grey_cell_is_unreadable_not_guessed(self):
        rgba, w, h = encode(0x1234)
        fill_cell(rgba, w, fsd.StampLayout(), 2, 7, 0, 128, 16)
        self.assertEqual(fsd.decode_rgba(bytes(rgba), w, h, 2), -1)

    def test_bit_agreeing_with_its_complement_is_unreadable(self):
        # Flip only the bit row of one column: both rows now agree. A decoder
        # reading row 0 alone would return a wrong index instead of -1.
        rgba, w, h = encode(0)
        fill_cell(rgba, w, fsd.StampLayout(), 2, 3, 0, 255, 16)
        self.assertEqual(fsd.decode_rgba(bytes(rgba), w, h, 2), -1)

    def test_grid_outside_the_image_is_unreadable(self):
        rgba, w, h = encode(7)
        self.assertEqual(fsd.decode_rgba(bytes(rgba), w // 2, h, 2), -1)

    def test_blank_frame_is_unreadable(self):
        self.assertEqual(fsd.decode_rgba(bytes(64 * 64 * 4), 64, 64, 1), -1)


class AnalyzeTests(unittest.TestCase):
    def test_every_update_on_time_is_exact(self):
        r = fsd.analyze(list(range(10, 20)))
        self.assertEqual((r.exact, r.exact_pct, r.held, r.skipped, r.judder),
                         (10, 100.0, 0, 0, 0))
        self.assertEqual(r.problems, [])

    def test_frozen_scene_is_held_not_exact(self):
        # The planted negative: a retained scene that stopped refreshing shows
        # the same update at every capture.
        r = fsd.analyze([5] * 12)
        self.assertEqual(r.held, 11)
        self.assertEqual(r.exact, 1)
        self.assertEqual(r.skipped, 11)
        self.assertEqual(r.distinct, 1)
        self.assertIn("held", r.problems)
        self.assertLess(r.exact_pct, 10.0)

    def test_one_held_capture_then_catch_up(self):
        r = fsd.analyze([0, 1, 1, 3, 4])
        self.assertEqual((r.exact, r.held, r.skipped, r.judder), (4, 1, 1, 0))

    def test_unreadable_capture_is_unknown_not_skipped(self):
        r = fsd.analyze([0, 1, -1, 3])
        self.assertEqual((r.unreadable, r.exact, r.skipped, r.unknown, r.held),
                         (1, 3, 0, 1, 0))
        self.assertEqual(r.problems, ["unknown", "unreadable"])

    def test_all_unreadable_reports_only_unreadable(self):
        r = fsd.analyze([-1, -1])
        self.assertEqual((r.readable, r.exact, r.start_index), (0, 0, None))
        self.assertEqual(r.problems, ["unreadable"])

    def test_wraparound_is_an_advance_not_a_regression(self):
        r = fsd.analyze([65534, 65535, 0, 1], bits=16)
        self.assertEqual((r.exact, r.regressed, r.skipped, r.judder), (4, 0, 0, 0))
        self.assertEqual(r.problems, [])

    def test_regression_is_reported(self):
        r = fsd.analyze([0, 1, 2, 1, 4])
        self.assertEqual(r.regressed, 1)

    def test_judder_from_timestamps(self):
        # 60 Hz captures, one late by a full period.
        times = [0.0, 16.7, 33.3, 66.7, 83.3]
        r = fsd.analyze([0, 1, 2, 3, 4], timestamps_ms=times, refresh_ms=16.7)
        self.assertEqual((r.exact, r.judder), (5, 1))

    def test_step_schedules_due_updates(self):
        r = fsd.analyze([0, 2, 4, 6], step=2)
        self.assertEqual((r.exact, r.skipped, r.judder), (4, 0, 0))
        r = fsd.analyze([0, 2, 2, 6], step=2)
        self.assertEqual((r.exact, r.held, r.skipped), (3, 1, 1))


class CppFramesTests(unittest.TestCase):
    """Decode PNGs the C++ painter rendered; the layouts must agree."""

    def setUp(self):
        if CPP_FRAMES is None:
            self.skipTest("no --cpp-frames")

    def decode(self, name: str) -> list[int]:
        paths = sorted((CPP_FRAMES / name).glob("frame_*.png"))
        self.assertGreater(len(paths), 2, f"no frames under {CPP_FRAMES / name}")
        return fsd.decode_files(paths, scale=2.0)

    def test_live_sequence_decodes_exactly(self):
        indices = self.decode("live")
        r = fsd.analyze(indices)
        self.assertEqual(r.unreadable, 0, indices)
        self.assertEqual(r.exact_pct, 100.0, indices)
        self.assertEqual(r.problems, [])

    def test_frozen_sequence_is_reported_held(self):
        indices = self.decode("frozen")
        r = fsd.analyze(indices, start_index=self.decode("live")[0])
        self.assertEqual(r.unreadable, 0, indices)
        self.assertEqual(r.distinct, 1, indices)
        self.assertEqual(r.held, len(indices) - 1)
        self.assertIn("held", r.problems)
        self.assertLess(r.exact_pct, 100.0)


def main() -> int:
    global CPP_FRAMES
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-frames", type=Path, default=None)
    args, rest = parser.parse_known_args()
    if args.cpp_frames is not None:
        try:
            import PIL  # noqa: F401
        except ImportError as exc:
            print(f"test_frame_stamp_decode: Pillow missing ({exc})", file=sys.stderr)
            return 3
        CPP_FRAMES = args.cpp_frames
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(CppFramesTests)
    else:
        suite = unittest.TestSuite([
            unittest.defaultTestLoader.loadTestsFromTestCase(DecodeTests),
            unittest.defaultTestLoader.loadTestsFromTestCase(AnalyzeTests),
        ])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
