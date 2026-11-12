#!/usr/bin/env python3
"""Tests for tools/scripts/release_step_timeouts.py.

The lint must fail when a release job has no `timeout-minutes`, when a long
step loses its timeout, and when a value drifts from the derived policy — and
must pass on the tree as committed. The derivation must size a step for its
SLOWEST matrix leg and must bound the v1.119.0 hang well below its 105+ minutes.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import release_step_timeouts as rst  # noqa: E402

ROOT = HERE.parent.parent
POLICY = json.loads((ROOT / "tools" / "ci" / "release_step_timeouts.json").read_text(encoding="utf-8"))


class CommittedTree(unittest.TestCase):
    def test_policy_holds_on_the_committed_workflows(self) -> None:
        self.assertEqual(rst.check(POLICY, str(ROOT)), [])

    def test_policy_polices_the_incident_step(self) -> None:
        step = POLICY["workflows"][".github/workflows/release-cli.yml"]["steps"]["build-cli"][
            "Prepare SDK build dir (macOS)"
        ]
        # v1.119.0's darwin-x64 leg sat in this step for 105+ minutes.
        self.assertLess(step["timeout_minutes"], 105)
        self.assertGreaterEqual(step["timeout_minutes"], 4 * step["p95_minutes"])


class BrokenCopies(unittest.TestCase):
    """Each mutation of a copy of the workflows must be caught."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        for rel in POLICY["workflows"]:
            dst = self.tmp / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / rel, dst)
        self.release_cli = self.tmp / ".github/workflows/release-cli.yml"

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp)

    def edit(self, old: str, new: str) -> None:
        text = self.release_cli.read_text(encoding="utf-8")
        self.assertEqual(text.count(old), 1, old)
        self.release_cli.write_text(text.replace(old, new), encoding="utf-8")

    def problems(self) -> list[str]:
        return rst.check(POLICY, str(self.tmp))

    def test_control_unmodified_copy_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_missing_long_step_timeout_fails(self) -> None:
        self.edit(
            "      - name: Prepare SDK build dir (macOS)\n        timeout-minutes: ",
            "      - name: Prepare SDK build dir (macOS)\n        x-removed: ",
        )
        found = self.problems()
        self.assertTrue(any("Prepare SDK build dir (macOS)" in p and "no timeout-minutes" in p for p in found), found)

    def test_missing_job_timeout_fails(self) -> None:
        text = self.release_cli.read_text(encoding="utf-8")
        marker = "    needs: [build-cli, smoke-cli, ios-compile-gate]\n    runs-on: ubuntu-latest\n    timeout-minutes: "
        self.assertIn(marker, text)
        self.release_cli.write_text(
            text.replace(marker, marker.replace("timeout-minutes", "x-removed")), encoding="utf-8"
        )
        found = self.problems()
        self.assertTrue(any("job `release` has no timeout-minutes" in p for p in found), found)

    def test_drifted_value_fails(self) -> None:
        self.edit("    timeout-minutes: 243\n", "    timeout-minutes: 360\n")
        found = self.problems()
        self.assertTrue(any("job `build-cli`" in p and "360" in p for p in found), found)

    def test_renamed_long_step_fails(self) -> None:
        self.edit("      - name: Prepare SDK build dir (Linux)\n", "      - name: Prepare the Linux SDK dir\n")
        found = self.problems()
        self.assertTrue(any("not in the workflow" in p for p in found), found)


class Derivation(unittest.TestCase):
    def test_p95_nearest_rank(self) -> None:
        self.assertEqual(rst.p95([1, 2, 3]), 3)
        self.assertEqual(rst.p95(list(range(1, 21))), 19)
        with self.assertRaises(ValueError):
            rst.p95([])

    def test_floor_wins_for_short_steps(self) -> None:
        self.assertEqual(rst.derive_timeout(2.0, multiplier=4, floor=15), 15)
        self.assertEqual(rst.derive_timeout(16.8, multiplier=4, floor=15), 68)

    def test_slowest_leg_sets_the_step_timeout(self) -> None:
        workflow = {"jobs": {"build-cli": {
            "name": "CLI ${{ matrix.platform }}",
            "strategy": {"matrix": {"platform": ["darwin-arm64", "linux-x64"]}},
            "steps": [{"name": "Bootstrap dependencies"}],
        }}}

        def job(platform: str, minutes: int) -> dict:
            return {
                "name": f"CLI {platform}", "conclusion": "success",
                "started_at": "2026-11-12T10:00:00Z", "completed_at": f"2026-11-12T10:{minutes:02d}:00Z",
                "steps": [{"name": "Bootstrap dependencies", "conclusion": "success",
                           "started_at": "2026-11-12T10:00:00Z",
                           "completed_at": f"2026-11-12T10:{minutes:02d}:00Z"}],
            }

        runs = [[job("darwin-arm64", 18), job("linux-x64", 2)] for _ in range(5)]
        runs += [[job("linux-x64", 2)] for _ in range(20)]
        jobs, steps = rst.collect_durations(runs, workflow)
        policy = rst.build_policy(
            workflow=workflow, job_minutes=jobs, step_minutes=steps, n_runs=25, run_ids=[],
            multiplier=4, step_floor=15, job_floor=30, long_step_minutes=2,
        )
        rec = policy["steps"]["build-cli"]["Bootstrap dependencies"]
        self.assertEqual(rec["slowest_leg"], "CLI darwin-arm64")
        self.assertEqual(rec["timeout_minutes"], 72)

    def test_non_matrix_job_with_tag_in_name_is_one_leg(self) -> None:
        workflow = {"jobs": {"ios-compile-gate": {
            "name": "iOS compile gate — ${{ github.ref_name }}", "steps": [],
        }}}
        runs = [[{"name": f"iOS compile gate — v1.{i}.0", "conclusion": "success",
                  "started_at": "2026-11-12T10:00:00Z", "completed_at": "2026-11-12T10:10:00Z",
                  "steps": []}] for i in range(3)]
        jobs, _ = rst.collect_durations(runs, workflow)
        self.assertEqual(list(jobs["ios-compile-gate"]), ["ios-compile-gate"])
        self.assertEqual(len(jobs["ios-compile-gate"]["ios-compile-gate"]), 3)


if __name__ == "__main__":
    unittest.main()
