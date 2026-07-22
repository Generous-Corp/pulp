#!/usr/bin/env python3
"""Unit tests for the sequence-loop-seek scraper + mode dispatch in reaper_smoke.py.

These test the parseable half of the harness with NO REAPER and NO plugin: they
feed synthetic REAPER stdout (the plugin's per-block transport markers) to the
pure `analyze_seq_loop_log` scraper and assert the PASS / FAIL / INCONCLUSIVE
verdict, and they exercise argument parsing / mode dispatch / the new lua's
structure. The end-to-end proof (loop + seek driven in a real REAPER against a
plugin that actually embeds a sequence) is gated behind REAPER-present and is NOT
covered here — that is Phase-2 DoD Proof #2, which needs the embedded-sequence
plugin to exist.

Includes a NEGATIVE case (a synthetic log that SHOULD fail) proving the scraper
actually detects a bad loop/seek, not just green-on-everything.

Run:
    python3 tools/testing/daw-smoke/test_reaper_smoke.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
MODULE_PATH = HERE / "reaper_smoke.py"
SEQ_LUA = HERE / "sequence_loop_seek.lua"


def _load_module():
    spec = importlib.util.spec_from_file_location("reaper_smoke", MODULE_PATH)
    mod = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(mod)
    return mod


rs = _load_module()


def blk(host, seq, active=1, jump=0, dropout=0):
    return (f"[seq-loop] blk host_qn={host:.3f} seq_qn={seq:.3f} "
            f"active={active} jump={jump} dropout={dropout}")


HEADER = ["[seq-loop] loaded events=4 len_qn=4.000", "[seq-loop] play"]


def good_blocks():
    """seq tracks host exactly; includes one forward seek (block 4) and one loop
    wrap (block 6). 9 blocks, all dropout=0, active=1."""
    return [
        blk(2.0, 2.0),               # 1
        blk(2.5, 2.5),               # 2
        blk(3.0, 3.0),               # 3
        blk(5.0, 5.0, jump=1),       # 4 forward seek (host>prev, jump)
        blk(5.5, 5.5),               # 5
        blk(2.0, 2.0, jump=1),       # 6 loop wrap  (host<prev, jump)
        blk(2.5, 2.5),               # 7
        blk(3.0, 3.0),               # 8
        blk(3.5, 3.5),               # 9
    ]


def make_log(blocks, header=HEADER):
    return "\n".join([*header, *blocks, ""])


class AnalyzePass(unittest.TestCase):
    def test_clean_run_passes(self):
        v = rs.analyze_seq_loop_log(make_log(good_blocks()))
        self.assertEqual(v.code, rs.EXIT_PASS, v.reason)
        self.assertIn("PASS", v.reason)

    def test_noise_around_markers_is_tolerated(self):
        # Real REAPER stdout is full of unrelated lines; the scraper must ignore them.
        noisy = ["REAPER v7.x", "scanning plugins...", *HEADER,
                 *good_blocks(), "some unrelated trailer", ""]
        v = rs.analyze_seq_loop_log("\n".join(noisy))
        self.assertEqual(v.code, rs.EXIT_PASS, v.reason)


class AnalyzeFail(unittest.TestCase):
    def test_drift_after_wrap_fails(self):
        # NEGATIVE: a free-running counter that ignores the host jump. After the
        # wrap at block 6 the host returns to 2.0 but seq keeps climbing → drift.
        blocks = [
            blk(2.0, 2.0),
            blk(2.5, 2.5),
            blk(3.0, 3.0),
            blk(5.0, 5.0, jump=1),   # seek, still tracking
            blk(5.5, 5.5),
            blk(2.0, 6.0, jump=1),   # wrap: host->2.0 but seq free-ran to 6.0
            blk(2.5, 6.5),
            blk(3.0, 7.0),
            blk(3.5, 7.5),
        ]
        v = rs.analyze_seq_loop_log(make_log(blocks))
        self.assertEqual(v.code, rs.EXIT_FAIL, v.reason)
        self.assertIn("DRIFT", v.reason)

    def test_dropout_on_reposition_fails(self):
        # NEGATIVE: correct tracking, but the wrap block underran (dropout=1).
        blocks = good_blocks()
        blocks[5] = blk(2.0, 2.0, jump=1, dropout=1)  # the wrap block dropped out
        v = rs.analyze_seq_loop_log(make_log(blocks))
        self.assertEqual(v.code, rs.EXIT_FAIL, v.reason)
        self.assertIn("DROPOUT", v.reason)

    def test_tiny_drift_within_tolerance_still_passes(self):
        # Block-quantized jitter under the tolerance is not a failure.
        blocks = good_blocks()
        blocks[0] = blk(2.0, 2.02)  # 0.02 qn < default 0.05 tol
        v = rs.analyze_seq_loop_log(make_log(blocks))
        self.assertEqual(v.code, rs.EXIT_PASS, v.reason)

    def test_drift_just_over_tolerance_fails(self):
        blocks = good_blocks()
        blocks[0] = blk(2.0, 2.06)  # 0.06 qn > default 0.05 tol
        v = rs.analyze_seq_loop_log(make_log(blocks))
        self.assertEqual(v.code, rs.EXIT_FAIL, v.reason)


class AnalyzeInconclusive(unittest.TestCase):
    def test_no_loaded_marker(self):
        v = rs.analyze_seq_loop_log(make_log(good_blocks(),
                                             header=["[seq-loop] play"]))
        self.assertEqual(v.code, rs.EXIT_INCONCLUSIVE, v.reason)
        self.assertIn("never loaded", v.reason)

    def test_empty_sequence(self):
        header = ["[seq-loop] loaded events=0 len_qn=0.000", "[seq-loop] play"]
        v = rs.analyze_seq_loop_log(make_log(good_blocks(), header=header))
        self.assertEqual(v.code, rs.EXIT_INCONCLUSIVE, v.reason)
        self.assertIn("EMPTY", v.reason)

    def test_no_play_marker(self):
        v = rs.analyze_seq_loop_log(make_log(good_blocks(),
                                             header=["[seq-loop] loaded events=4 len_qn=4.0"]))
        self.assertEqual(v.code, rs.EXIT_INCONCLUSIVE, v.reason)
        self.assertIn("transport never started", v.reason)

    def test_too_few_blocks(self):
        v = rs.analyze_seq_loop_log(make_log(good_blocks()[:3]))
        self.assertEqual(v.code, rs.EXIT_INCONCLUSIVE, v.reason)
        self.assertIn("not enough", v.reason)

    def test_no_loop_or_seek_coverage(self):
        # Monotonic advance, no jump=1 anywhere → neither a wrap nor a seek was
        # exercised → cannot prove the behavior.
        blocks = [blk(2.0 + 0.5 * i, 2.0 + 0.5 * i) for i in range(9)]
        v = rs.analyze_seq_loop_log(make_log(blocks))
        self.assertEqual(v.code, rs.EXIT_INCONCLUSIVE, v.reason)
        self.assertIn("did not exercise BOTH", v.reason)

    def test_wrap_but_no_seek_is_inconclusive(self):
        # Has a loop wrap but never a forward seek → coverage incomplete.
        blocks = [
            blk(2.0, 2.0), blk(2.5, 2.5), blk(3.0, 3.0), blk(3.5, 3.5),
            blk(2.0, 2.0, jump=1),  # wrap only
            blk(2.5, 2.5), blk(3.0, 3.0), blk(3.5, 3.5), blk(4.0, 4.0),
        ]
        v = rs.analyze_seq_loop_log(make_log(blocks))
        self.assertEqual(v.code, rs.EXIT_INCONCLUSIVE, v.reason)
        self.assertIn("did not exercise BOTH", v.reason)

    def test_no_note_activity(self):
        # Correct tracking + full coverage + no dropout, but the sequence never
        # produced a note → we never observed it play → INCONCLUSIVE (not PASS).
        blocks = [blk(b_host, b_seq, active=0, jump=j)
                  for (b_host, b_seq, j) in (
                      (2.0, 2.0, 0), (2.5, 2.5, 0), (3.0, 3.0, 0),
                      (5.0, 5.0, 1), (5.5, 5.5, 0), (2.0, 2.0, 1),
                      (2.5, 2.5, 0), (3.0, 3.0, 0), (3.5, 3.5, 0))]
        v = rs.analyze_seq_loop_log(make_log(blocks))
        self.assertEqual(v.code, rs.EXIT_INCONCLUSIVE, v.reason)
        self.assertIn("active notes", v.reason)


class ArgParsing(unittest.TestCase):
    def test_mode_choice_present(self):
        ap = rs.build_parser()
        args = ap.parse_args(["--mode", "sequence-loop-seek",
                              "--plugin-name", "Pulp Sequence",
                              "--plugin-path", "/nonexistent.vst3"])
        self.assertEqual(args.mode, "sequence-loop-seek")
        # Defaults wired.
        self.assertEqual(args.loop_start, 1.0)
        self.assertEqual(args.loop_end, 3.0)
        self.assertEqual(args.pos_tolerance_qn, 0.05)

    def test_loop_end_must_exceed_start(self):
        ap = rs.build_parser()
        args = ap.parse_args(["--mode", "sequence-loop-seek",
                              "--plugin-name", "X", "--plugin-path", "/x.vst3",
                              "--loop-start", "3.0", "--loop-end", "1.0"])
        with self.assertRaises(SystemExit):
            rs.validate_mode_args(ap, args)

    def test_valid_seq_args_pass_validation(self):
        ap = rs.build_parser()
        args = ap.parse_args(["--mode", "sequence-loop-seek",
                              "--plugin-name", "X", "--plugin-path", "/x.vst3"])
        rs.validate_mode_args(ap, args)  # must not raise

    def test_reload_mode_still_validates(self):
        # Regression: the existing modes' validation is unchanged.
        ap = rs.build_parser()
        args = ap.parse_args(["--mode", "reload",
                              "--plugin-name", "X", "--plugin-path", "/x.vst3"])
        with self.assertRaises(SystemExit):
            rs.validate_mode_args(ap, args)

    def test_help_is_a_clean_dry_run(self):
        # `--help` is the no-REAPER dry-run path; it must exit 0.
        cp = subprocess.run([sys.executable, str(MODULE_PATH), "--help"],
                            capture_output=True, text=True)
        self.assertEqual(cp.returncode, 0, cp.stderr)
        self.assertIn("sequence-loop-seek", cp.stdout)


class LuaStructure(unittest.TestCase):
    def test_lua_exists(self):
        self.assertTrue(SEQ_LUA.exists())

    def test_lua_drives_loop_and_seeks_and_handshakes(self):
        text = SEQ_LUA.read_text()
        for token in (
            "TrackFX_AddByName",       # inserts the FX
            "TrackFX_Show",            # floats the editor
            "GetSet_LoopTimeRange",    # sets the loop region
            "GetSetRepeat",            # enables repeat/loop
            "OnPlayButton",            # starts playback
            "SetEditCurPos",           # seeks the transport
            "OnStopButton",            # stops
            "reaper.defer",            # the deferred drive pump
            "FX_SHOWN",                # handshake: FX inserted
            "FX_NOT_FOUND",            # handshake: scan/insert failed
            "SEEKS_DONE",              # handshake: scripted drive complete
        ):
            self.assertIn(token, text, f"lua missing {token}")

    def test_lua_reads_loop_env(self):
        text = SEQ_LUA.read_text()
        self.assertIn("PULP_DAW_SMOKE_LOOP_START", text)
        self.assertIn("PULP_DAW_SMOKE_LOOP_END", text)


if __name__ == "__main__":
    unittest.main()
