#!/usr/bin/env python3
"""Unit tests for check_au_component_preflight.py."""

from __future__ import annotations

import importlib.util
import io
import json
import plistlib
import contextlib
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "check_au_component_preflight",
    SCRIPT_DIR / "check_au_component_preflight.py",
)
assert spec and spec.loader
preflight = importlib.util.module_from_spec(spec)
sys.modules["check_au_component_preflight"] = preflight
spec.loader.exec_module(preflight)


def _make_bundle(root: pathlib.Path, *, au_type: str = "aumf") -> pathlib.Path:
    bundle = root / "PulpHostBench.component"
    macos = bundle / "Contents" / "MacOS"
    macos.mkdir(parents=True)
    (macos / "PulpHostBench").write_text("fake executable\n", encoding="utf-8")
    plist = {
        "CFBundleExecutable": "PulpHostBench",
        "CFBundleIdentifier": "com.pulp.host-bench.au",
        "CFBundleName": "PulpHostBench",
        "CFBundleShortVersionString": "1.0.0",
        "AudioComponents": [
            {
                "type": au_type,
                "subtype": "PHBn",
                "manufacturer": "Pulp",
                "factoryFunction": "PulpHostBenchAUFactory",
                "name": "Pulp: PulpHostBench",
                "description": "PulpHostBench - DAW quirk validation bench",
                "version": 65536,
            }
        ],
    }
    with (bundle / "Contents" / "Info.plist").open("wb") as f:
        plistlib.dump(plist, f)
    return bundle


