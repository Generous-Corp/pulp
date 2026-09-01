#!/usr/bin/env python3
"""Fail-closed integration tests for GPU-health run attestation tooling."""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
PRODUCER = HERE / "gpu_health_run_attestation.py"
VERIFIER = HERE / "verify_gpu_health_run_attestation.py"
SOURCE_ROOT = HERE.parents[1]
SCHEMA_PATH = Path("docs/contracts/gpu-health-run-attestation-v1.schema.json")
CREATED = "2026-09-01T07:00:00Z"
NOW = "2026-09-01T07:05:00Z"
sys.path.insert(0, str(HERE))
import json_schema_lite  # noqa: E402


def run(*args: str | Path, cwd: Path | None = None, check: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run([str(arg) for arg in args], cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if check and result.returncode:
        raise AssertionError(f"command failed: {args}\n{result.stdout}\n{result.stderr}")
    return result


class Fixture:
    def __init__(self, root: Path) -> None:
        self.repo = root / "repo"
        self.repo.mkdir()
        run("git", "init", "-q", "-b", "main", cwd=self.repo)
        run("git", "config", "user.name", "GPU Health Test", cwd=self.repo)
        run("git", "config", "user.email", "gpu-health@example.invalid", cwd=self.repo)
        (self.repo / "implementation.txt").write_text("implementation\n")
        self.commit("implementation")
        self.implementation = self.head()

        schema = self.repo / SCHEMA_PATH
        schema.parent.mkdir(parents=True)
        shutil.copyfile(SOURCE_ROOT / SCHEMA_PATH, schema)
        self.health_path = Path("docs/validation/gpu-health/a1/m5/pulp-doctor-gpu.json")
        health_file = self.repo / self.health_path
        health_file.parent.mkdir(parents=True)
        health_file.write_text(json.dumps({
            "schema": "pulp.gpu-health-result.v1",
            "version": 1,
            "run_id": "m5-a1-run-1",
            "probes": [{
                "probe_id": "gpu-compute-magnitude", "required": True, "verdict": "pass",
                "adapter": {
                    "status": "authentic", "class": "hardware", "name": "Apple M5",
                    "backend": "Metal", "device": "vendor=0x106b,device=0x0001",
                },
            }],
        }, sort_keys=True) + "\n")
        self.commit("publish result and schema")
        self.evidence = self.head()

        self.binary = root / "pulp-cpp"
        self.binary.write_bytes(b"fixture gpu-health producer\n")
        self.key = root / "host-key"
        run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", self.key)
        public_key = (root / "host-key.pub").read_text().split(" ", 2)
        self.public_key = " ".join(public_key[:2])
        self.trust = root / "trusted-hosts.json"
        self.trust.write_text(json.dumps({
            "schema": "pulp.gpu-health-trusted-hosts.v1", "version": 1,
            "hosts": [{
                "host_id": "m5", "stable_machine_id": "platform-uuid-m5",
                "public_key": self.public_key,
            }],
        }, sort_keys=True) + "\n")
        self.attestation_path = Path("docs/validation/gpu-health/a1/m5/run-attestation.json")
        run("python3", PRODUCER,
            "--repository", self.repo,
            "--health-result", self.health_path,
            "--output", self.repo / self.attestation_path,
            "--signing-key", self.key,
            "--host-id", "m5",
            "--stable-machine-id", "platform-uuid-m5",
            "--configuration", "power=low;fallback=false",
            "--probe-id", "gpu-compute-magnitude",
            "--implementation-revision", self.implementation,
            "--evidence-publication-revision", self.evidence,
            "--producer-binary", self.binary,
            "--producer-build-id", "pulp-build-fixture",
            "--producer-code-signature", "adhoc:fixture-cdhash",
            "--created-at", CREATED)
        self.commit("publish signed run attestation")
        self.attestation = self.head()
        run("git", "branch", "protected", self.attestation, cwd=self.repo)

    def commit(self, message: str) -> None:
        run("git", "add", ".", cwd=self.repo)
        run("git", "commit", "-q", "-m", message, cwd=self.repo)

    def head(self) -> str:
        return run("git", "rev-parse", "HEAD", cwd=self.repo).stdout.strip()

    def verify(self, *extra: str, protected_ref: str = "protected") -> subprocess.CompletedProcess:
        command: list[str | Path] = [
            "python3", VERIFIER,
            "--repository", self.repo,
            "--attestation-revision", self.attestation,
            "--attestation-path", self.attestation_path,
            "--protected-ref", protected_ref,
            "--trusted-hosts", self.trust,
            "--producer-binary", self.binary,
            "--expected-producer-build-id", "pulp-build-fixture",
            "--expected-producer-code-signature", "adhoc:fixture-cdhash",
            "--expected-host-id", "m5",
            "--expected-stable-machine-id", "platform-uuid-m5",
            "--expected-configuration", "power=low;fallback=false",
            "--expected-adapter-name", "Apple M5",
            "--expected-backend", "Metal",
            "--expected-device", "vendor=0x106b,device=0x0001",
            "--max-age-seconds", "600",
            "--now", NOW,
        ]
        command.extend(extra)
        return run(*command, check=False)


@unittest.skipUnless(shutil.which("ssh-keygen"), "ssh-keygen is required")
class GpuHealthRunAttestationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.fixture = Fixture(Path(self.temp.name))

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_protected_authenticated_run_verifies(self) -> None:
        result = self.fixture.verify()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verified", result.stdout)
        schema = json.loads((SOURCE_ROOT / SCHEMA_PATH).read_text())
        attestation = json.loads(
            (self.fixture.repo / self.fixture.attestation_path).read_text()
        )
        self.assertEqual(json_schema_lite.validate(attestation, schema), [])

    def test_cross_host_configuration_backend_device_and_adapter_reuse_fails(self) -> None:
        substitutions = {
            "--expected-host-id": "other-host",
            "--expected-stable-machine-id": "other-machine",
            "--expected-configuration": "fallback=true",
            "--expected-adapter-name": "substituted adapter",
            "--expected-backend": "Software",
            "--expected-device": "vendor=0xffff,device=0xffff",
        }
        for option, value in substitutions.items():
            with self.subTest(option=option):
                result = self.fixture.verify(option, value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("does not match verifier policy", result.stdout)

    def test_result_substitution_fails(self) -> None:
        attestation = json.loads((self.fixture.repo / self.fixture.attestation_path).read_text())
        attestation["gpu_health_result"]["sha256"] = "0" * 64
        (self.fixture.repo / self.fixture.attestation_path).write_text(
            json.dumps(attestation, sort_keys=True, separators=(",", ":")) + "\n")
        self.fixture.commit("substitute result digest")
        self.fixture.attestation = self.fixture.head()
        run("git", "branch", "-f", "protected", self.fixture.attestation,
            cwd=self.fixture.repo)
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("result digest", result.stdout)

    def test_stale_run_fails(self) -> None:
        result = self.fixture.verify("--max-age-seconds", "60")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stale", result.stdout)

    def test_unprotected_attestation_revision_fails(self) -> None:
        run("git", "branch", "-f", "protected", self.fixture.evidence,
            cwd=self.fixture.repo)
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not protected ancestry", result.stdout)

    def test_binary_substitution_fails(self) -> None:
        self.fixture.binary.write_bytes(b"substituted producer\n")
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("binary digest", result.stdout)


if __name__ == "__main__":
    unittest.main()
