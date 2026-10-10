#!/usr/bin/env python3
"""Interaction replay contract for pulp-design-ir-observe.

The observer replays --click/--drag/--key steps against a materialized DesignIR
tree and records each step's hit target, focus and control state. A parity
harness compares those logs between implementations, so the log has to be
deterministic, has to name what each step actually reached, and the render has
to show the post-interaction state.

Usage: test_design_ir_observe_interactions.py --observer <pulp-design-ir-observe>
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FIXTURE = Path(__file__).resolve().parent / "fixtures/design-ir-observe/controls.ir.json"
OBSERVER: Path | None = None


def observe(out: Path, *steps: str, log: bool = True) -> subprocess.CompletedProcess:
    argv = [
        str(OBSERVER),
        "--input", str(FIXTURE),
        "--render", str(out / "render.png"),
        "--layout", str(out / "layout.json"),
        "--width", "240",
        "--height", "160",
        *steps,
    ]
    if log:
        argv += ["--interaction-log", str(out / "interaction.json")]
    return subprocess.run(argv, capture_output=True, text=True, timeout=120)


def load_log(out: Path) -> dict:
    return json.loads((out / "interaction.json").read_text())


def state_of(record: list, anchor: str) -> dict:
    matches = [entry for entry in record if entry["anchor"] == anchor]
    if len(matches) != 1:
        raise AssertionError(f"expected one state entry for {anchor}, got {matches}")
    return matches[0]


class InteractionReplayTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_dir(self, name: str) -> Path:
        path = self.tmp / name
        path.mkdir()
        return path

    def test_initial_state_lists_controls_in_tree_order(self) -> None:
        out = self.run_dir("initial")
        result = observe(out)
        self.assertEqual(result.returncode, 0, result.stderr)
        log = load_log(out)
        self.assertEqual(log["schema"], "pulp-design-ir-interaction-log-v1")
        self.assertEqual(log["steps"], [])
        self.assertEqual(
            [(e["anchor"], e["path"], e["kind"]) for e in log["initial_state"]],
            [("cutoff-knob", "0", "knob"), ("level-fader", "1", "fader")])
        self.assertEqual(state_of(log["initial_state"], "cutoff-knob")["value"], 0.5)
        self.assertEqual(state_of(log["initial_state"], "level-fader")["value"], 0.25)

    def test_drag_reaches_the_knob_and_changes_its_value(self) -> None:
        out = self.run_dir("drag")
        result = observe(out, "--drag", "40,60:40,10")
        self.assertEqual(result.returncode, 0, result.stderr)
        (step,) = load_log(out)["steps"]
        self.assertEqual(step["kind"], "drag")
        self.assertEqual(step["target"], "cutoff-knob")
        self.assertEqual(step["target_path"], "0")
        self.assertTrue(step["handled"])
        self.assertGreater(state_of(step["state"], "cutoff-knob")["value"], 0.5)
        self.assertEqual(state_of(step["state"], "level-fader")["value"], 0.25)

    def test_tab_traversal_moves_focus_forward_and_back(self) -> None:
        out = self.run_dir("tab")
        result = observe(out, "--key", "tab", "--key", "tab", "--key", "shift+tab")
        self.assertEqual(result.returncode, 0, result.stderr)
        steps = load_log(out)["steps"]
        self.assertEqual([s["focused"] for s in steps],
                         ["cutoff-knob", "level-fader", "cutoff-knob"])
        self.assertEqual([s["focused_path"] for s in steps], ["0", "1", "0"])
        self.assertTrue(all(s["handled"] for s in steps))

    def test_click_on_empty_area_reports_root_and_is_not_handled(self) -> None:
        out = self.run_dir("miss")
        result = observe(out, "--click", "200,150")
        self.assertEqual(result.returncode, 0, result.stderr)
        (step,) = load_log(out)["steps"]
        self.assertEqual(step["target"], "controls-root")
        self.assertEqual(step["target_path"], "")
        self.assertFalse(step["handled"])

    def test_key_without_focus_is_not_handled(self) -> None:
        out = self.run_dir("nofocus")
        result = observe(out, "--key", "up")
        self.assertEqual(result.returncode, 0, result.stderr)
        (step,) = load_log(out)["steps"]
        self.assertFalse(step["handled"])
        self.assertEqual(step["target_path"], "-")
        self.assertEqual(step["focused_path"], "-")

    def test_render_shows_post_interaction_state(self) -> None:
        before = self.run_dir("before")
        after = self.run_dir("after")
        self.assertEqual(observe(before).returncode, 0)
        self.assertEqual(observe(after, "--drag", "40,60:40,10").returncode, 0)
        digest = lambda p: hashlib.sha256((p / "render.png").read_bytes()).hexdigest()
        self.assertNotEqual(digest(before), digest(after))

    def test_replay_is_deterministic_across_processes(self) -> None:
        first = self.run_dir("first")
        second = self.run_dir("second")
        steps = ("--key", "tab", "--drag", "40,60:40,10", "--click", "100,60")
        self.assertEqual(observe(first, *steps).returncode, 0)
        self.assertEqual(observe(second, *steps).returncode, 0)
        for name in ("interaction.json", "render.png", "layout.json"):
            self.assertEqual((first / name).read_bytes(), (second / name).read_bytes(), name)

    def test_steps_without_a_log_are_refused(self) -> None:
        out = self.run_dir("nolog")
        result = observe(out, "--click", "40,40", log=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("require --interaction-log", result.stderr)
        self.assertFalse((out / "render.png").exists())

    def test_malformed_steps_are_refused(self) -> None:
        for args, message in (
            (("--click", "40"), "--click expects"),
            (("--click", "40,y"), "--click expects"),
            (("--drag", "1,2"), "--drag expects"),
            (("--key", "hyper"), "does not name a supported key"),
        ):
            with self.subTest(args=args):
                out = self.run_dir("bad-" + "-".join(a.strip("-,:") for a in args))
                result = observe(out, *args)
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)


def main() -> int:
    global OBSERVER
    parser = argparse.ArgumentParser()
    parser.add_argument("--observer", required=True, type=Path)
    args, rest = parser.parse_known_args()
    OBSERVER = args.observer
    if not OBSERVER.is_file():
        print(f"observer not built: {OBSERVER}", file=sys.stderr)
        return 1
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(InteractionReplayTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
