#!/usr/bin/env python3
"""Fail-closed integration tests for GPU-health run attestation tooling."""

from __future__ import annotations

import base64
import contextlib
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
PRODUCER = HERE / "gpu_health_run_attestation.py"
VERIFIER = HERE / "verify_gpu_health_run_attestation.py"
SOURCE_ROOT = HERE.parents[1]
SCHEMA_PATH = Path("docs/contracts/gpu-health-run-attestation-v1.schema.json")
HEALTH_SCHEMA_PATH = Path("docs/contracts/gpu-health-result-v2.schema.json")
VERIFICATION_SCHEMA_PATH = Path(
    "docs/contracts/gpu-health-run-attestation-verification-v1.schema.json"
)
CREATED = "2026-09-01T07:00:00Z"
MEASURED = "2026-09-01T06:59:00Z"
NOW = "2026-09-01T07:05:00Z"
RAW_MACHINE_ID = "fixture-private-platform-uuid-m5"
sys.path.insert(0, str(HERE))
import json_schema_lite  # noqa: E402
import gpu_health_run_attestation as producer  # noqa: E402
import verify_gpu_health_run_attestation as verifier  # noqa: E402

MACHINE_ID_SHA256 = hashlib.sha256(
    b"pulp.gpu-health.machine.v1\0" + RAW_MACHINE_ID.encode("utf-8")
).hexdigest()


