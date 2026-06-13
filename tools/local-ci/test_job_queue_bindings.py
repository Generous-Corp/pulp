#!/usr/bin/env python3
"""Tests for job queue facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("job_queue_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class JobQueueBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_queue_helpers_delegate_to_job_queue_module(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        bindings = {
            "_job_queue": types.SimpleNamespace(
                normalize_job=make_runner("normalize_job", {"id": "job-1"}),
                load_queue_unlocked=make_runner("load_queue_unlocked", [{"id": "job-1"}]),
                save_queue_unlocked=make_runner("save_queue_unlocked", None),
            )
        }
        job = {"branch": "feature/x"}
        queue = [{"id": "job-1"}]

        self.assertEqual(self.mod.normalize_job(bindings, job), {"id": "job-1"})
        self.assertEqual(self.mod.load_queue_unlocked(bindings), [{"id": "job-1"}])
        self.assertIsNone(self.mod.save_queue_unlocked(bindings, queue))

        self.assertEqual([call[0] for call in calls], [
            "normalize_job",
            "load_queue_unlocked",
            "save_queue_unlocked",
        ])
        self.assertEqual(calls[0][1], (job,))
        self.assertEqual(calls[1][1], ())
        self.assertEqual(calls[2][1], (queue,))

    def test_install_job_queue_helpers_wires_named_exports(self):
        calls = []

        def make_runner(name, value):
            def runner(*args, **kwargs):
                calls.append((name, args, kwargs))
                return value

            return runner

        bindings = {
            "_job_queue": types.SimpleNamespace(
                normalize_job=make_runner("normalize_job", {"id": "job-1"}),
                load_queue_unlocked=make_runner("load_queue_unlocked", [{"id": "job-1"}]),
            )
        }
        self.mod.install_job_queue_helpers(bindings, ("normalize_job", "load_queue_unlocked"))

        self.assertEqual(bindings["normalize_job"]({"branch": "feature/x"}), {"id": "job-1"})
        self.assertEqual(bindings["load_queue_unlocked"](), [{"id": "job-1"}])
        self.assertEqual(bindings["normalize_job"].__name__, "normalize_job")
        self.assertEqual([call[0] for call in calls], ["normalize_job", "load_queue_unlocked"])

    def test_install_job_queue_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_job_queue_helper = lambda _bindings: "future"

        self.mod.install_job_queue_helpers(bindings, ("future_job_queue_helper",))

        self.assertEqual(bindings["future_job_queue_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
