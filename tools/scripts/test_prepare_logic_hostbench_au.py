#!/usr/bin/env python3
"""Unit tests for prepare_logic_hostbench_au.py."""

from __future__ import annotations

import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parent / "prepare_logic_hostbench_au.py"
spec = importlib.util.spec_from_file_location("prepare_logic_hostbench_au", SCRIPT)
assert spec and spec.loader
prep = importlib.util.module_from_spec(spec)
sys.modules["prepare_logic_hostbench_au"] = prep
spec.loader.exec_module(prep)


def make_component(root: pathlib.Path) -> pathlib.Path:
    component = root / "PulpHostBench.component"
    (component / "Contents" / "MacOS").mkdir(parents=True)
    (component / "Contents" / "MacOS" / "PulpHostBench").write_text("binary", encoding="utf-8")
    return component


class PrepareLogicHostBenchAuTests(unittest.TestCase):
    def test_env_file_resolution_expands_home(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            env_file = pathlib.Path(tmp) / "notary.env"
            env_file.write_text(
                "PULP_SIGN_IDENTITY='Developer ID Application: Example'\n"
                "PULP_NOTARY_KEY_PATH=\"$HOME/.config/pulp/secrets/AuthKey_TEST.p8\"\n"
                "PULP_NOTARY_KEY_ID=KEYID\n",
                encoding="utf-8",
            )
            values = prep.load_env_file(env_file)
            self.assertEqual(values["PULP_SIGN_IDENTITY"], "Developer ID Application: Example")
            self.assertTrue(values["PULP_NOTARY_KEY_PATH"].startswith(str(pathlib.Path.home())))
            self.assertEqual(values["PULP_NOTARY_KEY_ID"], "KEYID")

    def test_missing_identity_fails_before_commands(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            component = make_component(root)
            install_dir = root / "install"
            with mock.patch.object(prep, "resolve_env", return_value={}), \
                 mock.patch.object(prep, "run") as run, \
                 mock.patch.object(sys, "stdout", io.StringIO()), \
                 mock.patch.object(sys, "stderr", io.StringIO()) as stderr:
                rc = prep.main([
                    "--component", str(component),
                    "--install-dir", str(install_dir),
                    "--dry-run",
                ])
            self.assertEqual(rc, 2)
            run.assert_not_called()
            self.assertIn("missing signing identity", stderr.getvalue())

    def test_dry_run_prints_install_sign_reset_and_preflight(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            component = make_component(root)
            install_dir = root / "install"
            calls: list[list[str]] = []

            def fake_run(cmd: list[str], **_: object) -> int:
                calls.append(cmd)
                return 0

            with mock.patch.object(prep, "run", side_effect=fake_run), \
                 mock.patch.object(prep, "resolve_env", return_value={
                     "PULP_SIGN_IDENTITY": "Developer ID Application: Example",
                 }), \
                 mock.patch.object(sys, "stdout", io.StringIO()):
                rc = prep.main([
                    "--component", str(component),
                    "--install-dir", str(install_dir),
                    "--dry-run",
                ])

            self.assertEqual(rc, 0)
            self.assertEqual(calls[0][:4], ["codesign", "--force", "--deep", "--sign"])
            self.assertIn("Developer ID Application: Example", calls[0])
            self.assertEqual(calls[1][0:3], ["rm", "-rf", str(pathlib.Path.home() / "Library" / "Caches" / "AudioUnitCache")])
            self.assertEqual(calls[2][0:3], ["killall", "-KILL", "AudioComponentRegistrar"])
            self.assertIn("--check-gatekeeper", calls[3])
            self.assertIn("--check-signing-identity", calls[3])
            self.assertIn("--run-auval", calls[3])

    def test_notarize_dry_run_uses_zipped_component_and_staples_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            component = make_component(root)
            calls: list[list[str]] = []

            def fake_run(cmd: list[str], **_: object) -> int:
                calls.append(cmd)
                return 0

            rc = None
            with mock.patch.object(prep, "run", side_effect=fake_run):
                rc = prep.notarize_component(
                    component,
                    {
                        "PULP_NOTARY_KEY_PATH": "/tmp/AuthKey_TEST.p8",
                        "PULP_NOTARY_KEY_ID": "KEYID",
                        "PULP_NOTARY_ISSUER_ID": "issuer",
                    },
                    dry_run=True,
                )

            self.assertEqual(rc, 0)
            self.assertEqual(calls[0][0:4], ["ditto", "-c", "-k", "--keepParent"])
            self.assertEqual(calls[1][0:3], ["xcrun", "notarytool", "submit"])
            self.assertIn("--wait", calls[1])
            self.assertEqual(calls[2], ["xcrun", "stapler", "staple", str(component)])


if __name__ == "__main__":
    unittest.main()
