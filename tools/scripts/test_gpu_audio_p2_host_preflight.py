import json
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

import gpu_audio_p2_host_preflight as preflight


class HostPreflightTests(unittest.TestCase):
    @staticmethod
    def runner(command, **kwargs):
        name = command[0]
        if name.endswith("host_vitals.sh"):
            output = json.dumps({"level": "green", "ncpu": 8, "load1": "1.0"})
        elif name == "ps":
            output = " 10 2.0 /sbin/launchd\n 11 3.0 /System/Library/CoreServices/WindowServer\n"
        elif name == "pmset":
            output = "Note: No thermal warning level has been recorded\nNote: No performance warning level has been recorded\n"
        else:
            output = 'AGXAccelerator busy 0\n'
        return subprocess.CompletedProcess(command, 0, output, "")

    def test_positive_receipt_contains_hashed_real_observations(self):
        receipt = preflight.collect(runner=self.runner, source_revision="a" * 40)
        self.assertEqual(receipt["status"], "passed")
        self.assertTrue(receipt["quiet_host"])
        self.assertRegex(receipt["raw_observations_sha256"], preflight.SHA256_RE)
        self.assertEqual(receipt["gpu_busy_work_queues"], 0)

    def test_load_and_contention_are_negative_controls(self):
        def busy_runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if command[0] == "ps":
                result.stdout += " 99 75.0 /usr/bin/renderer\n"
            if command[0].endswith("host_vitals.sh"):
                result.stdout = json.dumps({"level": "green", "ncpu": 8, "load1": "8.0"})
            return result
        receipt = preflight.collect(runner=busy_runner, source_revision="b" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertFalse(receipt["quiet_host"])
        self.assertIn("load_above_quiet_threshold", receipt["reasons"])
        self.assertIn("contending_processes_present", receipt["reasons"])

    def test_unknown_thermal_or_gpu_observation_fails_closed(self):
        def missing_runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if command[0] == "pmset":
                result.stdout = ""
            if command[0] == "ioreg":
                result.stdout = "AGXAccelerator registered\n"
            return result
        receipt = preflight.collect(runner=missing_runner, source_revision="c" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertIn("thermal_observation_unknown", receipt["reasons"])
        self.assertIn("gpu_observation_unavailable", receipt["reasons"])

    def test_source_revision_is_immutable(self):
        with patch.object(preflight, "_git_head", return_value="not-a-sha"):
            with self.assertRaisesRegex(RuntimeError, "immutable"):
                preflight.collect(runner=self.runner)

    def test_malformed_vitals_and_missing_tool_fail_closed(self):
        def malformed_runner(command, **kwargs):
            if command[0] == "pmset":
                raise FileNotFoundError("pmset")
            result = self.runner(command, **kwargs)
            if command[0].endswith("host_vitals.sh"):
                result.stdout = "not-json"
            return result

        receipt = preflight.collect(runner=malformed_runner, source_revision="d" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertIn("host_vitals_invalid", receipt["reasons"])
        self.assertIn("thermal_observation_unknown", receipt["reasons"])

    def test_timeout_is_retained_as_unavailable_observation(self):
        def timeout_runner(command, **kwargs):
            if command[0] == "ioreg":
                raise subprocess.TimeoutExpired(command, kwargs.get("timeout", 10))
            return self.runner(command, **kwargs)

        receipt = preflight.collect(runner=timeout_runner, source_revision="e" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertIn("gpu_observation_unavailable", receipt["reasons"])
        ioreg = next(item for item in receipt["observations"] if item["argv"][0] == "ioreg")
        self.assertEqual(ioreg["returncode"], 124)


if __name__ == "__main__":
    unittest.main()
