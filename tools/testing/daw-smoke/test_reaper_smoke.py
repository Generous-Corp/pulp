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

import argparse
import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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


class EditorOpenMode(unittest.TestCase):
    """The fourth mode: insert, open the editor, confirm it rendered.

    Products whose editor IS the product need this and none of the hot-swap or
    transport scenarios. auval and clap-validator prove a plugin scans and
    instantiates; neither proves its window comes up.
    """

    def test_mode_is_offered(self):
        ap = rs.build_parser()
        args = ap.parse_args(["--mode", "editor-open",
                              "--plugin-name", "Forge Modular",
                              "--plugin-path", __file__])
        self.assertEqual(args.mode, "editor-open")

    def test_needs_nothing_but_the_plugin(self):
        # The other modes each demand extra flags. Requiring any here would
        # defeat the point: the question is only whether the thing loads and
        # draws.
        ap = rs.build_parser()
        args = ap.parse_args(["--mode", "editor-open",
                              "--plugin-name", "Forge Modular",
                              "--plugin-path", __file__])
        rs.validate_mode_args(ap, args)   # must not raise or exit

    def test_a_missing_plugin_is_not_a_pass(self):
        ap = rs.build_parser()
        args = ap.parse_args(["--mode", "editor-open",
                              "--plugin-name", "Nope",
                              "--plugin-path", "/tmp/definitely-not-here.vst3"])
        rc = rs.run_editor_open_mode(pathlib.Path("/nonexistent/REAPER"), args)
        self.assertNotEqual(rc, rs.EXIT_PASS)


class AuAlreadyInstalled(unittest.TestCase):
    """An AU that is already installed is the case worth proving, not a clash.

    The AU leg copies the component into ~/Library/Audio/Plug-Ins/Components
    and removes it again afterwards, and it refused outright when something
    was already there -- a guard against deleting somebody's real plugin on
    teardown. But the component being there IS the normal state: it is how the
    host finds it, and it is what a signed, notarized, installed AU looks like.
    So the AU leg could not run on any machine anyone would actually test on,
    and reported FAIL for a reason that had nothing to do with the plugin.

    Now the same path is used in place and NOT uninstalled; a different path
    still refuses.
    """

    def _prep(self, plugin_path):
        import argparse
        args = argparse.Namespace(format="au", plugin_path=str(plugin_path),
                                  mode="editor-open", plugin_name="X")
        smoke = rs.ReaperSession.__new__(rs.ReaperSession)
        smoke.args = args
        smoke.au_installed = None
        return smoke

    def test_the_installed_component_is_used_in_place(self):
        with tempfile.TemporaryDirectory() as home:
            comps = pathlib.Path(home) / "Library/Audio/Plug-Ins/Components"
            comps.mkdir(parents=True)
            comp = comps / "Forge Modular.component"
            comp.mkdir()
            smoke = self._prep(comp)
            with mock.patch.dict(os.environ, {"HOME": home}):
                smoke.portable = pathlib.Path(home) / "portable"
                smoke.scan_dir = pathlib.Path(home) / "scan"
                smoke.portable.mkdir(); smoke.scan_dir.mkdir()
                smoke.args.plugin_path = str(comp)
                rc = rs.ReaperSession.place_plugin(smoke)
            self.assertIsNone(rc, "an already-installed AU must not be refused")
            self.assertIsNone(smoke.au_installed,
                              "teardown must not uninstall what it did not install")
            self.assertTrue(comp.exists())

    def test_a_different_component_still_refuses(self):
        with tempfile.TemporaryDirectory() as home:
            comps = pathlib.Path(home) / "Library/Audio/Plug-Ins/Components"
            comps.mkdir(parents=True)
            (comps / "Forge Modular.component").mkdir()
            other = pathlib.Path(home) / "build" / "Forge Modular.component"
            other.mkdir(parents=True)
            smoke = self._prep(other)
            with mock.patch.dict(os.environ, {"HOME": home}):
                smoke.portable = pathlib.Path(home) / "portable"
                smoke.scan_dir = pathlib.Path(home) / "scan"
                smoke.portable.mkdir(); smoke.scan_dir.mkdir()
                smoke.args.plugin_path = str(other)
                rc = rs.ReaperSession.place_plugin(smoke)
            self.assertEqual(rc, rs.EXIT_FAIL,
                             "a DIFFERENT build must still not clobber an install")