def run(*args: str | Path, cwd: Path | None = None, check: bool = True,
        input_text: str | None = None) -> subprocess.CompletedProcess:
    result = subprocess.run([str(arg) for arg in args], cwd=cwd, text=True,
                            input=input_text, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
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
        (self.repo / "bootstrap.txt").write_text("repository bootstrap\n")
        self.commit("repository bootstrap")
        (self.repo / "implementation.txt").write_text("implementation\n")
        self.commit("implementation")
        self.implementation = self.head()

        schema = self.repo / SCHEMA_PATH
        schema.parent.mkdir(parents=True)
        shutil.copyfile(SOURCE_ROOT / SCHEMA_PATH, schema)
        shutil.copyfile(SOURCE_ROOT / HEALTH_SCHEMA_PATH,
                        self.repo / HEALTH_SCHEMA_PATH)
        shutil.copyfile(SOURCE_ROOT / VERIFICATION_SCHEMA_PATH,
                        self.repo / VERIFICATION_SCHEMA_PATH)
        self.health_path = Path("docs/validation/gpu-health/a1/m5/pulp-doctor-gpu.json")
        health_file = self.repo / self.health_path
        health_file.parent.mkdir(parents=True)
        health = json.loads(
            (SOURCE_ROOT / "test/fixtures/gpu-ux/pass-hardware.json").read_text()
        )
        health["run_id"] = "m5-a1-run-1"
        health["schema"] = "pulp.gpu-health-result.v2"
        health["version"] = 2
        health["measured_at_utc"] = MEASURED
        health["probes"][0]["probe_id"] = "gpu-compute-magnitude"
        health["probes"][0]["adapter"].update({
            "name": "Apple M5", "device": "vendor=0x106b,device=0x0001",
        })
        alternate_probe = json.loads(json.dumps(health["probes"][0]))
        alternate_probe["probe_id"] = "renderer3d-frame"
        for event in alternate_probe["events"]:
            event["sequence"] += len(health["probes"][0]["events"])
        health["probes"].append(alternate_probe)
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
                "host_id": "m5",
                "stable_machine_id_sha256": MACHINE_ID_SHA256,
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

    def produce(
        self, evidence: str, created_at: str, output: Path | None = None,
        signing_key: Path | None = None,
        machine_input: str = RAW_MACHINE_ID + "\n",
        machine_option: tuple[str, ...] = ("--stable-machine-id-stdin",),
    ) -> subprocess.CompletedProcess:
        output = output or self.repo / self.attestation_path
        signing_key = signing_key or self.key
        return run("python3", PRODUCER,
            "--repository", self.repo,
            "--health-result", self.health_path,
            "--output", output,
            "--signing-key", signing_key,
            "--host-id", "m5",
            *machine_option,
            "--configuration", "power=low;fallback=false",
            "--probe-id", "gpu-compute-magnitude",
            "--implementation-revision", self.implementation,
            "--evidence-publication-revision", evidence,
            "--producer-binary", self.binary,
            "--producer-build-id", "pulp-build-fixture",
            "--producer-code-signature", "adhoc:fixture-cdhash",
            "--created-at", created_at, check=False, input_text=machine_input)

    def commit(self, message: str) -> None:
        run("git", "add", ".", cwd=self.repo)
        run("git", "commit", "-q", "-m", message, cwd=self.repo)

    def head(self) -> str:
        return run("git", "rev-parse", "HEAD", cwd=self.repo).stdout.strip()

    def verify_arguments(self, *extra: str, protected_ref: str = "protected",
                         attestation_revision: str | None = None) -> list[str | Path]:
        command: list[str | Path] = [
            "--repository", self.repo,
            "--attestation-revision", attestation_revision or self.attestation,
            "--attestation-path", self.attestation_path,
            "--protected-ref", protected_ref,
            "--trusted-hosts", self.trust,
            "--producer-binary", self.binary,
            "--expected-implementation-revision", self.implementation,
            "--expected-producer-build-id", "pulp-build-fixture",
            "--expected-producer-code-signature", "adhoc:fixture-cdhash",
            "--expected-host-id", "m5",
            "--expected-stable-machine-id-sha256", MACHINE_ID_SHA256,
            "--expected-configuration", "power=low;fallback=false",
            "--expected-probe-id", "gpu-compute-magnitude",
            "--expected-adapter-name", "Apple M5",
            "--expected-backend", "Metal",
            "--expected-device", "vendor=0x106b,device=0x0001",
            "--max-age-seconds", "600",
        ]
        command.extend(extra)
        return command

    def verify(self, *extra: str, protected_ref: str = "protected",
               attestation_revision: str | None = None,
               now: str = NOW) -> subprocess.CompletedProcess:
        command = self.verify_arguments(
            *extra, protected_ref=protected_ref,
            attestation_revision=attestation_revision,
        )
        output = io.StringIO()
        errors = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            try:
                status = verifier.verify(
                    [str(argument) for argument in command],
                    clock=lambda: verifier.parse_time(now, "test clock"),
                )
            except SystemExit as error:
                status = int(error.code)
        return subprocess.CompletedProcess(
            [str(VERIFIER), *map(str, command)], status,
            stdout=output.getvalue(), stderr=errors.getvalue(),
        )

    def mutate_attestation(self, operation) -> None:
        path = self.repo / self.attestation_path
        value = json.loads(path.read_text())
        operation(value)
        path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n")
        self.commit("publish mutated attestation")
        self.attestation = self.head()
        run("git", "branch", "-f", "protected", self.attestation, cwd=self.repo)

    def publish_adversarially_signed(self, evidence: str,
                                     signing_key: Path | None = None,
                                     operation=None) -> None:
        """Sign an evidence binding that the real producer correctly refuses."""
        signing_key = signing_key or self.key
        path = self.repo / self.attestation_path
        value = json.loads(path.read_text())
        health = (self.repo / self.health_path).read_bytes()
        health_value = json.loads(health)
        value["evidence_publication_revision"] = evidence
        value["gpu_health_result"].update({
            "sha256": hashlib.sha256(health).hexdigest(),
            "run_id": health_value.get("run_id", "m5-a1-run-1"),
            "schema": health_value.get("schema", "pulp.gpu-health-result.v2"),
            "measured_at_utc": health_value.get("measured_at_utc", MEASURED),
        })
        if operation is not None:
            operation(value)
        unsigned = dict(value)
        del unsigned["authentication"]
        with tempfile.TemporaryDirectory() as temp_dir:
            statement = Path(temp_dir) / "statement.json"
            statement.write_text(
                json.dumps(unsigned, allow_nan=False, ensure_ascii=False,
                           separators=(",", ":"), sort_keys=True) + "\n"
            )
            run("ssh-keygen", "-Y", "sign", "-f", signing_key,
                "-n", "pulp.gpu-health-run-attestation.v1", statement)
            signature = (Path(str(statement) + ".sig")).read_bytes()
        public_key = run("ssh-keygen", "-y", "-f", signing_key).stdout.strip()
        with tempfile.NamedTemporaryFile("w", encoding="utf-8") as key_file:
            key_file.write(public_key + "\n")
            key_file.flush()
            fingerprint_output = run(
                "ssh-keygen", "-lf", key_file.name, "-E", "sha256"
            ).stdout
        value["authentication"]["signer_key_fingerprint"] = (
            "SHA256:" + fingerprint_output.split("SHA256:", 1)[1].split()[0]
        )
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
        record = json.loads(result.stdout)
        self.assertEqual(record["status"], "verified")
        self.assertEqual(record["implementation_revision"], self.fixture.implementation)
        self.assertEqual(record["selection"]["probe_id"], "gpu-compute-magnitude")
        self.assertEqual(record["evidence_revision"], self.fixture.evidence)
        self.assertEqual(record["attestation"]["revision"], self.fixture.attestation)
        self.assertEqual(record["attestation"]["path"], str(self.fixture.attestation_path))
        self.assertEqual(record["health_result"]["schema"],
                         "pulp.gpu-health-result.v2")
        self.assertEqual(record["health_result"]["run_id"], "m5-a1-run-1")
        self.assertEqual(record["health_result"]["measured_at_utc"], MEASURED)
        self.assertEqual(record["max_age_seconds"], 600)
        self.assertEqual(record["host"]["stable_machine_id_sha256"],
                         MACHINE_ID_SHA256)
        self.assertEqual(record["trusted_host"]["key_type"], "ssh-ed25519")
        self.assertEqual(record["producer"]["build_id"], "pulp-build-fixture")
        self.assertEqual(record["verifier"]["contract_version"], 1)
        verifier_artifacts = [record["verifier"]["entrypoint"],
                              *record["verifier"]["dependencies"]]
        for artifact in verifier_artifacts:
            self.assertEqual(
                artifact["sha256"],
                hashlib.sha256(Path(artifact["path"]).read_bytes()).hexdigest(),
            )
        for name, relative in (
            ("attestation", SCHEMA_PATH),
            ("health_result", HEALTH_SCHEMA_PATH),
            ("verification", VERIFICATION_SCHEMA_PATH),
        ):
            binding = record["canonical_schemas"][name]
            self.assertEqual(binding["path"], str(relative))
            self.assertEqual(
                binding["blob_sha1"],
                run("git", "rev-parse", f"{self.fixture.evidence}:{relative}",
                    cwd=self.fixture.repo).stdout.strip(),
            )
        verification_schema = json.loads(
            (SOURCE_ROOT / VERIFICATION_SCHEMA_PATH).read_text()
        )
        self.assertEqual(json_schema_lite.validate(record, verification_schema), [])
        schema = json.loads((SOURCE_ROOT / SCHEMA_PATH).read_text())
        attestation = json.loads(
            (self.fixture.repo / self.fixture.attestation_path).read_text()
        )
        self.assertEqual(json_schema_lite.validate(attestation, schema), [])
        self.assertEqual(
            attestation["host"]["stable_machine_id_sha256"], MACHINE_ID_SHA256
        )
        self.assertNotIn(RAW_MACHINE_ID, json.dumps(attestation))
        self.assertNotIn(RAW_MACHINE_ID, self.fixture.trust.read_text())
        self.assertNotIn(RAW_MACHINE_ID, result.stdout)
        self.assertNotIn(self.fixture.public_key, result.stdout)

    def test_verification_record_schema_rejects_missing_substitution_and_unknown(self) -> None:
        result = self.fixture.verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        schema = json.loads((SOURCE_ROOT / VERIFICATION_SCHEMA_PATH).read_text())
        mutations = (
            lambda value: value.pop("status"),
            lambda value: value.update(schema="pulp.other.verification.v1"),
            lambda value: value["canonical_schemas"]["health_result"].update(
                path=str(SCHEMA_PATH)
            ),
            lambda value: value["verifier"]["dependencies"].reverse(),
            lambda value: value.update(unverified_input="forbidden"),
        )
        for operation in mutations:
            with self.subTest(operation=operation):
                changed = json.loads(json.dumps(record))
                operation(changed)
                self.assertTrue(json_schema_lite.validate(changed, schema))

    def test_failures_emit_no_partial_verification_record(self) -> None:
        for result in (
            self.fixture.verify("--expected-host-id", "other-host"),
            self.fixture.verify("--max-age-seconds", "60"),
        ):
            with self.subTest(stderr=result.stderr):
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertIn("verification failed", result.stderr)

    def test_machine_pseudonym_uses_the_canonical_domain_separator(self) -> None:
        raw = RAW_MACHINE_ID.encode("utf-8")
        expected = hashlib.sha256(
            b"pulp.gpu-health.machine.v1\0" + raw
        ).hexdigest()
        self.assertEqual(producer.MACHINE_ID_DOMAIN,
                         b"pulp.gpu-health.machine.v1\0")
        self.assertEqual(
            producer.read_stable_machine_id_sha256(
                io.BytesIO(raw + b"\n"), is_tty=False
            ),
            expected,
        )
        self.assertNotEqual(expected, hashlib.sha256(raw).hexdigest())
        self.assertNotEqual(
            expected,
            hashlib.sha256(b"pulp.gpu-health.machine.v2\0" + raw).hexdigest(),
        )

    def test_producer_never_publishes_or_logs_raw_machine_id(self) -> None:
        output = Path(self.temp.name) / "privacy-scan.json"
        result = self.fixture.produce(self.fixture.evidence, CREATED, output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn(RAW_MACHINE_ID, output.read_text())
        self.assertNotIn(RAW_MACHINE_ID, result.stdout)
        self.assertNotIn(RAW_MACHINE_ID, result.stderr)
        self.assertNotIn(RAW_MACHINE_ID, " ".join(result.args))

    def test_raw_machine_id_argv_option_is_rejected(self) -> None:
        output = Path(self.temp.name) / "argv-refused.json"
        result = self.fixture.produce(
            self.fixture.evidence, CREATED, output,
            machine_option=(
                "--stable-machine-id-stdin",
                "--stable-machine-id", "forbidden-argv-value",
            ),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments: --stable-machine-id", result.stderr)
        self.assertFalse(output.exists())

    def test_machine_id_stdin_rejects_empty_multiline_nul_and_oversize(self) -> None:
        cases = (
            ("empty", "\n", "must not be empty"),
            ("multiline", "first\nsecond\n", "multiple lines or trailing data"),
            ("nul", "first\0second\n", "must not contain NUL"),
            ("oversize", "x" * 1025 + "\n", "1024-byte limit"),
            ("unterminated", "one-line", "LF-terminated"),
        )
        for label, machine_input, expected in cases:
            with self.subTest(label=label):
                output = Path(self.temp.name) / f"stdin-{label}.json"
                result = self.fixture.produce(
                    self.fixture.evidence, CREATED, output,
                    machine_input=machine_input,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertFalse(output.exists())

    def test_machine_id_stdin_refuses_interactive_tty_and_invalid_utf8(self) -> None:
        with self.assertRaisesRegex(SystemExit, "non-interactive pipe"):
            producer.read_stable_machine_id_sha256(
                io.BytesIO(b"private\n"), is_tty=True
            )
        with self.assertRaisesRegex(SystemExit, "valid UTF-8"):
            producer.read_stable_machine_id_sha256(
                io.BytesIO(b"\xff\n"), is_tty=False
            )

    def test_mutable_attestation_ref_is_resolved_once(self) -> None:
        valid_revision = self.fixture.attestation
        path = self.fixture.repo / self.fixture.attestation_path
        path.write_text('{"moved":"after-resolution"}\n')
        self.fixture.commit("publish post-resolution substitution")
        moved_revision = self.fixture.head()
        run("git", "branch", "moving-attestation", valid_revision,
            cwd=self.fixture.repo)

        original_git = verifier.git
        moved = False

        def move_after_resolution(
            repo: Path, *args: str, input_bytes: bytes | None = None
        ) -> subprocess.CompletedProcess:
            nonlocal moved
            result = original_git(repo, *args, input_bytes=input_bytes)
            if args == ("rev-parse", "--verify", "moving-attestation^{commit}"):
                run("git", "branch", "-f", "moving-attestation", moved_revision,
                    cwd=self.fixture.repo)
                moved = True
            return result

        arguments = self.fixture.verify_arguments(
            attestation_revision="moving-attestation"
        )
        output = io.StringIO()
        with mock.patch.object(verifier, "git", side_effect=move_after_resolution), \
                contextlib.redirect_stdout(output):
            result = verifier.verify(
                [str(argument) for argument in arguments],
                clock=lambda: verifier.parse_time(NOW, "test clock"),
            )
        self.assertTrue(moved)
        self.assertEqual(result, 0, output.getvalue())
        record = json.loads(output.getvalue())
        self.assertEqual(record["attestation"]["revision"], valid_revision)
        self.assertEqual(
            run("git", "rev-parse", "moving-attestation",
                cwd=self.fixture.repo).stdout.strip(),
            moved_revision,
        )

    def test_cross_host_configuration_backend_device_and_adapter_reuse_fails(self) -> None:
        substitutions = {
            "--expected-host-id": "other-host",
            "--expected-stable-machine-id-sha256": "0" * 64,
            "--expected-configuration": "fallback=true",
            "--expected-adapter-name": "substituted adapter",
            "--expected-backend": "Software",
            "--expected-device": "vendor=0xffff,device=0xffff",
        }
        for option, value in substitutions.items():
            with self.subTest(option=option):
                result = self.fixture.verify(option, value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("does not match verifier policy", result.stderr)

    def test_trusted_machine_pseudonym_substitution_fails(self) -> None:
        trust = json.loads(self.fixture.trust.read_text())
        trust["hosts"][0]["stable_machine_id_sha256"] = "f" * 64
        self.fixture.trust.write_text(json.dumps(trust, sort_keys=True) + "\n")
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pseudonym does not match trusted host policy", result.stderr)

    def test_older_valid_implementation_ancestor_is_not_expected(self) -> None:
        older = run(
            "git", "rev-parse", f"{self.fixture.implementation}^",
            cwd=self.fixture.repo,
        ).stdout.strip()
        result = self.fixture.verify("--expected-implementation-revision", older)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match verifier policy", result.stderr)

    def test_expected_implementation_policy_rejects_movable_or_noncanonical_input(self) -> None:
        run("git", "tag", "implementation-tag", self.fixture.implementation,
            cwd=self.fixture.repo)
        for value in (
            "HEAD", "main", "implementation-tag",
            f"{self.fixture.implementation}^{{commit}}",
            self.fixture.implementation.upper(),
        ):
            with self.subTest(value=value):
                result = self.fixture.verify(
                    "--expected-implementation-revision", value
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("exact lowercase 40-character Git SHA", result.stderr)

    def test_cli_rejects_now_override(self) -> None:
        result = run(
            "python3", VERIFIER,
            *self.fixture.verify_arguments(), "--now", NOW,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments: --now", result.stderr)

    def test_in_process_clock_is_injected_once(self) -> None:
        calls = 0

        def clock():
            nonlocal calls
            calls += 1
            return verifier.parse_time(NOW, "test clock")

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = verifier.verify(
                [str(argument) for argument in self.fixture.verify_arguments()],
                clock=clock,
            )
        self.assertEqual(status, 0, output.getvalue())
        self.assertEqual(calls, 1)

    def test_alternate_passing_probe_with_same_adapter_is_not_selected(self) -> None:
        result = self.fixture.verify("--expected-probe-id", "renderer3d-frame")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match verifier policy", result.stderr)

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
        self.assertIn("result digest", result.stderr)

    def test_stale_run_fails(self) -> None:
        result = self.fixture.verify("--max-age-seconds", "60")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stale", result.stderr)

    def test_unprotected_attestation_revision_fails(self) -> None:
        run("git", "branch", "-f", "protected", self.fixture.evidence,
            cwd=self.fixture.repo)
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not protected ancestry", result.stderr)

    def test_binary_substitution_fails(self) -> None:
        self.fixture.binary.write_bytes(b"substituted producer\n")
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("binary digest", result.stderr)

    def test_untrusted_signer_fails(self) -> None:
        other_key = Path(self.temp.name) / "other-key"
        run("ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", other_key)
        public_parts = Path(str(other_key) + ".pub").read_text().split(" ", 2)
        trust = json.loads(self.fixture.trust.read_text())
        trust["hosts"][0]["public_key"] = " ".join(public_parts[:2])
        self.fixture.trust.write_text(json.dumps(trust, sort_keys=True) + "\n")
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fingerprint does not match", result.stderr)

    def test_non_ed25519_and_malformed_trusted_keys_fail_closed(self) -> None:
        rsa_key = Path(self.temp.name) / "rsa-key"
        run("ssh-keygen", "-q", "-t", "rsa", "-b", "2048", "-N", "",
            "-f", rsa_key)
        refused = self.fixture.produce(
            self.fixture.evidence, CREATED,
            Path(self.temp.name) / "rsa-producer-refused.json",
            signing_key=rsa_key,
        )
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("not Ed25519", refused.stderr)

        self.fixture.publish_adversarially_signed(
            self.fixture.evidence, signing_key=rsa_key
        )
        rsa_public_key = run("ssh-keygen", "-y", "-f", rsa_key).stdout.strip()
        trust = json.loads(self.fixture.trust.read_text())
        trust["hosts"][0]["public_key"] = rsa_public_key
        self.fixture.trust.write_text(json.dumps(trust, sort_keys=True) + "\n")
        result = self.fixture.verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly ssh-ed25519", result.stderr)

        for label, public_key, expected in (
            ("ecdsa", "ecdsa-sha2-nistp256 AAAA", "exactly ssh-ed25519"),
            ("unsupported", "ssh-dss AAAA", "exactly ssh-ed25519"),
            ("malformed", "ssh-ed25519 !!!!", "malformed"),
        ):
            with self.subTest(label=label):
                trust["hosts"][0]["public_key"] = public_key
                self.fixture.trust.write_text(json.dumps(trust, sort_keys=True) + "\n")
                result = self.fixture.verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)

    def test_health_result_schema_and_semantic_failures_are_rejected_before_signing(self) -> None:
        health_path = self.fixture.repo / self.fixture.health_path
        valid = json.loads(health_path.read_text())
        cases = (
            ("historical-v1", lambda value: (
                value.update(schema="pulp.gpu-health-result.v1", version=1),
                value.pop("measured_at_utc"),
            ), "canonical schema"),
            ("missing-required", lambda value: value.pop("recommendations"),
             "canonical schema"),
            ("wrong-type", lambda value: value.update(render_requested="yes"),
             "canonical schema"),
            ("semantic-inconsistent", lambda value: value.update(
                verdict="fail", health_state="failed"), "semantic contract"),
            ("impossible-measurement-date", lambda value: value.update(
                measured_at_utc="2023-02-29T06:59:00Z"), "semantic contract"),
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
                self.assertIn(expected, verified.stderr)

    def test_impossible_attestation_calendar_values_fail_closed(self) -> None:
        for label, created_at in (
            ("year-zero", "0000-01-01T00:00:00Z"),
            ("century-non-leap", "1900-02-29T07:00:00Z"),
            ("non-leap-day", "2023-02-29T07:00:00Z"),
            ("short-month", "2026-04-31T07:00:00Z"),
            ("hour", "2026-09-01T24:00:00Z"),
            ("leap-second", "2026-09-01T07:00:60Z"),
        ):
            with self.subTest(label=label):
                output = Path(self.temp.name) / f"invalid-{label}.json"
                produced = self.fixture.produce(
                    self.fixture.evidence, created_at, output
                )
                self.assertNotEqual(produced.returncode, 0)
                self.assertIn("created_at", produced.stderr)

                case_root = Path(self.temp.name) / f"signed-{label}"
                case_root.mkdir()
                fixture = Fixture(case_root)

                def set_created_at(value, timestamp=created_at):
                    value["created_at"] = timestamp

                fixture.publish_adversarially_signed(
                    fixture.evidence,
                    operation=set_created_at,
                )
                verified = fixture.verify()
                self.assertNotEqual(verified.returncode, 0)
                self.assertTrue(
                    "created_at" in verified.stderr
                    or "canonical schema" in verified.stderr,
                    verified.stderr,
                )

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
                    "canonical schema" in result.stderr or "malformed" in result.stderr,
                    result.stderr,
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

    def test_verification_schema_missing_or_identity_substitution_fails_closed(self) -> None:
        for label, operation, expected in (
            ("missing", lambda path: path.unlink(), "absent from revision"),
            ("identity", lambda path: path.write_text(json.dumps({
                **json.loads(path.read_text()),
                "$id": "https://example.invalid/substituted.schema.json",
            }, sort_keys=True) + "\n"), "wrong identity"),
            ("unsupported", lambda path: path.write_text(json.dumps({
                **json.loads(path.read_text()),
                "dependentRequired": {},
            }, sort_keys=True) + "\n"), "not implemented"),
        ):
            with self.subTest(label=label):
                case_root = Path(self.temp.name) / f"verification-schema-{label}"
                case_root.mkdir()
                fixture = Fixture(case_root)
                operation(fixture.repo / VERIFICATION_SCHEMA_PATH)
                fixture.commit(f"publish {label} verification schema")
                evidence = fixture.head()
                fixture.publish_adversarially_signed(evidence)
                result = fixture.verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
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
        self.assertIn("GPU measurement is stale", verified.stderr)

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
                verified = fixture.verify(now=now)
                self.assertNotEqual(verified.returncode, 0)
                self.assertIn(expected, verified.stderr)


if __name__ == "__main__":
    unittest.main()
