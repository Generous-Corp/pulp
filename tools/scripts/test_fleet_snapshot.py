#!/usr/bin/env python3
"""Tests for fleet_snapshot.py against a fake tartci checkout.

The fake generator stands in for tartci's `macos_fleet_lanes.py
advertised-labels`: each profile file holds the registrations it advertises
as JSON, so a test adds or removes a machine by adding or deleting a file.
tartci's published `fleet/advertised-labels.json`, when present, takes
precedence over the generator.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE / "fleet_snapshot.py"

FAKE_GENERATOR = r'''
import json, sys
paths = [a for a in sys.argv[2:] if a != "--json"]
regs = []
for path in paths:
    regs.extend(json.load(open(path)))
print(json.dumps({"schema": "tartci.advertised-labels/v1",
                  "generated_from": {"repo": "fake/tartci", "commit": "abc",
                                     "profiles": paths},
                  "registrations": regs}))
'''


def _reg(host: str, lane: str = "pulp-gate", cls: str | None = "pulp-build-pr-head") -> dict:
    return {"profile": f"{host}-macos-fleet", "host_id": host, "lane": lane,
            "repo": "Generous-Corp/pulp", "assignment_mode": "event-class-v2",
            "class_label": cls, "labels": ["self-hosted", "pulp-build-vm", cls or lane],
            "workflows": ["Build and Test"]}


class FleetSnapshot(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.tartci = root / "tartci"
        (self.tartci / "scripts").mkdir(parents=True)
        (self.tartci / "profiles").mkdir()
        (self.tartci / "scripts" / "macos_fleet_lanes.py").write_text(FAKE_GENERATOR)
        self.snapshot = root / "fleet_advertised_labels.json"
        self._profile("hosta", [_reg("hosta")])
        self._profile("hostb", [_reg("hostb")])
        # Matches no glob: a non-fleet profile must never be read.
        (self.tartci / "profiles" / "normal-local-fast.toml").write_text("not json")

    def tearDown(self):
        self._tmp.cleanup()

    def _profile(self, host: str, regs: list[dict]) -> Path:
        path = self.tartci / "profiles" / f"{host}-macos-fleet.toml"
        path.write_text(json.dumps(regs))
        return path

    def _run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(TOOL), "--tartci", str(self.tartci),
                               "--snapshot", str(self.snapshot), *args],
                              capture_output=True, text=True)

    def test_identical_passes_and_metadata_is_ignored(self):
        self.assertEqual(self._run("--write").returncode, 0)
        data = json.loads(self.snapshot.read_text())
        self.assertIn("*-macos-fleet.toml", data["generated_from"]["regenerate"])
        data["generated_from"]["commit"] = "something-older"
        self.snapshot.write_text(json.dumps(data))
        proc = self._run("--check")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_a_new_profile_file_is_picked_up_by_the_glob(self):
        self.assertEqual(self._run("--write").returncode, 0)
        self._profile("hostnew", [_reg("hostnew")])
        proc = self._run("--check")
        self.assertEqual(proc.returncode, 1)
        self.assertIn("host added upstream: hostnew", proc.stdout)
        self.assertEqual(self._run("--write").returncode, 0)
        hosts = {r["host_id"] for r in json.loads(self.snapshot.read_text())["registrations"]}
        self.assertEqual(hosts, {"hosta", "hostb", "hostnew"})

    def test_a_removed_profile_fails_check_naming_it(self):
        self.assertEqual(self._run("--write").returncode, 0)
        (self.tartci / "profiles" / "hostb-macos-fleet.toml").unlink()
        proc = self._run("--check")
        self.assertEqual(proc.returncode, 1)
        self.assertIn("host removed upstream: hostb", proc.stdout)

    def test_lane_level_changes_are_named(self):
        self.assertEqual(self._run("--write").returncode, 0)
        self._profile("hosta", [_reg("hosta"), _reg("hosta", "pulp-release", "rel")])
        self._profile("hostb", [])
        proc = self._run("--check")
        self.assertEqual(proc.returncode, 1)
        self.assertIn("lane added upstream: hosta:pulp-release/rel", proc.stdout)
        self.assertIn("host removed upstream: hostb", proc.stdout)

    def test_a_changed_registration_is_named(self):
        self.assertEqual(self._run("--write").returncode, 0)
        reg = _reg("hosta")
        reg["workflows"] = ["Other"]
        self._profile("hosta", [reg])
        proc = self._run("--check")
        self.assertEqual(proc.returncode, 1)
        self.assertIn("registration changed: hosta:pulp-gate/pulp-build-pr-head (workflows)",
                      proc.stdout)

    def test_the_published_file_wins_over_the_generator(self):
        self.assertEqual(self._run("--write").returncode, 0)
        published = self.tartci / "fleet" / "advertised-labels.json"
        published.parent.mkdir()
        data = json.loads(self.snapshot.read_text())
        data["registrations"].append(_reg("hostpub"))
        published.write_text(json.dumps(data))
        proc = self._run("--check")
        self.assertEqual(proc.returncode, 1)
        self.assertIn("host added upstream: hostpub", proc.stdout)
        self.assertIn("published fleet/advertised-labels.json", proc.stdout)

    def test_a_malformed_published_file_is_unreadable_not_a_fallback(self):
        (self.tartci / "fleet").mkdir()
        (self.tartci / "fleet" / "advertised-labels.json").write_text('{"schema": "x"}')
        proc = self._run("--check")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("UNREADABLE", proc.stderr)

    def test_the_source_url_is_fetched(self):
        self.assertEqual(self._run("--write").returncode, 0)
        remote = Path(self._tmp.name) / "remote.json"
        data = json.loads(self.snapshot.read_text())
        data["registrations"] = data["registrations"][:1]
        remote.write_text(json.dumps(data))
        proc = subprocess.run([sys.executable, str(TOOL), "--source-url", remote.as_uri(),
                               "--snapshot", str(self.snapshot), "--check"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("host removed upstream: hostb", proc.stdout)

    def test_a_tartci_without_the_generator_is_unreadable(self):
        (self.tartci / "scripts" / "macos_fleet_lanes.py").unlink()
        proc = self._run("--check")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("UNREADABLE", proc.stderr)


if __name__ == "__main__":
    unittest.main()