class EditorBuildMode(unittest.TestCase):
    """Pressing Build INSIDE the host, which editor-open does not do.

    editor-open proves the window comes up. It cannot prove the product works
    there: the generator is spawned BY the plugin, and a plugin whose editor
    draws perfectly can still never reach it -- the standalone did exactly
    that once, because an app launched from Finder inherits no PATH. Only a
    press inside the host tests that path.
    """

    def test_mode_is_offered_and_needs_nothing_extra(self):
        ap = rs.build_parser()
        args = ap.parse_args(["--mode", "editor-build",
                              "--plugin-name", "Forge Modular",
                              "--plugin-path", __file__])
        self.assertEqual(args.mode, "editor-build")
        rs.validate_mode_args(ap, args)   # must not raise or exit

    def test_it_does_not_drive_the_screen_at_all(self):
        """No clicks, no keystrokes, no screen coordinates.

        The earlier version of this mode pressed the editor's Build button by
        posting synthetic events at computed coordinates. It typed a prompt
        into somebody's terminal twice -- once believing "REAPER is frontmost"
        while its window sat buried, once taking that terminal's dark
        background for the plugin's editor. Each guard was correct and each
        was defeated by the next assumption.

        The claim being proven is that the generator runs when the PLUGIN
        spawns it, inheriting the host's environment. That is just as true
        asked through a file as asked through a button, and a file cannot
        land on somebody else's window.
        """
        src = open(rs.__file__ if hasattr(rs, "__file__")
                   else MODULE_PATH).read()
        body = src[src.index("def run_editor_build_mode"):]
        body = body[:body.index("\ndef ", 1)]
        for banned in ("uidriver", "screencapture", "click(", "\"type\"",
                       "AXRaise"):
            self.assertNotIn(banned, body,
                             f"editor-build still reaches for {banned!r} — "
                             f"it must not drive the screen")


class FormatIsAsked(unittest.TestCase):
    """--format has to decide which plugin REAPER inserts.

    TrackFX_AddByName with a bare name lets REAPER pick whichever format it
    finds first. A crash report from a `--format clap` run named
    com.generous.forge.modular.au, so the CLAP leg had been proving the AU --
    and the three format legs were one leg wearing three hats.
    """

    def test_each_format_asks_for_that_format(self):
        import argparse
        seen = {}
        for fmt, prefix in (("vst3", "VST3:"), ("clap", "CLAP:"), ("au", "AU:")):
            args = argparse.Namespace(format=fmt, plugin_name="Forge Modular",
                                      mode="editor-open", plugin_path=__file__,
                                      timeout=30)
            env = rs._common_env(args, pathlib.Path("/tmp/status"))
            seen[fmt] = env["PULP_DAW_SMOKE_FX"]
            self.assertTrue(env["PULP_DAW_SMOKE_FX"].startswith(prefix),
                            f"{fmt} asked for {env['PULP_DAW_SMOKE_FX']!r}")
        self.assertEqual(len(set(seen.values())), 3,
                         f"the three formats must ask for three things: {seen}")



