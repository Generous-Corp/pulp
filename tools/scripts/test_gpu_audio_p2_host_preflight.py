import json
import subprocess
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

import gpu_audio_p2_host_preflight as preflight


class HostPreflightTests(unittest.TestCase):
    @staticmethod
    def collect_fixture(runner, source):
        identity = {
            "status": "valid", "path": "/fixture/build/pulp", "sha256": "a" * 64,
            "source_revision": source, "manifest_path": "/fixture/build_info.hpp",
            "manifest_sha256": "b" * 64,
        }
        with patch.object(preflight, "_gpu_health_identity", return_value=identity):
            return preflight.collect(runner=runner, source_revision=source)
    @staticmethod
    def thermal(state="nominal", sampled_at=None):
        return json.dumps({
            "schema": preflight.THERMAL_SCHEMA,
            "source": "Foundation.ProcessInfo.thermalState",
            "thermal_state": state,
            "thermal_state_code": {"nominal": 0, "fair": 1, "serious": 2,
                                    "critical": 3}.get(state, 99),
            "sampled_at": sampled_at or datetime.now(timezone.utc).isoformat(),
        })

    @staticmethod
    def runner(command, **kwargs):
        name = Path(command[0]).name
        if name.endswith("host_vitals.sh"):
            output = json.dumps({"level": "green", "ncpu": 8, "load1": "1.0"})
        elif name == "ps":
            output = " 10 2.0 /sbin/launchd\n 11 3.0 /System/Library/CoreServices/WindowServer\n"
        elif name == "swift":
            output = HostPreflightTests.thermal()
        elif name == "pmset":
            output = "Note: No thermal warning level has been recorded\nNote: No performance warning level has been recorded\n"
        else:
            output = 'AGXAccelerator busy 0\n'
        return subprocess.CompletedProcess(command, 0, output, "")

    @staticmethod
    def passing_gpu_runner(command, **kwargs):
        if Path(command[0]).name == "pulp" or command[0].endswith("/pulp"):
            now = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
            return subprocess.CompletedProcess(command, 0, json.dumps({
                "schema": "pulp.gpu-health-result.v2", "version": 2,
                "run_id": "run-p2-fixture", "measured_at_utc": now,
                "render_requested": True, "verdict": "pass", "health_state": "healthy",
                "recommendations": [], "probes": [{
                    "probe_id": "gpu-compute-magnitude", "required": True, "verdict": "pass",
                    "adapter": {"status": "authentic", "class": "hardware",
                                 "name": "Apple M5 Ultra", "vendor": "apple",
                                 "architecture": "metal-3", "backend": "Metal", "device": "apple-m5"},
                    "measurements": {"command_submitted": None, "readback_completed": None,
                                      "pixel_output_produced": None, "content_floor_passed": None,
                                      "compute_initialized": True, "compute_oracle_passed": True,
                                      "device_lost": False, "non_transparent_pixel_count": None,
                                      "distinct_color_count": None, "rgba_fingerprint": None},
                    "events": [{"sequence": 0, "stage": "adapter", "verdict": "pass",
                                 "code": "gpu_compute_adapter_acquired",
                                 "detail": "GpuCompute acquired authentic hardware adapter"},
                               {"sequence": 1, "stage": "compute", "verdict": "pass",
                                "code": "gpu_compute_oracle_passed",
                                "detail": "Compute output matched the independent oracle"}],
                }],
            }), "")
        return HostPreflightTests.runner(command, **kwargs)

    def test_authenticated_gpu_health_compute_fixture_passes(self):
        receipt = self.collect_fixture(self.passing_gpu_runner, "0" * 40)
        self.assertEqual(receipt["status"], "passed")
        self.assertTrue(receipt["quiet_host"])
        self.assertEqual(receipt["gpu_observation_status"], "passed")
        self.assertEqual(receipt["gpu_health_observation"]["status"], "valid")
        self.assertEqual(receipt["gpu_health_observation"]["observation"]["probe_id"], "gpu-compute-magnitude")
        self.assertEqual(receipt["gpu_health_observation"]["observation"]["adapter"]["backend"], "Metal")

    def test_stale_or_non_authentic_gpu_health_blocks(self):
        def stale_runner(command, **kwargs):
            result = self.passing_gpu_runner(command, **kwargs)
            if Path(command[0]).name == "pulp" or command[0].endswith("/pulp"):
                value = json.loads(result.stdout)
                value["measured_at_utc"] = "2020-01-01T00:00:00Z"
                result.stdout = json.dumps(value)
            return result

        receipt = self.collect_fixture(stale_runner, "3" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertIn("gpu_observation_unavailable", receipt["reasons"])
        self.assertEqual(receipt["gpu_health_observation"]["reason"], "measurement_stale")

        def identity_runner(command, **kwargs):
            result = self.passing_gpu_runner(command, **kwargs)
            if Path(command[0]).name == "pulp" or command[0].endswith("/pulp"):
                value = json.loads(result.stdout)
                value["probes"][0]["adapter"]["status"] = "unverified"
                result.stdout = json.dumps(value)
            return result

        receipt = self.collect_fixture(identity_runner, "4" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertEqual(receipt["gpu_health_observation"]["reason"],
                         "compute_identity_or_proof_invalid")

        def offset_timestamp_runner(command, **kwargs):
            result = self.passing_gpu_runner(command, **kwargs)
            if Path(command[0]).name == "pulp" or command[0].endswith("/pulp"):
                value = json.loads(result.stdout)
                value["measured_at_utc"] = value["measured_at_utc"].replace("Z", "+00:00")
                result.stdout = json.dumps(value)
            return result

        receipt = self.collect_fixture(offset_timestamp_runner, "5" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertEqual(receipt["gpu_health_observation"]["reason"], "measured_at_invalid")

    def test_gpu_health_requires_v2_shape_and_build_identity(self):
        raw = self.passing_gpu_runner(["pulp", "doctor", "gpu", "--json"]).stdout
        for field in ("version", "run_id", "render_requested", "recommendations", "probes"):
            value = json.loads(raw)
            value.pop(field)
            status, detail = preflight._parse_gpu_health(json.dumps(value))
            self.assertEqual(status, "unknown", field)
            self.assertEqual(detail["reason"], "top_level_shape_invalid", field)
        status, detail = preflight._parse_gpu_health(raw)
        self.assertEqual(status, "passed")
        identity = preflight._gpu_health_identity(
            ["/definitely/missing/pulp", "doctor", "gpu", "--json"], "a" * 40)
        self.assertEqual(identity["status"], "invalid")

    def test_positive_receipt_contains_hashed_real_observations(self):
        receipt = preflight.collect(runner=self.runner, source_revision="a" * 40)
        # IORegistry ``busy`` is diagnostic bookkeeping, not authenticated
        # queue-idle evidence, so this fixture must fail closed.
        self.assertEqual(receipt["status"], "blocked")
        self.assertFalse(receipt["quiet_host"])
        self.assertRegex(receipt["raw_observations_sha256"], preflight.SHA256_RE)
        self.assertEqual(receipt["gpu_busy_work_queues"], 0)
        self.assertEqual(receipt["gpu_observation_status"], "unknown")
        self.assertIn("gpu_observation_unavailable", receipt["reasons"])
        self.assertEqual(receipt["thermal_source"]["schema"], preflight.THERMAL_SCHEMA)
        self.assertRegex(receipt["thermal_source"]["helper_sha256"], preflight.SHA256_RE)
        self.assertEqual(receipt["thermal_source"]["tool"]["name"], "swift")

    def test_load_and_contention_are_negative_controls(self):
        def busy_runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if Path(command[0]).name == "ps":
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
            if Path(command[0]).name == "pmset":
                result.stdout = ""
            if Path(command[0]).name == "swift":
                result.stdout = "not-json"
            if Path(command[0]).name == "ioreg":
                result.stdout = "AGXAccelerator registered\n"
            return result
        receipt = preflight.collect(runner=missing_runner, source_revision="c" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertIn("thermal_observation_unknown", receipt["reasons"])
        self.assertIn("gpu_observation_unavailable", receipt["reasons"])

    def test_non_nominal_thermal_state_blocks(self):
        def warm_runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if Path(command[0]).name == "swift":
                result.stdout = self.thermal("serious")
            return result

        receipt = preflight.collect(runner=warm_runner, source_revision="7" * 40)
        self.assertEqual(receipt["thermal_state"], "serious")
        self.assertIn("thermal_state_not_nominal", receipt["reasons"])
        self.assertNotIn("thermal_observation_unknown", receipt["reasons"])

    def test_stale_or_malformed_foundation_observation_blocks(self):
        stale = datetime.now(timezone.utc).replace(year=2020).isoformat()

        def stale_runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if Path(command[0]).name == "swift":
                result.stdout = self.thermal(sampled_at=stale)
            return result

        receipt = preflight.collect(runner=stale_runner, source_revision="8" * 40)
        self.assertEqual(receipt["thermal_state"], "unknown")
        self.assertIn("thermal_observation_unknown", receipt["reasons"])

        def malformed_runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if Path(command[0]).name == "swift":
                result.stdout = json.dumps({"schema": preflight.THERMAL_SCHEMA,
                                            "source": "pmset", "thermal_state": "nominal"})
            return result

        receipt = preflight.collect(runner=malformed_runner, source_revision="9" * 40)
        self.assertEqual(receipt["thermal_state"], "unknown")
        self.assertIn("thermal_observation_unknown", receipt["reasons"])

    def test_missing_or_timed_out_foundation_tool_blocks(self):
        def missing_runner(command, **kwargs):
            if Path(command[0]).name == "swift":
                raise FileNotFoundError("swift")
            return self.runner(command, **kwargs)

        receipt = preflight.collect(runner=missing_runner, source_revision="a" * 40)
        self.assertIn("thermal_observation_unknown", receipt["reasons"])
        swift = next(item for item in receipt["observations"] if Path(item["argv"][0]).name == "swift")
        self.assertEqual(swift["returncode"], 127)

        def timeout_runner(command, **kwargs):
            if Path(command[0]).name == "swift":
                raise subprocess.TimeoutExpired(command, kwargs.get("timeout", 10))
            return self.runner(command, **kwargs)

        receipt = preflight.collect(runner=timeout_runner, source_revision="b" * 40)
        self.assertIn("thermal_observation_unknown", receipt["reasons"])
        swift = next(item for item in receipt["observations"] if Path(item["argv"][0]).name == "swift")
        self.assertEqual(swift["returncode"], 124)

    def test_source_revision_is_immutable(self):
        with patch.object(preflight, "_git_head", return_value="not-a-sha"):
            with self.assertRaisesRegex(RuntimeError, "immutable"):
                preflight.collect(runner=self.runner)

    def test_malformed_vitals_and_missing_tool_fail_closed(self):
        def malformed_runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if command[0].endswith("host_vitals.sh"):
                result.stdout = "not-json"
            if Path(command[0]).name == "swift":
                raise FileNotFoundError("swift")
            return result

        receipt = preflight.collect(runner=malformed_runner, source_revision="d" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertIn("host_vitals_invalid", receipt["reasons"])
        self.assertIn("thermal_observation_unknown", receipt["reasons"])

    def test_timeout_is_retained_as_unavailable_observation(self):
        def timeout_runner(command, **kwargs):
            if Path(command[0]).name == "ioreg":
                raise subprocess.TimeoutExpired(command, kwargs.get("timeout", 10))
            return self.runner(command, **kwargs)

        receipt = preflight.collect(runner=timeout_runner, source_revision="e" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertIn("gpu_observation_unavailable", receipt["reasons"])
        ioreg = next(item for item in receipt["observations"] if Path(item["argv"][0]).name == "ioreg")
        self.assertEqual(ioreg["returncode"], 124)

    def test_non_object_vitals_and_nonzero_status_fail_closed(self):
        def invalid_runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if command[0].endswith("host_vitals.sh"):
                result.stdout = "[]"
                result.returncode = 1
            return result

        receipt = preflight.collect(runner=invalid_runner, source_revision="f" * 40)
        self.assertEqual(receipt["status"], "blocked")
        self.assertIn("host_vitals_invalid", receipt["reasons"])
        self.assertIn("host_vitals_unavailable", receipt["reasons"])

    def test_nonfinite_load_and_process_cpu_fail_closed(self):
        for load in ("NaN", "Infinity"):
            def nonfinite_runner(command, **kwargs):
                result = self.runner(command, **kwargs)
                if command[0].endswith("host_vitals.sh"):
                    result.stdout = json.dumps({"level": "green", "ncpu": 8,
                                                "load1": load})
                if Path(command[0]).name == "ps":
                    result.stdout += " 99 nan /usr/bin/renderer\n"
                    result.stdout += " 100 25.0 kernel_task\n"
                return result

            receipt = preflight.collect(runner=nonfinite_runner,
                                        source_revision=("1" if load == "NaN" else "2") * 40)
            self.assertEqual(receipt["status"], "blocked")
            self.assertIn("load_unknown", receipt["reasons"])
            self.assertIn("contending_processes_present", receipt["reasons"])


if __name__ == "__main__":
    unittest.main()
