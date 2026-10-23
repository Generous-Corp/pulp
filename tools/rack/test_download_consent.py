#!/usr/bin/env python3
"""Automatic plugin downloads wait for consent where a person is watching.

The app shows a one-time notice before the first third-party plugin is
downloaded for a patch. patch.py's half: an automatic download in a run the
app launched (FORGE_INTERACTIVE_DOWNLOADS=1) is held and reported until the
notice is answered, "ask" holds every one, and a run nobody is watching keeps
following auto_download exactly as before.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import tempfile
import unittest
from unittest import mock

import patch

PLAN = {"fetch": [{"plugin": "Valley", "version": "2.4.5", "premium": False,
                   "owned": False, "modules": 9, "why": "'valley'"}],
        "blocked": []}


class DownloadConsentTests(unittest.TestCase):
    def run_named_fetch(self, st, env):
        installs = []

        def fake_install(plugin, version, premium, owned):
            installs.append(plugin)
            return True, f"installed {plugin}"

        out = io.StringIO()
        with mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(patch, "settings", return_value=st), \
                mock.patch.object(patch, "named_fetch_plan", return_value=PLAN), \
                mock.patch.object(patch, "entitlements_cached", return_value=set()), \
                mock.patch.object(patch, "install_module", side_effect=fake_install), \
                mock.patch.object(patch, "inventory", return_value={}), \
                contextlib.redirect_stdout(out):
            _, fetched = patch.ensure_named_installed(
                "use valley", {}, {}, {}, {})
        consent = [json.loads(line[len(patch.DOWNLOAD_CONSENT_PREFIX):])
                   for line in out.getvalue().splitlines()
                   if line.startswith(patch.DOWNLOAD_CONSENT_PREFIX)]
        return installs, fetched, consent

    def settings_with(self, **values):
        st = dict(patch.SETTINGS_DEFAULTS)
        st.update(values)
        return st

    def test_unattended_runs_download_as_before(self):
        installs, fetched, consent = self.run_named_fetch(
            self.settings_with(), {})
        self.assertEqual(installs, ["Valley"])
        self.assertEqual(fetched, ["Valley"])
        self.assertEqual(consent, [])

    def test_the_app_holds_until_the_notice_is_answered(self):
        installs, fetched, consent = self.run_named_fetch(
            self.settings_with(), {"FORGE_INTERACTIVE_DOWNLOADS": "1"})
        self.assertEqual(installs, [])
        self.assertEqual(fetched, [])
        self.assertEqual(consent, [{"plugin": "Valley", "version": "2.4.5",
                                    "modules": 9, "why": "'valley'"}])

    def test_an_answered_notice_follows_the_choice(self):
        installs, _, consent = self.run_named_fetch(
            self.settings_with(auto_download_acknowledged=True),
            {"FORGE_INTERACTIVE_DOWNLOADS": "1"})
        self.assertEqual(installs, ["Valley"])
        self.assertEqual(consent, [])

    def test_ask_holds_every_download_watched_or_not(self):
        for env in ({}, {"FORGE_INTERACTIVE_DOWNLOADS": "1"}):
            with self.subTest(env=env):
                installs, _, consent = self.run_named_fetch(
                    self.settings_with(auto_download="ask",
                                       auto_download_acknowledged=True), env)
                self.assertEqual(installs, [])
                self.assertEqual(len(consent), 1)

    def test_the_new_values_are_writable(self):
        with tempfile.TemporaryDirectory() as home:
            path = os.path.join(home, "settings.json")
            with mock.patch.object(patch, "SETTINGS_PATH", path):
                patch.write_setting("auto_download", "ask")
                patch.write_setting("auto_download_acknowledged", True)
                with open(path, encoding="utf-8") as f:
                    written = json.load(f)
                self.assertEqual(written["auto_download"], "ask")
                self.assertIs(written["auto_download_acknowledged"], True)
                with self.assertRaises(SystemExit):
                    patch.write_setting("auto_download", "sometimes")
        self.assertIs(patch.SETTINGS_DEFAULTS["auto_download_acknowledged"], False)


if __name__ == "__main__":
    unittest.main()
