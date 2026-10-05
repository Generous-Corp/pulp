#!/usr/bin/env python3
"""Tests for schedule_backstop_check.py."""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import schedule_backstop_check as check  # noqa: E402


def workflow(cron="*/30 * * * *", dispatch="  workflow_dispatch: {}\n", concurrency=True,
             runs_on="ubuntu-latest"):
    text = "name: net\non:\n"
    if cron:
        text += f"  schedule:\n    - cron: '{cron}'\n"
    text += dispatch
    if concurrency:
        text += "concurrency:\n  group: net\n"
    text += f"jobs:\n  check:\n    runs-on: {runs_on}\n    steps:\n      - run: echo hi\n"
    return text


class Fixture:
    def __init__(self, workflows, listed, excluded=()):
        self.tmp = tempfile.TemporaryDirectory()
        root = self.tmp.name
        os.makedirs(os.path.join(root, ".github", "workflows"))
        for name, body in workflows.items():
            with open(os.path.join(root, ".github", "workflows", name), "w") as out:
                out.write(body)
        with open(os.path.join(root, ".github", "schedule-backstop.json"), "w") as out:
            json.dump({
                "schema_version": 1,
                "ref": "main",
                "workflows": [{"file": f, "cadence_minutes": c} for f, c in listed],
                "excluded": [{"file": f, "reason": r} for f, r in excluded],
            }, out)
        self.root = root

    def violations(self):
        return check.check(self.root)


class CadenceTests(unittest.TestCase):
    def test_step_cron(self):
        self.assertEqual(check.hourly_cadence("*/15 * * * *"), 15)

    def test_listed_minutes_take_the_longest_gap(self):
        self.assertEqual(check.hourly_cadence("17,47 * * * *"), 30)
        self.assertEqual(check.hourly_cadence("5,15 * * * *"), 50)

    def test_single_minute_is_hourly(self):
        self.assertEqual(check.hourly_cadence("17 * * * *"), 60)

    def test_daily_and_weekly_are_not_hourly(self):
        self.assertIsNone(check.hourly_cadence("41 5 * * *"))
        self.assertIsNone(check.hourly_cadence("0 8 * * 1"))


class ManifestTests(unittest.TestCase):
    def test_consistent_fixture_passes(self):
        fx = Fixture({"net.yml": workflow()}, [("net.yml", 30)])
        self.assertEqual(fx.violations(), [])

    def test_cadence_drift_is_reported(self):
        fx = Fixture({"net.yml": workflow(cron="*/15 * * * *")}, [("net.yml", 30)])
        self.assertTrue(any("cadence_minutes=30" in v for v in fx.violations()))

    def test_missing_dispatch_is_reported(self):
        fx = Fixture({"net.yml": workflow(dispatch="")}, [("net.yml", 30)])
        self.assertTrue(any("no workflow_dispatch" in v for v in fx.violations()))

    def test_required_input_without_default_is_reported(self):
        dispatch = ("  workflow_dispatch:\n    inputs:\n      sha:\n"
                    "        required: true\n")
        fx = Fixture({"net.yml": workflow(dispatch=dispatch)}, [("net.yml", 30)])
        self.assertTrue(any("'sha' is required" in v for v in fx.violations()))

    def test_missing_concurrency_is_reported(self):
        fx = Fixture({"net.yml": workflow(concurrency=False)}, [("net.yml", 30)])
        self.assertTrue(any("concurrency" in v for v in fx.violations()))

    def test_self_hosted_runner_is_reported(self):
        fx = Fixture({"net.yml": workflow(runs_on="[self-hosted, macOS]")}, [("net.yml", 30)])
        self.assertTrue(any("self-hosted" in v for v in fx.violations()))

    def test_unlisted_frequent_cron_is_reported(self):
        fx = Fixture({"net.yml": workflow(), "other.yml": workflow()}, [("net.yml", 30)])
        self.assertTrue(any(v.startswith("other.yml") for v in fx.violations()))

    def test_exclusion_with_reason_satisfies_the_sweep(self):
        fx = Fixture({"net.yml": workflow(), "other.yml": workflow()}, [("net.yml", 30)],
                     excluded=[("other.yml", "drained by its own event trigger")])
        self.assertEqual(fx.violations(), [])

    def test_daily_cron_is_not_swept(self):
        fx = Fixture({"net.yml": workflow(), "nightly.yml": workflow(cron="41 5 * * *")},
                     [("net.yml", 30)])
        self.assertEqual(fx.violations(), [])

    def test_a_daily_workflow_may_be_listed_at_1440(self):
        fx = Fixture({"nightly.yml": workflow(cron="41 7 * * *")}, [("nightly.yml", 1440)])
        self.assertEqual(fx.violations(), [])

    def test_a_daily_listing_needs_exactly_one_daily_cron_and_1440(self):
        cases = [
            ({"n.yml": workflow(cron="41 7 * * *")}, [("n.yml", 60)]),       # wrong cadence
            ({"n.yml": workflow(cron="41 7 * * 1")}, [("n.yml", 1440)]),     # weekly
            ({"n.yml": workflow(cron="41 */6 * * *")}, [("n.yml", 1440)]),   # every six hours
            ({"n.yml": workflow(cron="*/30 * * * *")}, [("n.yml", 1440)]),   # hourly listed as daily
            ({"n.yml": workflow(cron="41 7 * * *")}, [("n.yml", 720)]),      # no other long cadence
        ]
        for workflows, listed in cases:
            self.assertNotEqual(Fixture(workflows, listed).violations(), [], listed)

    def test_missing_listed_file_is_reported(self):
        fx = Fixture({}, [("gone.yml", 30)])
        self.assertTrue(any("does not exist" in v for v in fx.violations()))


class RepositoryTests(unittest.TestCase):
    def test_repository_manifest_is_consistent(self):
        self.assertEqual(check.check(REPO), [])

    def test_repository_sweep_sees_every_frequent_cron(self):
        # Control: the sweep must find the frequent safety nets, otherwise an
        # empty violation list proves nothing about completeness.
        with open(os.path.join(REPO, check.MANIFEST)) as source:
            manifest = json.load(source)
        listed = {row["file"] for row in manifest["workflows"]}
        self.assertGreaterEqual(len(listed), 10)
        for name in listed:
            with open(os.path.join(REPO, check.WORKFLOWS, name)) as source:
                self.assertIsNotNone(check.listed_cadence(check.yaml.safe_load(source)), name)


if __name__ == "__main__":
    unittest.main()