class AuComponentPreflightTests(unittest.TestCase):
    def _run(self, args: list[str]) -> tuple[int, str]:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            rc = preflight.main(args)
        return rc, stdout.getvalue()

    def test_static_preflight_passes_for_expected_component_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            rc, out = self._run([
                str(bundle),
                "--expect-type", "aumf",
                "--expect-subtype", "PHBn",
                "--expect-manufacturer", "Pulp",
                "--expect-factory", "PulpHostBenchAUFactory",
            ])
            self.assertEqual(rc, 0, out)
            self.assertIn("PASS: type: aumf", out)
            self.assertIn("PASS: name: Pulp: PulpHostBench", out)
            self.assertIn("PASS: version: 65536", out)
            self.assertIn("PASS: CFBundleExecutable:", out)

    def test_static_preflight_fails_on_wrong_component_type(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d), au_type="aufx")
            rc, out = self._run([
                str(bundle),
                "--expect-type", "aumf",
                "--expect-subtype", "PHBn",
                "--expect-manufacturer", "Pulp",
                "--expect-factory", "PulpHostBenchAUFactory",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: type: expected 'aumf', found 'aufx'", out)

    def test_static_preflight_fails_when_executable_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            (bundle / "Contents" / "MacOS" / "PulpHostBench").unlink()
            rc, out = self._run([
                str(bundle),
                "--expect-type", "aumf",
                "--expect-subtype", "PHBn",
                "--expect-manufacturer", "Pulp",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: CFBundleExecutable:", out)

    def test_static_preflight_fails_when_component_name_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            plist_path = bundle / "Contents" / "Info.plist"
            with plist_path.open("rb") as f:
                plist = plistlib.load(f)
            del plist["AudioComponents"][0]["name"]
            with plist_path.open("wb") as f:
                plistlib.dump(plist, f)
            rc, out = self._run([
                str(bundle),
                "--expect-type", "aumf",
                "--expect-subtype", "PHBn",
                "--expect-manufacturer", "Pulp",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: name: missing or non-string name", out)

    def test_static_preflight_fails_when_component_version_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            plist_path = bundle / "Contents" / "Info.plist"
            with plist_path.open("rb") as f:
                plist = plistlib.load(f)
            del plist["AudioComponents"][0]["version"]
            with plist_path.open("wb") as f:
                plistlib.dump(plist, f)
            rc, out = self._run([
                str(bundle),
                "--expect-type", "aumf",
                "--expect-subtype", "PHBn",
                "--expect-manufacturer", "Pulp",
            ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: version: missing or non-negative integer version", out)

    def test_static_preflight_fails_when_factory_symbol_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            with mock.patch.object(preflight.shutil, "which", return_value="/usr/bin/nm"), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["nm"], returncode=0, stdout="_SomeOtherFactory\n"
                )
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--expect-symbol", "PulpHostBenchAUFactory",
                ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: factory symbol: _PulpHostBenchAUFactory not exported", out)

    def test_permissions_check_reports_bundle_modes(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            rc, out = self._run([
                str(bundle),
                "--expect-type", "aumf",
                "--check-permissions",
            ])
            self.assertEqual(rc, 0, out)
            self.assertIn("PASS: permissions:", out)
            self.assertIn("PulpHostBench.component=", out)

    def test_codesign_check_reports_verification_failure(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                return "/usr/bin/codesign" if name == "codesign" else None
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["codesign"], returncode=1, stdout="resource envelope is obsolete\n"
                )
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--check-codesign",
                ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: codesign: resource envelope is obsolete", out)

    def test_signing_identity_check_reports_accessible_identity(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                return "/usr/bin/codesign" if name == "codesign" else None
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["codesign"], returncode=0, stdout=""
                )
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--check-signing-identity", "Developer ID Application: Example",
                ])
            self.assertEqual(rc, 0, out)
            self.assertIn(
                "PASS: signing identity: Developer ID Application: Example",
                out,
            )

    def test_signing_identity_check_reports_keychain_access_failure(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                return "/usr/bin/codesign" if name == "codesign" else None
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["codesign"], returncode=1, stdout="errSecInternalComponent\n"
                )
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--check-signing-identity", "Developer ID Application: Example",
                ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: signing identity: errSecInternalComponent", out)
            self.assertIn("private key access failed", out)

    def test_gatekeeper_check_reports_acceptance(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                return "/usr/sbin/spctl" if name == "spctl" else None
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["spctl"], returncode=0, stdout="accepted\n"
                )
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--check-gatekeeper",
                ])
            self.assertEqual(rc, 0, out)
            self.assertIn("PASS: gatekeeper: accepted", out)

    def test_gatekeeper_check_reports_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                if name == "spctl":
                    return "/usr/sbin/spctl"
                if name == "syspolicy_check":
                    return "/usr/bin/syspolicy_check"
                return None
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.side_effect = [
                    preflight.subprocess.CompletedProcess(
                        args=["spctl"],
                        returncode=3,
                        stdout="rejected\nsource=Insufficient Context\n",
                    ),
                    preflight.subprocess.CompletedProcess(
                        args=["syspolicy_check"],
                        returncode=1,
                        stdout="Notary Ticket Missing\nAdhoc Signed App\n",
                    ),
                ]
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--check-gatekeeper",
                ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: gatekeeper: rejected", out)
            self.assertIn("source=Insufficient Context", out)
            self.assertIn("Notary Ticket Missing", out)
            self.assertIn("Adhoc Signed App", out)

    def test_auval_list_check_requires_component_registration(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                return "/usr/bin/auval" if name == "auval" else None
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["auval"], returncode=0, stdout="aufx PGan Pulp  -  Pulp: PulpGain\n"
                )
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--expect-subtype", "PHBn",
                    "--expect-manufacturer", "Pulp",
                    "--check-auval-list",
                ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: auval list: aumf PHBn Pulp not listed", out)
            self.assertIn("including non-Apple: aufx PGan Pulp", out)

    def test_auval_list_check_reports_when_only_apple_components_are_visible(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                return "/usr/bin/auval" if name == "auval" else None
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["auval"], returncode=0, stdout=(
                        "aufx bpas appl  -  Apple: AUBandpass\n"
                        "aumu dls  appl  -  Apple: DLSMusicDevice\n"
                    )
                )
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--expect-subtype", "PHBn",
                    "--expect-manufacturer", "Pulp",
                    "--check-auval-list",
                ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: auval list: aumf PHBn Pulp not listed", out)
            self.assertIn("listed 2 Apple component(s) and no non-Apple components", out)

    def test_apple_only_inventory_gets_typed_registrar_diagnosis(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            outputs = {
                "auval": "aufx bpas appl  -  Apple: AUBandpass\n",
                "uname": "arm64\n",
                "arch": "Rosetta 2 is not installed\n",
                "launchctl domain": "Aqua\n",
                "launchctl print": "service not found\n",
                "pgrep": "",
                "log": '{"process":"AudioComponentRegistrar","eventMessage":"Rosetta runtime is not available!"}\n',
            }
            def fake_which(name: str) -> str | None:
                return "/usr/bin/" + name
            def fake_run(command, **kwargs):
                tool = command[0]
                if tool == "/usr/bin/auval":
                    return preflight.subprocess.CompletedProcess(command, 0, outputs["auval"])
                if tool == "uname":
                    return preflight.subprocess.CompletedProcess(command, 0, outputs["uname"])
                if tool == "arch":
                    return preflight.subprocess.CompletedProcess(command, 1, outputs["arch"])
                if tool == "/usr/bin/file":
                    return preflight.subprocess.CompletedProcess(command, 0, "Mach-O 64-bit executable arm64\n")
                if tool == "/usr/bin/launchctl" and command[1] == "managername":
                    return preflight.subprocess.CompletedProcess(command, 0, outputs["launchctl domain"])
                if tool == "/usr/bin/launchctl":
                    return preflight.subprocess.CompletedProcess(command, 1, outputs["launchctl print"])
                if tool == "/usr/bin/pgrep":
                    return preflight.subprocess.CompletedProcess(command, 1, outputs["pgrep"])
                if tool == "/usr/bin/log":
                    return preflight.subprocess.CompletedProcess(command, 0, outputs["log"])
                raise AssertionError(command)
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run", side_effect=fake_run):
                rc, out = self._run([
                    str(bundle), "--expect-type", "aumf", "--expect-subtype", "PHBn",
                    "--expect-manufacturer", "Pulp", "--check-auval-list", "--format", "json",
                ])
            data = json.loads(out)
            self.assertEqual(rc, 1)
            checks = {item["label"]: item for item in data["checks"]}
            self.assertEqual(checks["auval list"]["code"], "SYSTEM_REGISTRAR_APPLE_ONLY")
            self.assertEqual(checks["Rosetta"]["code"], "HOST_ROSETTA_MISSING")
            self.assertEqual(checks["registrar logs"]["code"], "REGISTRAR_ROSETTA_FAILURE")

    def test_log_command_cannot_self_trigger_rosetta_failure(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                return "/usr/bin/" + name
            def fake_run(command, **kwargs):
                if command[:2] == ["uname", "-m"]:
                    return preflight.subprocess.CompletedProcess(command, 0, "arm64\n")
                if command[0] == "/usr/bin/file":
                    return preflight.subprocess.CompletedProcess(command, 0, "Mach-O universal binary arm64 x86_64\n")
                if command[0] in ("arch", "/usr/bin/arch"):
                    return preflight.subprocess.CompletedProcess(command, 0, "")
                if command[0] == "/usr/bin/launchctl":
                    return preflight.subprocess.CompletedProcess(command, 0, "Aqua\n")
                if command[0] == "/usr/bin/log":
                    return preflight.subprocess.CompletedProcess(
                        command, 0,
                        '{"process":"log","eventMessage":"args=--predicate process == '
                        'AudioComponentRegistrar eventMessage CONTAINS[c] Rosetta runtime is not available"}\n',
                    )
                raise AssertionError(command)
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run", side_effect=fake_run):
                checks = preflight._diagnose_host(type("Args", (), {"bundle": bundle})())
            logs = next(check for check in checks if check.label == "registrar logs")
            self.assertTrue(logs.ok)
            self.assertEqual(logs.code, "REGISTRAR_LOGS_CLEAR")

    def test_host_probe_timeout_is_bounded(self) -> None:
        with mock.patch.object(preflight.subprocess, "run",
                               side_effect=preflight.subprocess.TimeoutExpired(["log"], 10.0)):
            result = preflight._run_probe(["log", "show"])
        self.assertEqual(result.returncode, 124)
        self.assertIn("timed out after 10s", result.stdout)

    def test_diagnosis_reports_non_arm64_without_rosetta_failure(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            def fake_which(name: str) -> str | None:
                return "/usr/bin/" + name
            def fake_run(command, **kwargs):
                if command[:2] == ["uname", "-m"]:
                    return preflight.subprocess.CompletedProcess(command, 0, "x86_64\n")
                if command[0] == "/usr/bin/file":
                    return preflight.subprocess.CompletedProcess(command, 0, "Mach-O 64-bit executable x86_64\n")
                if command[0] == "/usr/bin/launchctl":
                    return preflight.subprocess.CompletedProcess(command, 0, "Background\n")
                if command[0] == "/usr/bin/log":
                    return preflight.subprocess.CompletedProcess(command, 0, "")
                raise AssertionError(command)
            with mock.patch.object(preflight.shutil, "which", side_effect=fake_which), \
                 mock.patch.object(preflight.subprocess, "run", side_effect=fake_run):
                checks = preflight._diagnose_host(type("Args", (), {"bundle": bundle})())
            rosetta = next(check for check in checks if check.label == "Rosetta")
            self.assertTrue(rosetta.ok)
            self.assertEqual(rosetta.code, "ROSETTA_NOT_REQUIRED")

    def test_json_output_is_machine_readable(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            rc, out = self._run([
                str(bundle),
                "--expect-type", "aumf",
                "--expect-subtype", "PHBn",
                "--expect-manufacturer", "Pulp",
                "--format", "json",
            ])
            data = json.loads(out)
            self.assertEqual(rc, 0)
            self.assertTrue(data["ok"])
            self.assertIn({
                "label": "type",
                "ok": True,
                "detail": "aumf",
            }, data["checks"])

    def test_auval_list_timeout_is_typed_and_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            with mock.patch.object(preflight.shutil, "which", return_value="/usr/bin/auval"), \
                 mock.patch.object(preflight.subprocess, "run", side_effect=preflight.subprocess.TimeoutExpired(["auval", "-a"], 0.1)):
                rc, out = self._run([
                    str(bundle), "--expect-type", "aumf", "--expect-subtype", "PHBn",
                    "--expect-manufacturer", "Pulp", "--check-auval-list", "--auval-timeout", "0.1",
                    "--format", "json",
                ])
            data = json.loads(out)
            self.assertEqual(rc, 1)
            check = next(item for item in data["checks"] if item["label"] == "auval list")
            self.assertEqual(check["code"], "AUVAL_TIMEOUT")
            self.assertIn("timed out", check["detail"])

    def test_auval_validation_timeout_is_typed(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            with mock.patch.object(preflight.shutil, "which", return_value="/usr/bin/auval"), \
                 mock.patch.object(preflight.subprocess, "run", side_effect=preflight.subprocess.TimeoutExpired(["auval", "-v"], 0.1)):
                rc, out = self._run([
                    str(bundle), "--expect-type", "aumf", "--expect-subtype", "PHBn",
                    "--expect-manufacturer", "Pulp", "--run-auval", "--auval-timeout", "0.1",
                ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: auval: run 1/2 timed out", out)

    def test_auval_repeat_runs_requested_number_of_times(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            with mock.patch.object(preflight.shutil, "which", return_value="/usr/bin/auval"), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["auval"], returncode=0, stdout="AU VALIDATION PASSED\nPASS\n"
                )
                rc, out = self._run([
                    str(bundle),
                    "--expect-type", "aumf",
                    "--expect-subtype", "PHBn",
                    "--expect-manufacturer", "Pulp",
                    "--run-auval",
                    "--auval-repeat", "2",
                ])
            self.assertEqual(rc, 0, out)
            self.assertEqual(run.call_count, 2)
            self.assertIn("PASS: auval: run 1/2", out)
            self.assertIn("PASS: auval: run 2/2", out)

    def test_auval_requires_exact_success_marker(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            with mock.patch.object(preflight.shutil, "which", return_value="/usr/bin/auval"), \
                 mock.patch.object(preflight.subprocess, "run") as run:
                run.return_value = preflight.subprocess.CompletedProcess(
                    args=["auval"], returncode=0, stdout="NOT PASS\n"
                )
                rc, out = self._run([
                    str(bundle), "--expect-type", "aumf", "--expect-subtype", "PHBn",
                    "--expect-manufacturer", "Pulp", "--run-auval", "--auval-repeat", "1",
                ])
            self.assertEqual(rc, 1)
            self.assertIn("FAIL: auval: run 1/1 failed", out)

    def test_rejects_invalid_auval_repeat(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            bundle = _make_bundle(pathlib.Path(d))
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    preflight.main([str(bundle), "--auval-repeat", "0"])


if __name__ == "__main__":
    unittest.main()
