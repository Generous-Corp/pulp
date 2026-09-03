#!/usr/bin/env python3
"""Independently verify a protected GPU-health run attestation."""

from __future__ import annotations

import argparse
import base64
import binascii
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import sys
from collections.abc import Callable, Sequence
from typing import Any

import json_schema_lite
from gpu_health_contract import parse_utc_timestamp, semantic_errors

SCHEMA = "pulp.gpu-health-run-attestation.v1"
NAMESPACE = SCHEMA
SCHEMA_PATH = "docs/contracts/gpu-health-run-attestation-v1.schema.json"
HEALTH_SCHEMA_PATH = "docs/contracts/gpu-health-result-v2.schema.json"
VERIFICATION_SCHEMA = "pulp.gpu-health-run-attestation-verification.v1"
VERIFICATION_SCHEMA_PATH = \
    "docs/contracts/gpu-health-run-attestation-verification-v1.schema.json"
VERIFIER_CONTRACT_VERSION = 1
VERIFIER_DEPENDENCIES = ("gpu_health_contract.py", "json_schema_lite.py")


class Failure(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def canonical(value: object) -> bytes:
    return (json.dumps(value, allow_nan=False, ensure_ascii=False,
                       separators=(",", ":"), sort_keys=True) + "\n").encode()


def strict_json(data: bytes, label: str) -> dict[str, Any]:
    def pairs(values: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in values:
            require(key not in result, f"{label} contains duplicate key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(data, object_pairs_hook=pairs,
                           parse_constant=lambda constant: (_ for _ in ()).throw(
                               Failure(f"{label} contains non-finite value {constant}")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise Failure(f"{label} is not strict JSON: {error}") from error
    require(isinstance(value, dict), f"{label} must be an object")
    return value


def exact(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{label} must be an object")
    require(set(value) == keys, f"{label} has missing or unknown members")
    return value


def git(repo: Path, *args: str, input_bytes: bytes | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *args], input=input_bytes,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def git_blob(repo: Path, revision: str, path: str) -> bytes:
    result = git(repo, "show", f"{revision}:{path}")
    require(result.returncode == 0, f"{path} is absent from revision {revision}")
    return result.stdout


def git_blob_oid(repo: Path, revision: str, path: str) -> str:
    result = git(repo, "rev-parse", "--verify", f"{revision}:{path}")
    require(result.returncode == 0, f"{path} has no blob identity at revision {revision}")
    value = result.stdout.decode().strip()
    require(len(value) == 40 and all(c in "0123456789abcdef" for c in value),
            f"{path} has a malformed blob identity at revision {revision}")
    return value


def local_artifact(path: Path, label: str) -> dict[str, str]:
    resolved = path.resolve()
    try:
        digest = hashlib.sha256(resolved.read_bytes()).hexdigest()
    except OSError as error:
        raise Failure(f"{label} cannot be read: {error}") from error
    return {"path": str(resolved), "sha256": digest}


def read_descriptor(descriptor: int, size: int) -> bytes:
    return os.read(descriptor, size)


def sha256_opened_regular_file(path: Path, label: str) -> str:
    """Hash one unchanged regular-file inode without following a symlink."""
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise Failure(f"{label} cannot be securely opened: {error}") from error
    try:
        before = os.fstat(descriptor)
        require(stat.S_ISREG(before.st_mode), f"{label} must be a regular file")
        try:
            path_before = os.stat(path, follow_symlinks=False)
        except OSError as error:
            raise Failure(f"{label} path changed after opening: {error}") from error
        require(not stat.S_ISLNK(path_before.st_mode)
                and (path_before.st_dev, path_before.st_ino) ==
                    (before.st_dev, before.st_ino),
                f"{label} path changed or traverses a symlink")

        digest = hashlib.sha256()
        while True:
            chunk = read_descriptor(descriptor, 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)

        after = os.fstat(descriptor)
        try:
            path_after = os.stat(path, follow_symlinks=False)
        except OSError as error:
            raise Failure(f"{label} path changed while being read: {error}") from error
        stable_fields = ("st_dev", "st_ino", "st_mode", "st_size",
                         "st_mtime_ns", "st_ctime_ns")
        require(all(getattr(before, field) == getattr(after, field)
                    for field in stable_fields)
                and not stat.S_ISLNK(path_after.st_mode)
                and (path_after.st_dev, path_after.st_ino) ==
                    (after.st_dev, after.st_ino),
                f"{label} changed while being read")
        return digest.hexdigest()
    finally:
        os.close(descriptor)


def resolve_commit(repo: Path, revision: str, label: str) -> str:
    result = git(repo, "rev-parse", "--verify", f"{revision}^{{commit}}")
    require(result.returncode == 0, f"{label} does not resolve to a commit")
    resolved = result.stdout.decode().strip()
    require(len(resolved) == 40 and all(c in "0123456789abcdef" for c in resolved),
            f"{label} did not resolve to an exact lowercase Git revision")
    return resolved


def ancestor(repo: Path, older: str, newer: str, label: str) -> None:
    result = git(repo, "merge-base", "--is-ancestor", older, newer)
    require(result.returncode == 0, label)


def key_fingerprint(public_key: str) -> str:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8") as key_file:
        key_file.write(public_key + "\n")
        key_file.flush()
        result = subprocess.run(
            ["ssh-keygen", "-lf", key_file.name, "-E", "sha256"], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
    require(result.returncode == 0 and "SHA256:" in result.stdout,
            "trusted host public key is not a readable SSH key")
    return "SHA256:" + result.stdout.split("SHA256:", 1)[1].split()[0]


def parse_ed25519_public_key(value: Any) -> str:
    require(isinstance(value, str) and "\n" not in value and "\r" not in value,
            "trusted host public key must be one ssh-ed25519 key line")
    parts = value.split(" ")
    require(len(parts) == 2 and parts[0] == "ssh-ed25519" and bool(parts[1]),
            "trusted host public key must be exactly ssh-ed25519")
    try:
        blob = base64.b64decode(parts[1], validate=True)
    except (ValueError, binascii.Error) as error:
        raise Failure("trusted host Ed25519 public key is malformed") from error
    prefix = b"\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20"
    require(len(blob) == len(prefix) + 32 and blob.startswith(prefix),
            "trusted host Ed25519 public key is malformed")
    return value


def parse_time(value: str, label: str) -> dt.datetime:
    try:
        return parse_utc_timestamp(value, label)
    except ValueError as error:
        raise Failure(str(error)) from error


def verify_signature(statement: dict[str, Any], public_key: str,
                     identity: str, signature: bytes) -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        signature_path = Path(temp_dir) / "statement.sig"
        signature_path.write_bytes(signature)
        # ssh-keygen requires an allowed-signers pathname, but the signer
        # identity and public key are trust inputs and must not be persisted as
        # clear-text storage.  Feed the file through an inherited pipe instead.
        allowed_read, allowed_write = os.pipe()
        try:
            os.write(allowed_write, f"{identity} {public_key}\n".encode("utf-8"))
        finally:
            os.close(allowed_write)
        try:
            result = subprocess.run(
                ["ssh-keygen", "-Y", "verify", "-f", f"/dev/fd/{allowed_read}",
                 "-I", identity, "-n", NAMESPACE, "-s", str(signature_path)],
                input=canonical(statement), stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=False, pass_fds=(allowed_read,),
            )
        finally:
            os.close(allowed_read)
    require(result.returncode == 0, "run-attestation signature is not valid for the trusted host key")


def validate_shape(attestation: dict[str, Any]) -> None:
    exact(attestation, {"schema", "version", "created_at", "implementation_revision",
                        "evidence_publication_revision", "host", "selection", "producer",
                        "gpu_health_result", "canonical_health_schema",
                        "canonical_schema", "authentication"},
          "run attestation")
    require(attestation["schema"] == SCHEMA and attestation["version"] == 1,
            "run attestation has the wrong contract identity")
    exact(attestation["host"], {"host_id", "stable_machine_id_sha256"}, "host")
    exact(attestation["selection"], {"configuration", "probe_id", "adapter_name",
                                     "backend", "device"}, "selection")
    exact(attestation["producer"], {"binary_path", "binary_sha256"}, "producer")
    exact(attestation["gpu_health_result"],
          {"path", "sha256", "run_id", "schema", "measured_at_utc"},
          "gpu_health_result")
    exact(attestation["canonical_health_schema"], {"path", "sha256"},
          "canonical_health_schema")
    exact(attestation["canonical_schema"], {"path", "sha256"}, "canonical_schema")
    exact(attestation["authentication"], {"algorithm", "namespace",
                                           "signer_key_fingerprint", "signature"},
          "authentication")
    require(attestation["authentication"]["algorithm"] == "ssh-ed25519"
            and attestation["authentication"]["namespace"] == NAMESPACE,
            "run attestation uses an unsupported authentication method")
    require(attestation["canonical_schema"]["path"] == SCHEMA_PATH,
            "run attestation does not bind the canonical schema path")
    require(attestation["canonical_health_schema"]["path"] == HEALTH_SCHEMA_PATH,
            "run attestation does not bind the canonical GPU-health schema path")
    for field in ("implementation_revision", "evidence_publication_revision"):
        value = attestation[field]
        require(isinstance(value, str) and len(value) == 40
                and all(c in "0123456789abcdef" for c in value),
                f"{field} is not an exact lowercase Git revision")
    for parent, fields in ((attestation["host"], ("host_id",)),
                           (attestation["selection"], ("configuration", "probe_id",
                                                        "adapter_name", "backend", "device")),
                           (attestation["producer"], ("binary_path",))):
        require(all(isinstance(parent[field], str) and parent[field] for field in fields),
                "run attestation contains an empty identity field")
    for parent, field in ((attestation["host"], "stable_machine_id_sha256"),
                          (attestation["producer"], "binary_sha256"),
                          (attestation["gpu_health_result"], "sha256"),
                          (attestation["canonical_health_schema"], "sha256"),
                          (attestation["canonical_schema"], "sha256")):
        value = parent[field]
        require(isinstance(value, str) and len(value) == 64
                and all(c in "0123456789abcdef" for c in value),
                f"{field} is not a lowercase SHA-256 digest")
    require(all(isinstance(attestation["gpu_health_result"][field], str)
                and attestation["gpu_health_result"][field]
                for field in ("path", "run_id", "schema", "measured_at_utc")),
            "GPU-health result reference contains an empty field")
    require(isinstance(attestation["authentication"]["signer_key_fingerprint"], str)
            and attestation["authentication"]["signer_key_fingerprint"].startswith("SHA256:")
            and isinstance(attestation["authentication"]["signature"], str)
            and attestation["authentication"]["signature"],
            "authentication identity or signature is malformed")


def verify(argv: Sequence[str], *, clock: Callable[[], dt.datetime]) -> int:
    """Verify one invocation using an in-process-only injectable UTC clock."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--attestation-revision", required=True)
    parser.add_argument("--attestation-path", required=True)
    parser.add_argument("--protected-ref", required=True)
    parser.add_argument("--trusted-hosts", type=Path, required=True)
    parser.add_argument("--producer-binary", type=Path, required=True)
    parser.add_argument("--expected-producer-binary-path", required=True)
    parser.add_argument("--expected-implementation-revision", required=True)
    parser.add_argument("--expected-host-id", required=True)
    parser.add_argument("--expected-stable-machine-id-sha256", required=True)
    parser.add_argument("--expected-configuration", required=True)
    parser.add_argument("--expected-probe-id", required=True)
    parser.add_argument("--expected-adapter-name", required=True)
    parser.add_argument("--expected-backend", required=True)
    parser.add_argument("--expected-device", required=True)
    parser.add_argument("--max-age-seconds", type=int, required=True)
    args = parser.parse_args(argv)
    try:
        repo = args.repository.resolve()
        expected_implementation = args.expected_implementation_revision
        require(isinstance(expected_implementation, str)
                and len(expected_implementation) == 40
                and all(c in "0123456789abcdef" for c in expected_implementation),
                "expected implementation revision must be an exact lowercase "
                "40-character Git SHA")
        expected_machine_id = args.expected_stable_machine_id_sha256
        require(isinstance(expected_machine_id, str)
                and len(expected_machine_id) == 64
                and all(c in "0123456789abcdef" for c in expected_machine_id),
                "expected stable machine pseudonym must be an exact lowercase SHA-256")
        protected_revision = resolve_commit(repo, args.protected_ref, "protected ref")
        attestation_revision = resolve_commit(
            repo, args.attestation_revision, "attestation revision"
        )
        ancestor(repo, attestation_revision, protected_revision,
                 "attestation revision is not protected ancestry")
        attestation_bytes = git_blob(repo, attestation_revision, args.attestation_path)
        attestation_blob = git_blob_oid(
            repo, attestation_revision, args.attestation_path
        )
        attestation = strict_json(attestation_bytes, "run attestation")
        validate_shape(attestation)
        expected = {
            "implementation_revision": expected_implementation,
            "host_id": args.expected_host_id,
            "stable_machine_id_sha256": expected_machine_id,
            "configuration": args.expected_configuration,
            "probe_id": args.expected_probe_id,
            "adapter_name": args.expected_adapter_name,
            "backend": args.expected_backend,
            "device": args.expected_device,
        }
        observed = {
            "implementation_revision": attestation["implementation_revision"],
            **attestation["host"],
            **{
                key: attestation["selection"][key]
                for key in (
                    "configuration", "probe_id", "adapter_name", "backend", "device"
                )
            },
        }
        require(observed == expected,
                "implementation/host/configuration/probe/adapter selection does "
                "not match verifier policy")
        implementation = observed["implementation_revision"]
        evidence = attestation["evidence_publication_revision"]
        ancestor(repo, implementation, evidence,
                 "implementation revision is not evidence-publication ancestry")
        ancestor(repo, evidence, attestation_revision,
                 "evidence publication is not attestation-publication ancestry")
        require(evidence != attestation_revision,
                "evidence publication must precede the containing attestation revision")
        ancestor(repo, evidence, protected_revision,
                 "evidence publication revision is not protected ancestry")

        schema_bytes = git_blob(repo, evidence, SCHEMA_PATH)
        health_schema_bytes = git_blob(repo, evidence, HEALTH_SCHEMA_PATH)
        verification_schema_bytes = git_blob(
            repo, evidence, VERIFICATION_SCHEMA_PATH
        )
        require(hashlib.sha256(schema_bytes).hexdigest() == attestation["canonical_schema"]["sha256"],
                "canonical schema digest does not match protected evidence")
        require(hashlib.sha256(health_schema_bytes).hexdigest() ==
                attestation["canonical_health_schema"]["sha256"],
                "canonical GPU-health schema digest does not match protected evidence")
        schema = strict_json(schema_bytes, "canonical schema")
        require(schema.get("$id") ==
                "https://pulp.audio/contracts/gpu-health-run-attestation-v1.schema.json",
                "canonical schema has the wrong identity")
        schema_problems = json_schema_lite.validate(attestation, schema)
        require(not schema_problems,
                f"run attestation violates its canonical schema: {schema_problems[0] if schema_problems else ''}")
        health_ref = attestation["gpu_health_result"]
        health_bytes = git_blob(repo, evidence, health_ref["path"])
        health_blob = git_blob_oid(repo, evidence, health_ref["path"])
        require(hashlib.sha256(health_bytes).hexdigest() == health_ref["sha256"],
                "GPU-health result digest does not match protected evidence")
        health_schema = strict_json(health_schema_bytes, "canonical GPU-health schema")
        require(health_schema.get("$id") ==
                "https://pulp.audio/contracts/gpu-health-result-v2.schema.json",
                "canonical GPU-health schema has the wrong identity")
        health = strict_json(health_bytes, "GPU-health result")
        health_schema_problems = json_schema_lite.validate(health, health_schema)
        require(not health_schema_problems,
                "GPU-health result violates its canonical schema: " +
                (health_schema_problems[0] if health_schema_problems else ""))
        semantic_problems = semantic_errors(health)
        require(not semantic_problems,
                "GPU-health result violates its semantic contract: " +
                (semantic_problems[0] if semantic_problems else ""))
        require(health.get("schema") == health_ref["schema"] == "pulp.gpu-health-result.v2"
                and health.get("run_id") == health_ref["run_id"]
                and health.get("measured_at_utc") == health_ref["measured_at_utc"],
                "GPU-health result identity, run_id, or measurement time was substituted")
        all_probes = health.get("probes")
        require(isinstance(all_probes, list)
                and all(isinstance(probe, dict) for probe in all_probes),
                "GPU-health probes must be an array of objects")
        probes = [probe for probe in all_probes
                  if probe.get("probe_id") == attestation["selection"]["probe_id"]]
        require(len(probes) == 1, "selected GPU-health probe is missing or ambiguous")
        adapter = probes[0].get("adapter", {})
        require(probes[0].get("required") is True and probes[0].get("verdict") == "pass"
                and adapter.get("status") == "authentic" and adapter.get("class") == "hardware"
                and adapter.get("name") == attestation["selection"]["adapter_name"]
                and adapter.get("backend") == attestation["selection"]["backend"]
                and adapter.get("device") == attestation["selection"]["device"],
                "selected adapter/backend/device does not match the protected GPU-health result")

        signed_binary_path = args.expected_producer_binary_path
        require(isinstance(signed_binary_path, str) and signed_binary_path,
                "expected producer binary path must not be empty")
        require(signed_binary_path == attestation["producer"]["binary_path"],
                "producer binary path does not match verifier policy")
        binary_digest = sha256_opened_regular_file(
            args.producer_binary, "producer binary byte source"
        )
        require(binary_digest == attestation["producer"]["binary_sha256"],
                "producer binary digest does not match the signed run")

        verifier_path = Path(__file__).resolve()
        verifier_entrypoint = local_artifact(verifier_path, "verifier entrypoint")
        verifier_dependencies = [
            local_artifact(verifier_path.with_name(name), f"verifier dependency {name}")
            for name in VERIFIER_DEPENDENCIES
        ]

        trust_path = args.trusted_hosts.resolve()
        trust_bytes = trust_path.read_bytes()
        trust = strict_json(trust_bytes, "trusted-host registry")
        exact(trust, {"schema", "version", "hosts"}, "trusted-host registry")
        require(trust["schema"] == "pulp.gpu-health-trusted-hosts.v1"
                and trust["version"] == 1 and isinstance(trust["hosts"], list),
                "trusted-host registry has the wrong contract identity")
        require(all(isinstance(host, dict) for host in trust["hosts"]),
                "trusted-host entries must be objects")
        matches = [host for host in trust["hosts"]
                   if host.get("host_id") == attestation["host"]["host_id"]]
        require(len(matches) == 1, "signed host_id is not uniquely trusted")
        trusted = exact(matches[0], {"host_id", "stable_machine_id_sha256", "public_key"},
                        "trusted host")
        trusted_machine_id = trusted["stable_machine_id_sha256"]
        require(isinstance(trusted_machine_id, str) and len(trusted_machine_id) == 64
                and all(c in "0123456789abcdef" for c in trusted_machine_id),
                "trusted stable machine pseudonym must be an exact lowercase SHA-256")
        require(trusted_machine_id == attestation["host"]["stable_machine_id_sha256"],
                "stable machine pseudonym does not match trusted host policy")
        public_key = parse_ed25519_public_key(trusted["public_key"])
        fingerprint = key_fingerprint(public_key)
        require(fingerprint == attestation["authentication"]["signer_key_fingerprint"],
                "signer key fingerprint does not match trusted host policy")
        try:
            signature = base64.b64decode(attestation["authentication"]["signature"], validate=True)
        except (ValueError, binascii.Error) as error:
            raise Failure("run-attestation signature is not valid base64") from error
        unsigned = dict(attestation)
        del unsigned["authentication"]
        verify_signature(unsigned, public_key, trusted["host_id"], signature)

        require(args.max_age_seconds >= 0, "max age must be non-negative")
        measured = parse_time(health_ref["measured_at_utc"], "measured_at_utc")
        created = parse_time(attestation["created_at"], "created_at")
        now = clock()
        require(isinstance(now, dt.datetime) and now.tzinfo is not None
                and now.utcoffset() == dt.timedelta(0),
                "injected verification clock must return a UTC datetime")
        require(measured <= created, "run attestation predates the GPU measurement")
        require(created <= now, "run attestation is dated in the future")
        require(measured <= now, "GPU measurement is dated in the future")
        require((now - measured).total_seconds() <= args.max_age_seconds,
                "GPU measurement is stale")
        verified_at = now.isoformat(timespec="seconds").replace("+00:00", "Z")
        record = {
            "schema": VERIFICATION_SCHEMA,
            "version": 1,
            "status": "verified",
            "verified_at_utc": verified_at,
            "max_age_seconds": args.max_age_seconds,
            "implementation_revision": expected_implementation,
            "evidence_revision": evidence,
            "protected_ref_revision": protected_revision,
            "attestation": {
                "revision": attestation_revision,
                "path": args.attestation_path,
                "blob_sha1": attestation_blob,
                "sha256": hashlib.sha256(attestation_bytes).hexdigest(),
            },
            "health_result": {
                "path": health_ref["path"],
                "blob_sha1": health_blob,
                "sha256": health_ref["sha256"],
                "schema": health_ref["schema"],
                "run_id": health_ref["run_id"],
                "measured_at_utc": health_ref["measured_at_utc"],
            },
            "canonical_schemas": {
                "attestation": {
                    "path": SCHEMA_PATH,
                    "blob_sha1": git_blob_oid(repo, evidence, SCHEMA_PATH),
                    "sha256": hashlib.sha256(schema_bytes).hexdigest(),
                },
                "health_result": {
                    "path": HEALTH_SCHEMA_PATH,
                    "blob_sha1": git_blob_oid(repo, evidence, HEALTH_SCHEMA_PATH),
                    "sha256": hashlib.sha256(health_schema_bytes).hexdigest(),
                },
                "verification": {
                    "path": VERIFICATION_SCHEMA_PATH,
                    "blob_sha1": git_blob_oid(
                        repo, evidence, VERIFICATION_SCHEMA_PATH
                    ),
                    "sha256": hashlib.sha256(
                        verification_schema_bytes
                    ).hexdigest(),
                },
            },
            "selection": dict(attestation["selection"]),
            "host": dict(attestation["host"]),
            "trusted_host": {
                "registry_path": str(trust_path),
                "registry_sha256": hashlib.sha256(trust_bytes).hexdigest(),
                "key_type": "ssh-ed25519",
                "matched_key_fingerprint": fingerprint,
            },
            "producer": {
                "binary_path": signed_binary_path,
                "binary_sha256": binary_digest,
            },
            "verifier": {
                "contract_version": VERIFIER_CONTRACT_VERSION,
                "entrypoint": verifier_entrypoint,
                "dependencies": verifier_dependencies,
            },
            "signature": {
                "namespace": NAMESPACE,
                "signer_key_fingerprint": attestation["authentication"]
                                                     ["signer_key_fingerprint"],
                "result": "verified",
            },
            "chronology": {
                "measured_at_utc": health_ref["measured_at_utc"],
                "attestation_created_at": attestation["created_at"],
                "verified_at_utc": verified_at,
                "measurement_to_attestation_seconds": int(
                    (created - measured).total_seconds()
                ),
                "measurement_age_seconds": int((now - measured).total_seconds()),
                "attestation_age_seconds": int((now - created).total_seconds()),
            },
        }
        verification_schema = strict_json(
            verification_schema_bytes, "canonical verification schema"
        )
        require(verification_schema.get("$id") ==
                "https://pulp.audio/contracts/"
                "gpu-health-run-attestation-verification-v1.schema.json",
                "canonical verification schema has the wrong identity")
        record_problems = json_schema_lite.validate(record, verification_schema)
        require(not record_problems,
                "verification record violates its canonical schema: " +
                (record_problems[0] if record_problems else ""))
    except (Failure, OSError, json_schema_lite.UnsupportedKeyword) as error:
        print(f"gpu-health run-attestation verification failed: {error}",
              file=sys.stderr)
        return 1
    sys.stdout.write(canonical(record).decode("utf-8"))
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else argv
    return verify(arguments, clock=lambda: dt.datetime.now(dt.timezone.utc))


if __name__ == "__main__":
    raise SystemExit(main())
