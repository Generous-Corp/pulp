#!/usr/bin/env python3
"""Tests for the generated CI routing digest in CLAUDE.md."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import ci_routing_digest as digest  # noqa: E402
import runner_topology_check as rtc  # noqa: E402
import runner_topology_static as st  # noqa: E402
GENERATOR = HERE / "ci_routing_digest.py"
DOC = ROOT / "CLAUDE.md"
START = "<!-- generated:start id=ci-routing-digest -->"
END = "<!-- generated:end id=ci-routing-digest -->"


def _run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(GENERATOR), *args],
                          capture_output=True, text=True)


def _block(text: str) -> str:
    return text[text.index(START):text.index(END)]


class Digest(unittest.TestCase):
    def test_shipped_block_is_in_sync(self):
        proc = _run("--check")
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_block_states_the_required_gate_and_the_override(self):
        block = _block(DOC.read_text())
        self.assertIn("`PULP_LOCAL_MACOS_RUNS_ON_JSON`: REACHABLE", block)
        self.assertIn("DECLARED supply", block)
        self.assertIn("macos-overflow-local-only", block)
        self.assertIn("mergeQueueEntry", block)
        self.assertIn("rulesets", block)

    def test_a_hand_edit_inside_the_block_fails_check(self):
        text = DOC.read_text()
        edited = text.replace("`PULP_LOCAL_MACOS_RUNS_ON_JSON`: REACHABLE",
                              "`PULP_LOCAL_MACOS_RUNS_ON_JSON`: UNSERVED", 1)
        self.assertNotEqual(edited, text)
        with tempfile.TemporaryDirectory() as tmp:
            doc = Path(tmp) / "CLAUDE.md"
            doc.write_text(edited)
            proc = _run("--check", "--doc", str(doc))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("stale or hand-edited", proc.stderr)
            self.assertEqual(_run("--write", "--doc", str(doc)).returncode, 0)
            self.assertEqual(doc.read_text(), text)

    def test_a_workflow_only_change_cannot_stale_the_block(self):
        # Mirror the generator into a scratch tree, then make workflow-only
        # changes there: a new selector variable and a renamed workflow. Any
        # dependency of the block on the workflow tree would stale --check.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scripts = root / "tools" / "scripts"
            scripts.mkdir(parents=True)
            for name in ("ci_routing_digest.py", "runner_topology_check.py",
                         "runner_topology_static.py", "tools_registry_check.py",
                         "workflow_runner_selector_audit.py", "runner_topology.json",
                         "fleet_advertised_labels.json"):
                shutil.copy(HERE / name, scripts / name)
            shutil.copy(DOC, root / "CLAUDE.md")
            wf = root / ".github" / "workflows"
            shutil.copytree(ROOT / ".github" / "workflows", wf)
            mirror = [sys.executable, str(scripts / "ci_routing_digest.py"), "--check"]
            proc = subprocess.run(mirror, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)  # control: mirror is in sync

            (wf / "zz-new.yml").write_text(
                "name: Brand New\non: [push]\njobs:\n  a:\n"
                "    runs-on: ${{ fromJSON(vars.PULP_BRAND_NEW_RUNS_ON_JSON) }}\n"
                "    steps: [{run: 'true'}]\n")
            for path in wf.glob("release-cli.yml"):
                path.write_text(path.read_text().replace("name: Release CLI",
                                                         "name: Release CLI Renamed", 1))
            contract = rtc.load_contract(scripts / "runner_topology.json")
            rows = st.evaluate(contract, st.load_snapshot(scripts / "fleet_advertised_labels.json"),
                               wf, rtc.DEFAULT_REPO)
            # Control: the static audit SEES both changes.
            self.assertIn("PULP_BRAND_NEW_RUNS_ON_JSON",
                          {r.variable for r in rows if r.verdict == st.UNDECLARED})
            self.assertIn("Release CLI Renamed", {r.workflow for r in rows})
            proc = subprocess.run(mirror, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_editing_an_override_stales_the_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            raw = json.loads((HERE / "runner_topology.json").read_text())
            raw["overrides"][0]["expires"] = "2099-01-01"
            (tmpdir / "runner_topology.json").write_text(json.dumps(raw))
            shutil.copy(HERE / "fleet_advertised_labels.json", tmpdir)
            proc = _run("--check", "--contract", str(tmpdir / "runner_topology.json"))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("stale", proc.stderr)
            # Control: the unedited copy is in sync, so the failure is the edit.
            raw["overrides"][0]["expires"] = json.loads(
                (HERE / "runner_topology.json").read_text())["overrides"][0]["expires"]
            (tmpdir / "runner_topology.json").write_text(json.dumps(raw))
            self.assertEqual(_run("--check", "--contract",
                                  str(tmpdir / "runner_topology.json")).returncode, 0)

    def test_missing_markers_fail_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            doc = Path(tmp) / "CLAUDE.md"
            doc.write_text("# no markers\n")
            proc = _run("--check", "--doc", str(doc))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("missing", proc.stderr)


if __name__ == "__main__":
    unittest.main()