class DoesNotWreckSomebodysReaper(unittest.TestCase):
    """This harness ships in Pulp. It must be safe on a machine someone uses.

    Two faults did real damage before these existed. It launched REAPER with an
    EMPTY portable config, so every run came up as a fresh install -- licence
    prompt, first-run preferences, audio-hardware setup, again and again. And
    it ran `pkill -x REAPER`, killing the session the person at the keyboard
    was working in, which read to them as REAPER restarting by itself.
    """

    def _session(self, home):
        import argparse
        s = rs.ReaperSession.__new__(rs.ReaperSession)
        s.args = argparse.Namespace(format="clap", plugin_path=str(home),
                                    plugin_name="X", mode="editor-open")
        s.portable = pathlib.Path(home) / "portable"
        s.scan_dir = pathlib.Path(home) / "scan"
        s.portable.mkdir(parents=True)
        s.scan_dir.mkdir(parents=True)
        s.au_installed = None
        return s

    def test_the_users_settings_are_carried_not_discarded(self):
        with tempfile.TemporaryDirectory() as home:
            real = pathlib.Path(home) / "Library/Application Support/REAPER"
            real.mkdir(parents=True)
            (real / "reaper.ini").write_text(
                "[REAPER]\naudioconfig=coreaudio\naudio_device=Built-in\n"
                "vstpath=/somewhere/of/theirs\n[moreprefs]\nfoo=bar\n")
            (real / "reaper-license.rk").write_text("LICENCE")
            plugin = pathlib.Path(home) / "Thing.clap"
            plugin.mkdir()
            s = self._session(home)
            s.args.plugin_path = str(plugin)
            with mock.patch.dict(os.environ, {"HOME": home}):
                rs.ReaperSession.place_plugin(s)
            ini = (s.portable / "reaper.ini").read_text()

            # Their audio device and their other preferences survive, so
            # REAPER does not ask for them again.
            self.assertIn("audioconfig=coreaudio", ini)
            self.assertIn("audio_device=Built-in", ini)
            self.assertIn("foo=bar", ini)
            # Their licence travels, so it does not prompt for one.
            self.assertTrue((s.portable / "reaper-license.rk").exists())
            # Only the scan paths are ours.
            self.assertIn(str(s.scan_dir), ini)
            self.assertNotIn("/somewhere/of/theirs", ini)
            # And their real config is never written to.
            self.assertEqual((real / "reaper.ini").read_text().count("[REAPER]"), 1)
            self.assertNotIn(str(s.scan_dir), (real / "reaper.ini").read_text())

    def test_it_still_works_with_no_reaper_config_at_all(self):
        with tempfile.TemporaryDirectory() as home:
            plugin = pathlib.Path(home) / "Thing.clap"
            plugin.mkdir()
            s = self._session(home)
            s.args.plugin_path = str(plugin)
            with mock.patch.dict(os.environ, {"HOME": home}):
                rs.ReaperSession.place_plugin(s)
            ini = (s.portable / "reaper.ini").read_text()
            self.assertIn("[REAPER]", ini)
            self.assertIn(str(s.scan_dir), ini)

    def test_it_does_not_kill_a_reaper_it_did_not_start(self):
        with mock.patch.object(rs.subprocess, "run") as run:
            rs.kill_reaper()
        for call in run.call_args_list:
            self.assertNotIn("pkill", call.args[0],
                             "killed every REAPER, including somebody's session")

    def test_it_does_kill_the_one_it_started(self):
        with mock.patch.object(rs.subprocess, "run") as run:
            rs.kill_reaper(only_pid=4321)
        self.assertTrue(any("4321" in " ".join(c.args[0])
                            for c in run.call_args_list))


if __name__ == "__main__":
    unittest.main()


def test_seeded_config_never_reopens_the_users_project(tmp_path, monkeypatch):
    """The smoke must not open somebody's session, or list their filenames.

    Seeding the portable config from the real reaper.ini is deliberate — it is
    what carries the licence and the chosen audio device, so a run does not
    put a first-run wizard on screen. But it also carried `lastproject`, so an
    "isolated" REAPER opened the user's actual project; that project referenced
    a plugin the smoke had not installed, REAPER came up on a modal Project
    Load Warning, the scripted insert never ran, and the smoke reported
    INCONCLUSIVE while naming neither the dialog nor the project.
    """
    real = tmp_path / "REAPER"
    real.mkdir()
    (real / "reaper.ini").write_text(
        "[REAPER]\n"
        "lastproject=/Users/someone/Desktop/Private Session.RPP\n"
        "loadlastproj=1\n"
        "audiodevice=CoreAudio\n"
        "[Recent]\n"
        "recent01=/Users/someone/Desktop/Another Private.RPP\n"
        "recent02=/Users/someone/Music/Unreleased.RPP\n"
        "[MoreStuff]\n"
        "keepme=1\n")
    monkeypatch.setattr(os.path, "expanduser", lambda p: p.replace(
        "~/Library/Application Support/REAPER", str(real)).replace(
        "~", str(tmp_path)))

    plugin = tmp_path / "Thing.vst3"
    plugin.mkdir()
    args = argparse.Namespace(plugin_path=str(plugin), format="vst3")
    session = rs.ReaperSession.__new__(rs.ReaperSession)
    session.args = args
    session.portable = tmp_path / "portable"
    session.portable.mkdir()
    session.scan_dir = tmp_path / "scan"
    session.scan_dir.mkdir()
    session.au_installed = None
    assert session.place_plugin() is None

    ini = (session.portable / "reaper.ini").read_text()
    # The two that matter.
    assert "Private Session.RPP" not in ini, "it would reopen the user's project"
    assert "Unreleased.RPP" not in ini, "it copied the user's recent-project list"
    assert "loadlastproj=0" in ini
    # And the settings that are the whole reason for seeding must survive, or
    # the fix trades a modal project warning for a first-run wizard.
    assert "audiodevice=CoreAudio" in ini
    assert "keepme=1" in ini
    assert str(session.scan_dir) in ini
