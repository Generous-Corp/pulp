#!/usr/bin/env python3
"""The macOS lane rule: default off, new heads deferred, only the App's label counts."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import macos_pr_lane as lane  # noqa: E402

APP = lane.TRUSTED_ACTOR


def decide(*, gating="true", event_name="pull_request", action="opened",
           label_name="", sender="", labels=(), actor=None, lookups=None):
    def label_actor():
        if lookups is not None:
            lookups.append(1)
        return actor
    return lane.decide(gating=gating, event_name=event_name, action=action,
                       label_name=label_name, sender=sender, labels=list(labels),
                       label_actor=label_actor)[0]


class MacosPrLaneTest(unittest.TestCase):
    def test_gating_off_keeps_every_event_on_the_full_lane(self) -> None:
        for gating in ("", "false", "1", "TRUE"):
            for action in ("opened", "synchronize", "labeled"):
                with self.subTest(gating=gating, action=action):
                    self.assertEqual(decide(gating=gating, action=action), "full")

    def test_only_pull_requests_are_gated(self) -> None:
        for event in ("merge_group", "push", "workflow_dispatch"):
            with self.subTest(event=event):
                self.assertEqual(decide(event_name=event, action="synchronize"), "full")

    def test_a_new_head_is_deferred_even_while_it_carries_the_label(self) -> None:
        lookups = []
        self.assertEqual(decide(action="synchronize", labels=[lane.LABEL], actor=APP,
                                lookups=lookups), "contract")
        self.assertEqual(lookups, [], "synchronize never trusts the label, so never asks")

    def test_a_labeled_event_counts_only_from_the_fleet_app(self) -> None:
        self.assertEqual(decide(action="labeled", label_name=lane.LABEL, sender=APP,
                                labels=[lane.LABEL]), "full")
        for sender in ("a-maintainer", "github-actions[bot]", ""):
            with self.subTest(sender=sender):
                self.assertEqual(decide(action="labeled", label_name=lane.LABEL,
                                        sender=sender, labels=[lane.LABEL]), "contract")

    def test_an_existing_label_counts_only_when_the_app_applied_it_last(self) -> None:
        for action in ("opened", "reopened"):
            with self.subTest(action=action):
                self.assertEqual(decide(action=action, labels=[lane.LABEL], actor=APP), "full")
                self.assertEqual(decide(action=action, labels=[lane.LABEL],
                                        actor="a-maintainer"), "contract")
                # An unreadable timeline is untrusted.
                self.assertEqual(decide(action=action, labels=[lane.LABEL], actor=None),
                                 "contract")

    def test_no_label_is_deferred_without_asking(self) -> None:
        lookups = []
        self.assertEqual(decide(action="opened", labels=["queue-unblock"], actor=APP,
                                lookups=lookups), "contract")
        self.assertEqual(lookups, [])

    def test_cli_prints_one_decision(self) -> None:
        proc = subprocess.run(
            [sys.executable, str(Path(lane.__file__)), "--gating", "true",
             "--event-name", "pull_request", "--action", "labeled",
             "--label-name", lane.LABEL, "--sender", APP,
             "--labels-json", json.dumps([lane.LABEL])],
            capture_output=True, text=True, encoding="utf-8", check=True)
        self.assertEqual(json.loads(proc.stdout)["lane"], "full")
        # Malformed labels fall back to none, which defers.
        proc = subprocess.run(
            [sys.executable, str(Path(lane.__file__)), "--gating", "true",
             "--event-name", "pull_request", "--action", "opened", "--labels-json", "{"],
            capture_output=True, text=True, encoding="utf-8", check=True)
        self.assertEqual(json.loads(proc.stdout)["lane"], "contract")


if __name__ == "__main__":
    unittest.main()
