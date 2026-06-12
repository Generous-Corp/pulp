#!/usr/bin/env python3
"""Tests for job queue facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("job_queue_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("job_queue_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


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


if __name__ == "__main__":
    unittest.main()
