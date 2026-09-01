#!/usr/bin/env python3
"""Produce a host-signed attestation for a published GPU-health result."""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

SCHEMA = "pulp.gpu-health-run-attestation.v1"
NAMESPACE = SCHEMA
SCHEMA_PATH = "docs/contracts/gpu-health-run-attestation-v1.schema.json"


def fail(message: str) -> None:
    raise SystemExit(f"gpu-health attestation: {message}")


def canonical(value: object) -> bytes:
    return (json.dumps(value, allow_nan=False, ensure_ascii=False,
                       separators=(",", ":"), sort_keys=True) + "\n").encode()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def strict_json(data: bytes) -> dict:
    def pairs(values: list[tuple[str, object]]) -> dict:
        result = {}
        for key, value in values:
            if key in result:
                fail(f"GPU-health result contains duplicate key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(
            data, object_pairs_hook=pairs,
            parse_constant=lambda constant: fail(
                f"GPU-health result contains non-finite value {constant}"),
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"GPU-health result is not strict JSON: {error}")
    if not isinstance(value, dict):
        fail("GPU-health result must be an object")
    return value


def git_bytes(repo: Path, revision: str, path: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), "show", f"{revision}:{path}"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        fail(f"{path} is not a blob at evidence publication {revision}")
    return result.stdout


def public_key(signing_key: Path) -> str:
    result = subprocess.run(
        ["ssh-keygen", "-y", "-f", str(signing_key)], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        fail("ssh-keygen could not read the Ed25519 signing key")
    key = result.stdout.strip()
    if not key.startswith("ssh-ed25519 "):
        fail("the signing key is not Ed25519")
    return key


def fingerprint(key: str) -> str:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8") as public:
        public.write(key + "\n")
        public.flush()
        result = subprocess.run(
            ["ssh-keygen", "-lf", public.name, "-E", "sha256"], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
    if result.returncode or "SHA256:" not in result.stdout:
        fail("ssh-keygen could not fingerprint the signing key")
    return "SHA256:" + result.stdout.split("SHA256:", 1)[1].split()[0]


def selected_probe(health: dict, probe_id: str) -> dict:
    all_probes = health.get("probes")
    if not isinstance(all_probes, list) or not all(isinstance(p, dict) for p in all_probes):
        fail("GPU-health result probes must be an array of objects")
    probes = [p for p in all_probes if p.get("probe_id") == probe_id]
    if len(probes) != 1:
        fail(f"GPU-health result must contain exactly one {probe_id!r} probe")
    probe = probes[0]
    adapter = probe.get("adapter", {})
    required = ("name", "backend", "device")
    if (not probe.get("required") or probe.get("verdict") != "pass"
            or adapter.get("status") != "authentic"
            or adapter.get("class") != "hardware"
            or any(not isinstance(adapter.get(field), str) or not adapter[field]
                   for field in required)):
        fail("selected probe is not a passing authentic hardware identity with name/backend/device")
    return probe


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--health-result", required=True,
                        help="repository-relative result path")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--signing-key", type=Path, required=True)
    parser.add_argument("--host-id", required=True)
    parser.add_argument("--stable-machine-id", required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--probe-id", required=True)
    parser.add_argument("--implementation-revision", required=True)
    parser.add_argument("--evidence-publication-revision", required=True)
    parser.add_argument("--producer-binary", type=Path, required=True)
    parser.add_argument("--producer-build-id", required=True)
    parser.add_argument("--producer-code-signature", required=True)
    parser.add_argument("--created-at", help=argparse.SUPPRESS)
    args = parser.parse_args()

    for label, revision in (("implementation", args.implementation_revision),
                            ("evidence publication", args.evidence_publication_revision)):
        if len(revision) != 40 or any(c not in "0123456789abcdef" for c in revision):
            fail(f"{label} revision must be an exact lowercase 40-character Git SHA")
    repo = args.repository.resolve()
    health_bytes = git_bytes(repo, args.evidence_publication_revision, args.health_result)
    schema_bytes = git_bytes(repo, args.evidence_publication_revision, SCHEMA_PATH)
    health = strict_json(health_bytes)
    if health.get("schema") != "pulp.gpu-health-result.v1" or not health.get("run_id"):
        fail("GPU-health result has the wrong schema or an empty run_id")
    probe = selected_probe(health, args.probe_id)
    binary = args.producer_binary.resolve()
    try:
        binary_bytes = binary.read_bytes()
    except OSError as error:
        fail(f"cannot read producer binary: {error}")
    created_at = args.created_at or dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
    statement = {
        "schema": SCHEMA,
        "version": 1,
        "created_at": created_at,
        "implementation_revision": args.implementation_revision,
        "evidence_publication_revision": args.evidence_publication_revision,
        "host": {"host_id": args.host_id, "stable_machine_id": args.stable_machine_id},
        "selection": {
            "configuration": args.configuration,
            "probe_id": args.probe_id,
            "adapter_name": probe["adapter"]["name"],
            "backend": probe["adapter"]["backend"],
            "device": probe["adapter"]["device"],
        },
        "producer": {
            "binary_path": str(binary),
            "binary_sha256": sha256(binary_bytes),
            "build_id": args.producer_build_id,
            "code_signature": args.producer_code_signature,
        },
        "gpu_health_result": {
            "path": args.health_result,
            "sha256": sha256(health_bytes),
            "run_id": health["run_id"],
            "schema": health["schema"],
        },
        "canonical_schema": {"path": SCHEMA_PATH, "sha256": sha256(schema_bytes)},
    }
    key = public_key(args.signing_key)
    with tempfile.TemporaryDirectory() as temp_dir:
        message = Path(temp_dir) / "statement.json"
        message.write_bytes(canonical(statement))
        result = subprocess.run(
            ["ssh-keygen", "-Y", "sign", "-f", str(args.signing_key),
             "-n", NAMESPACE, str(message)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if result.returncode:
            fail("ssh-keygen could not sign the run statement")
        signature = Path(str(message) + ".sig").read_bytes()
    statement["authentication"] = {
        "algorithm": "ssh-ed25519",
        "namespace": NAMESPACE,
        "signer_key_fingerprint": fingerprint(key),
        "signature": base64.b64encode(signature).decode("ascii"),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(canonical(statement))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
