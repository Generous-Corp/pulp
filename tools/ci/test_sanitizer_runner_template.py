#!/usr/bin/env python3
"""Tests for tools/launchd/pulp-tart-runner-sanitizer-macos.plist.template (#4101).

The sanitizer VM lane is a SECONDARY macOS lane sharing a 2-guest-capped host
with the required `macos` gate. Two contracts keep it from repeating the
coverage-lane incident (which wedged the required gate on 2026-06-16), and both
are encoded in this launchd template:

  1. TCC-safe home-path shape — launchd (macOS TCC) cannot read scripts or a VM
     store under /Volumes, so EVERY path must be under $HOME. A /Volumes path is
     exactly what crash-looped the pre-#4087 coverage template.
  2. Gate-protecting routing — a dedicated label (never the pulp-build gate
     pool), cap=1, and the tartci idle-gate env (yield to "Build and Test") so
     the lane stands down whenever the required gate has work.

These are parsed from the actual plist (plistlib), not grepped loosely, so a
drift in any contract value fails the test by behavior.

Run:  python3 tools/ci/test_sanitizer_runner_template.py
"""
from __future__ import annotations

import plistlib
import unittest
from pathlib import Path

LAUNCHD = Path(__file__).resolve().parents[2] / "tools" / "launchd"
SANITIZER = LAUNCHD / "pulp-tart-runner-sanitizer-macos.plist.template"
GATE_POOL = {"pulp-build", "pulp-build-vm"}


def _load(path: Path) -> dict:
    # The template uses literal `$HOME` placeholders, which are valid plist
    # string content — plistlib parses it fine (the install `sed` substitutes
    # real paths at deploy time).
    return plistlib.loads(path.read_bytes())


def _string_values(obj) -> list:
    """Recursively collect every string VALUE in a parsed plist (keys excluded).

    Checks the actual config — not comment prose — so a template may *document*
    the /Volumes TCC lesson in a comment without tripping the path check, while a
    real /Volumes path in any value still fails.
    """
    out: list = []
    if isinstance(obj, str):
        out.append(obj)
    elif isinstance(obj, dict):
        for v in obj.values():
            out.extend(_string_values(v))
    elif isinstance(obj, (list, tuple)):
        for v in obj:
            out.extend(_string_values(v))
    return out


class SanitizerTemplateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.assertTrue(SANITIZER.is_file(), SANITIZER)
        self.raw = SANITIZER.read_text(encoding="utf-8")
        self.plist = _load(SANITIZER)
        self.env = self.plist.get("EnvironmentVariables", {})

    # ── Contract 1: TCC-safe, fully under $HOME ──────────────────────────────
    def test_no_volumes_path_in_any_value(self) -> None:
        # A /Volumes path in any plist VALUE is the TCC crash-loop bug (#4087).
        for v in _string_values(self.plist):
            self.assertNotIn("/Volumes", v, f"value {v!r} is under /Volumes")

    def test_all_runtime_paths_under_home(self) -> None:
        self.assertEqual(self.plist["WorkingDirectory"], "$HOME")
        for key in ("StandardOutPath", "StandardErrorPath"):
            self.assertTrue(self.plist[key].startswith("$HOME/"),
                            f"{key}={self.plist[key]!r}")
        self.assertEqual(self.env["HOME"], "$HOME")
        self.assertEqual(self.env["TART_HOME"], "$HOME/VMs")
        for key in ("TARTCI_CI_CACHE", "TARTCI_HOME"):
            self.assertTrue(self.env[key].startswith("$HOME/"),
                            f"{key}={self.env[key]!r}")

    def test_keepalive_and_runatload(self) -> None:
        # Reboot durability: a gui-domain LaunchAgent that must come back after
        # login and be restarted if it dies.
        self.assertTrue(self.plist.get("RunAtLoad") is True)
        self.assertTrue(self.plist.get("KeepAlive") is True)

    def test_drives_tartci_serve_macos(self) -> None:
        args = self.plist["ProgramArguments"]
        self.assertEqual(args[0], "/bin/bash")
        self.assertTrue(args[1].endswith("/.local/bin/tartci"), args[1])
        self.assertIn("serve", args)
        self.assertIn("macos", args)
        self.assertIn("--loop", args)

    # ── Contract 2: gate-protecting routing ──────────────────────────────────
    def test_routes_provider_gh_off_personal_pat(self) -> None:
        # The provider polls GitHub every VM_POLL seconds; on the shared personal
        # PAT that is the dominant throttle. The lane MUST route through the
        # GitHub-App CLI (TARTCI_GH_CLI=ghapp) so adding this VM to a host does
        # not multiply the PAT throttle. (Regression: the lane shipped without
        # this key, which would have polled on bare `gh`.)
        self.assertEqual(self.env.get("TARTCI_GH_CLI"), "ghapp",
                         "sanitizer lane must set TARTCI_GH_CLI=ghapp (off the PAT)")

    def test_dedicated_label_never_gate_pool(self) -> None:
        runner_labels = {s.strip() for s in
                         self.env["TARTCI_RUNNER_LABELS"].split(",")}
        self.assertIn("pulp-sanitizer-vm-macos", runner_labels)
        # The whole point: must NOT advertise the required-gate pool labels, or
        # GitHub could schedule a required `macos` job onto this lane.
        self.assertEqual(runner_labels & GATE_POOL, set(),
                         "sanitizer lane must not carry gate-pool labels")
        # The --labels CLI arg must agree with the env labels.
        args = self.plist["ProgramArguments"]
        cli_labels = {s.strip() for s in args[args.index("--labels") + 1].split(",")}
        self.assertEqual(cli_labels, runner_labels)

    def test_watches_sanitizer_workflow(self) -> None:
        self.assertEqual(self.env["TARTCI_RUNNER_WORKFLOW_NAME"], "Sanitizer Tests")

    def test_single_vm_cap(self) -> None:
        self.assertEqual(self.env["TARTCI_MACOS_VM_CAP"], "1")

    def test_idle_gate_yields_to_required_gate(self) -> None:
        # The idle-gate env is what makes a shared-store secondary lane safe.
        self.assertEqual(self.env["TARTCI_YIELD_TO_WORKFLOW_NAME"], "Build and Test")
        yield_labels = {s.strip() for s in
                        self.env["TARTCI_YIELD_TO_LABELS"].split(",")}
        # It must yield to the actual gate pool (so it detects gate demand).
        self.assertTrue(GATE_POOL.issubset(yield_labels),
                        f"yield labels {yield_labels} must include the gate pool")

    def test_does_not_read_namespace_vars(self) -> None:
        # Advisory lanes must not be wired to the paid Namespace pool.
        self.assertNotIn("PULP_NAMESPACE", self.raw)
        self.assertNotIn("namespace-profile", self.raw)


class AllTartRunnerTemplatesTCCSafe(unittest.TestCase):
    """Cheap regression net across every Tart macOS runner template: none may
    carry a /Volumes path (the #4087 crash-loop class) and all must parse."""

    def test_every_tart_macos_template_is_volumes_free_and_parses(self) -> None:
        templates = sorted(LAUNCHD.glob("pulp-tart-runner-*macos*.plist.template"))
        self.assertTrue(templates, "no tart macOS runner templates found")
        for tpl in templates:
            plist = _load(tpl)  # raises if the plist body is malformed XML
            for v in _string_values(plist):
                self.assertNotIn("/Volumes", v,
                                 f"{tpl.name} value {v!r} is under /Volumes")


if __name__ == "__main__":
    unittest.main(verbosity=2)
