#!/usr/bin/env python3
"""Fail-closed integration tests for GPU-health run attestation tooling."""

from __future__ import annotations

import base64
import hashlib
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
HEALTH_SCHEMA_PATH = Path("docs/contracts/gpu-health-result-v1.schema.json")
CREATED = "2026-09-01T07:00:00Z"
MEASURED = "2026-09-01T06:59:00Z"
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
        shutil.copyfile(SOURCE_ROOT / HEALTH_SCHEMA_PATH,
                        self.repo / HEALTH_SCHEMA_PATH)
        self.health_path = Path("docs/validation/gpu-health/a1/m5/pulp-doctor-gpu.json")
        health_file = self.repo / self.health_path
        health_file.parent.mkdir(parents=True)
        health = json.loads(
            (SOURCE_ROOT / "test/fixtures/gpu-ux/pass-hardware.json").read_text()
        )
        health["run_id"] = "m5-a1-run-1"
        health["measured_at_utc"] = MEASURED
        health["probes"][0]["probe_id"] = "gpu-compute-magnitude"
        health["probes"][0]["adapter"].update({
            "name": "Apple M5", "device": "vendor=0x106b,device=0x0001",
        })
        health_file.write_text(json.dumps(health, sort_keys=True) + "\n")
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
        result = self.produce(self.evidence, CREATED)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        self.commit("publish signed run attestation")
        self.attestation = self.head()
        run("git", "branch", "protected", self.attestation, cwd=self.repo)

    def produce(self, evidence: str, created_at: str,
                output: Path | None = None) -> subprocess.CompletedProcess:
        output = output or self.repo / self.attestation_path
        return run("python3", PRODUCER,
            "--repository", self.repo,
            "--health-result", self.health_path,
            "--output", output,
            "--signing-key", self.key,
            "--host-id", "m5",
            "--stable-machine-id", "platform-uuid-m5",
            "--configuration", "power=low;fallback=false",
            "--probe-id", "gpu-compute-magnitude",
            "--implementation-revision", self.implementation,
            "--evidence-publication-revision", evidence,
            "--producer-binary", self.binary,
            "--producer-build-id", "pulp-build-fixture",
            "--producer-code-signature", "adhoc:fixture-cdhash",
            "--created-at", created_at, check=False)

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

    def mutate_attestation(self, operation) -> None:
        path = self.repo / self.attestation_path
        value = json.loads(path.read_text())
        operation(value)
        path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n")
        self.commit("publish mutated attestation")
        self.attestation = self.head()
        run("git", "branch", "-f", "protected", self.attestation, cwd=self.repo)

    def publish_adversarially_signed(self, evidence: str) -> None:
        """Sign an evidence binding that the real producer correctly refuses."""
        path = self.repo / self.attestation_path
        value = json.loads(path.read_text())
        health = (self.repo / self.health_path).read_bytes()
        health_value = json.loads(health)
        value["evidence_publication_revision"] = evidence
        value["gpu_health_result"].update({
            "sha256": hashlib.sha256(health).hexdigest(),
            "run_id": health_value.get("run_id", "m5-a1-run-1"),
            "schema": health_value.get("schema", "pulp.gpu-health-result.v1"),
            "measured_at_utc": health_value.get("measured_at_utc", MEASURED),
        })
        unsigned = dict(value)
        del unsigned["authentication"]
        with tempfile.TemporaryDirectory() as temp_dir:
            statement = Path(temp_dir) / "statement.json"
            statement.write_text(
                json.dumps(unsigned, allow_nan=False, ensure_ascii=False,
                           separators=(",", ":"), sort_keys=True) + "\n"
            )
            run("ssh-keygen", "-Y", "sign", "-f", self.key,
                "-n", "pulp.gpu-health-run-attestation.v1", statement)
            signature = (Path(str(statement) + ".sig")).read_bytes()
        value["authentication"]["signature"] = base64.b64encode(signature).decode()
        path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n")
        self.commit("publish adversarially signed attestation")
        self.attestation = self.head()
        run("git", "branch", "-f", "protected", self.attestation, cwd=self.repo)


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

    def test_untrusted_signer_fails(self) -> None:
        other_key = Path(self.temp.name) / "other-key"
        run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", other_key)
        public_parts = Path(str(other_key) + ".pub").read_text().split(" ", 2)
        trust = json.loads(self.fixture.trust.read_text())
        trust["hosts"][0]["public_key"] = " ".join(public_parts[:2])
        self.fixture.trust.write_text(json.dumps(trust, sort_keys=True) + "\n")
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fingerprint does not match", result.stdout)

    def test_health_result_schema_and_semantic_failures_are_rejected_before_signing(self) -> None:
        health_path = self.fixture.repo / self.fixture.health_path
        valid = json.loads(health_path.read_text())
        cases = (
            ("missing-required", lambda value: value.pop("recommendations"),
             "canonical schema"),
            ("wrong-type", lambda value: value.update(render_requested="yes"),
             "canonical schema"),
            ("semantic-inconsistent", lambda value: value.update(
                verdict="fail", health_state="failed"), "semantic contract"),
        )
        for label, operation, expected in cases:
            with self.subTest(label=label):
                changed = json.loads(json.dumps(valid))
                operation(changed)
                health_path.write_text(json.dumps(changed, sort_keys=True) + "\n")
                self.fixture.commit(label)
                evidence = self.fixture.head()
                result = self.fixture.produce(
                    evidence, CREATED, Path(self.temp.name) / f"{label}.json"
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.fixture.publish_adversarially_signed(evidence)
                verified = self.fixture.verify()
                self.assertNotEqual(verified.returncode, 0)
                self.assertIn(expected, verified.stdout)

    def test_attestation_schema_rejects_timestamp_path_pattern_and_length_drift(self) -> None:
        cases = (
            ("timestamp", lambda value: value.update(created_at="not-a-time"), ()),
            ("path", lambda value: value["gpu_health_result"].update(path="../result.json"), ()),
            ("pattern", lambda value: value["authentication"].update(
                signer_key_fingerprint="not-a-fingerprint"), ()),
            ("length", lambda value: value["host"].update(host_id="x" * 1025),
             ("--expected-host-id", "x" * 1025)),
        )
        for label, operation, extra in cases:
            with self.subTest(label=label):
                case_root = Path(self.temp.name) / label
                case_root.mkdir()
                fixture = Fixture(case_root)
                fixture.mutate_attestation(operation)
                result = fixture.verify(*extra)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(
                    "canonical schema" in result.stdout or "malformed" in result.stdout,
                    result.stdout,
                )

    def test_canonical_schema_identity_divergence_is_rejected(self) -> None:
        for relative, expected in (
            (SCHEMA_PATH, "run-attestation schema"),
            (HEALTH_SCHEMA_PATH, "GPU-health schema"),
        ):
            with self.subTest(path=relative):
                case_root = Path(self.temp.name) / relative.name
                case_root.mkdir()
                fixture = Fixture(case_root)
                path = fixture.repo / relative
                schema = json.loads(path.read_text())
                schema["$id"] = "https://example.invalid/substituted.schema.json"
                path.write_text(json.dumps(schema, sort_keys=True) + "\n")
                fixture.commit("substitute canonical schema identity")
                result = fixture.produce(
                    fixture.head(), CREATED, case_root / "substituted.json"
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)

    def test_new_signature_cannot_refresh_an_old_measurement(self) -> None:
        result = self.fixture.produce(self.fixture.evidence, NOW)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.fixture.commit("republish newer signature over old measurement")
        self.fixture.attestation = self.fixture.head()
        run("git", "branch", "-f", "protected", self.fixture.attestation,
            cwd=self.fixture.repo)
        verified = self.fixture.verify("--max-age-seconds", "60")
        self.assertNotEqual(verified.returncode, 0)
        self.assertIn("GPU measurement is stale", verified.stdout)

    def test_attestation_chronology_and_future_time_fail_closed(self) -> None:
        cases = (
            ("predates", "2026-09-01T06:58:00Z", NOW,
             "predates the GPU measurement"),
            ("future", "2026-09-01T07:06:00Z", NOW,
             "attestation is dated in the future"),
        )
        for label, created, now, expected in cases:
            with self.subTest(label=label):
                case_root = Path(self.temp.name) / f"chronology-{label}"
                case_root.mkdir()
                fixture = Fixture(case_root)
                result = fixture.produce(fixture.evidence, created)
                self.assertEqual(result.returncode, 0, result.stderr)
                fixture.commit(f"publish {label} chronology fixture")
                fixture.attestation = fixture.head()
                run("git", "branch", "-f", "protected", fixture.attestation,
                    cwd=fixture.repo)
                verified = fixture.verify("--now", now)
                self.assertNotEqual(verified.returncode, 0)
                self.assertIn(expected, verified.stdout)


if __name__ == "__main__":
    unittest.main()
