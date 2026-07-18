#!/usr/bin/env python3
"""Fixture tests for the shipyard-local mac-routing advisory.

Mirrors `shipyard_local_check.py`. Fixture-only and fast — no Shipyard, no
network, no writes outside a temp dir.

Runs standalone (`python3 tools/scripts/test_shipyard_local_check.py`) or via
`python3 -m unittest test_shipyard_local_check`.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import shipyard_local_check as slc  # noqa: E402


CLOUD_OVERRIDE = """
[targets.mac]
backend = "cloud"
platform = "macos-arm64"
runner_provider = "github-hosted"
workflow = "build.yml"
"""

# Pulp's real .shipyard.local/config.toml shape: the mac block commented out,
# other tables live.
COMMENTED_OUT = """
[host_class.m5]
cap = 2
labels = ["pulp-build-m5"]

# ── DISABLED 2026-07-09 ──
# [targets.mac]
# backend = "cloud"
# runner_provider = "github-hosted"
"""


class MacRerouteWarningTests(unittest.TestCase):
    def test_active_cloud_override_warns(self) -> None:
        """The condition that actually breaks the required macos check."""
        warning = slc.mac_reroute_warning(CLOUD_OVERRIDE)
        self.assertIsNotNone(warning)
        self.assertIn("backend = \"cloud\"", warning)
        self.assertIn("github-hosted", warning)
        self.assertIn("shipyard status", warning)

    def test_commented_out_override_is_quiet(self) -> None:
        """PASS control — Pulp's real config shape must not warn.

        A checker that only reports failure is indistinguishable from a broken
        one, so pin the healthy case explicitly.
        """
        self.assertIsNone(slc.mac_reroute_warning(COMMENTED_OUT))

    def test_explicit_local_backend_is_quiet(self) -> None:
        self.assertIsNone(
            slc.mac_reroute_warning('[targets.mac]\nbackend = "local"\n')
        )

    def test_override_without_backend_inherits_local_and_is_quiet(self) -> None:
        """A mac table that only bumps a timeout still routes locally."""
        self.assertIsNone(
            slc.mac_reroute_warning("[targets.mac]\ntimeout_secs = 7200\n")
        )

    def test_unrelated_targets_are_quiet(self) -> None:
        self.assertIsNone(
            slc.mac_reroute_warning('[targets.ubuntu]\nbackend = "ssh"\n')
        )

    def test_malformed_toml_is_quiet(self) -> None:
        """Shipyard reports its own parse errors; this advisory stays out of it."""
        self.assertIsNone(slc.mac_reroute_warning("[targets.mac\nbackend ="))


class MainTests(unittest.TestCase):
    def test_missing_config_is_the_correct_state_not_a_gap(self) -> None:
        """A fresh worktree with no .shipyard.local must exit 0, silently.

        The repo's own `[targets.mac] backend = "local"` already routes
        correctly with no local config present — the Mac Studio runs this way.
        Warning here would push people to copy a config in, which is what
        CAUSED the reroute this check exists to catch.
        """
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(slc.main(["--repo-root", td]), 0)

    def test_main_flags_an_active_reroute(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            cfg = Path(td) / ".shipyard.local"
            cfg.mkdir()
            (cfg / "config.toml").write_text(CLOUD_OVERRIDE)
            self.assertEqual(slc.main(["--repo-root", td]), 1)

    def test_main_is_read_only(self) -> None:
        """The check must never write or repair — the tracked .example next to
        the gitignored live config was clobbered by exactly that instinct."""
        with tempfile.TemporaryDirectory() as td:
            cfg = Path(td) / ".shipyard.local"
            cfg.mkdir()
            (cfg / "config.toml").write_text(CLOUD_OVERRIDE)
            example = cfg / "config.toml.example"
            example.write_text("# tracked template\n")

            before = sorted(p.name for p in cfg.iterdir())
            slc.main(["--repo-root", td])

            self.assertEqual(sorted(p.name for p in cfg.iterdir()), before)
            self.assertEqual((cfg / "config.toml").read_text(), CLOUD_OVERRIDE)
            self.assertEqual(example.read_text(), "# tracked template\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
