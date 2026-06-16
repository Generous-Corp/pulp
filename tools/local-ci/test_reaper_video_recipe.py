#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest


from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("reaper_video_recipe.py", add_module_dir=True)


class ReaperVideoRecipeTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_plugin_candidates_include_reaper_prefixes(self):
        self.assertEqual(
            self.mod.reaper_plugin_candidates("PulpSynth", "clap"),
            ["PulpSynth", "CLAP: PulpSynth", "CLAPi: PulpSynth"],
        )
        self.assertEqual(
            self.mod.reaper_plugin_candidates("PulpEffect", "vst3"),
            ["PulpEffect", "VST3: PulpEffect", "VST3i: PulpEffect"],
        )

    def test_installed_clap_bundle_status_checks_executable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            ok, detail = self.mod.installed_clap_bundle_status("PulpSynth", home=temp_dir)
            self.assertFalse(ok)
            self.assertIn("not installed", detail)

            executable = Path(temp_dir) / "Library" / "Audio" / "Plug-Ins" / "CLAP" / "PulpSynth.clap" / "Contents" / "MacOS" / "PulpSynth"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"binary")
            ok, detail = self.mod.installed_clap_bundle_status("PulpSynth", home=temp_dir)
            self.assertTrue(ok)
            self.assertIn("CLAP bundle executable found", detail)

    def test_reaper_clap_cache_status_detects_stale_stanza(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            ok, detail = self.mod.reaper_clap_cache_status("PulpSynth", home=temp_dir)
            self.assertTrue(ok)
            self.assertIn("not present", detail)

            cache = Path(temp_dir) / "Library" / "Application Support" / "REAPER" / "reaper-clap-macos-aarch64.ini"
            cache.parent.mkdir(parents=True)
            cache.write_text("[PulpSynth.clap]\n_=0068FDF5FCFADC010068FDF5FCFADC01\n")
            ok, detail = self.mod.reaper_clap_cache_status("PulpSynth", home=temp_dir)
            self.assertFalse(ok)
            self.assertIn("no plugin descriptor", detail)

            cache.write_text("[PulpSynth.clap]\n_=0068FDF5FCFADC010068FDF5FCFADC01\ncom.pulp.synth=1|PulpSynth (Pulp)\n")
            ok, detail = self.mod.reaper_clap_cache_status("PulpSynth", home=temp_dir)
            self.assertTrue(ok)
            self.assertIn("com.pulp.synth", detail)

    def test_writes_wrapper_and_lua_script(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            files = self.mod.write_reaper_plugin_editor_recipe(
                plugin="PulpSynth",
                plugin_format="clap",
                reaper_app="/Applications/REAPER.app/Contents/MacOS/REAPER",
                root_dir=temp_dir,
            )

            wrapper = Path(files["command"])
            lua_script = Path(files["lua_script"])
            self.assertTrue(wrapper.exists())
            self.assertTrue(lua_script.exists())
            self.assertTrue(wrapper.stat().st_mode & 0o100)
            wrapper_text = wrapper.read_text()
            self.assertIn("REAPER_APP=/Applications/REAPER.app/Contents/MacOS/REAPER", wrapper_text)
            self.assertIn("quit_reaper_best_effort()", wrapper_text)
            self.assertIn('kill "$quit_pid"', wrapper_text)
            self.assertIn("terminate_reaper_pid()", wrapper_text)
            self.assertIn('terminate_reaper_pid "$reaper_pid"', wrapper_text)
            self.assertIn('kill -KILL "$pid"', wrapper_text)
            lua = lua_script.read_text()
            self.assertIn('"PulpSynth", "CLAP: PulpSynth", "CLAPi: PulpSynth"', lua)
            self.assertIn("TrackFX_AddByName", lua)
            self.assertIn("TrackFX_Show(track, fx, 3)", lua)
            self.assertIn("TrackFX_Show floating-editor mode=3", lua)
            self.assertIn("TrackFX_Show fx-chain mode=1", lua)


if __name__ == "__main__":
    unittest.main()
