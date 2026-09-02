#!/usr/bin/env python3
"""Emit or verify the embedded build identity used by A3 role campaigns."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import tempfile
from pathlib import Path
from typing import Any

MARKER_SCHEMA = "pulp.gpu-first-visible-embedded-build.v1"
REQUEST_SCHEMA = "pulp.gpu-first-visible-build-verification-request.v1"
RECEIPT_SCHEMA = "pulp.gpu-first-visible-build-verification-receipt.v1"
MARKER_PREFIX = b"\0PULP_A3_BUILD_IDENTITY_V1:"
MARKER_SUFFIX = b":END_PULP_A3_BUILD_IDENTITY\0"
IDENTITY_KEYS = {
    "pulp_revision", "forge_revision", "build_id", "product_id",
    "product_name", "plugin_format",
}
REQUEST_KEYS = {
    "schema", "version", "attempt_nonce", "control", "product_identity",
    "product_path", "product_sha256", "bundle_tree_sha256",
}
RECEIPT_KEYS = {
    "schema", "version", "attempt_nonce", "control", "outcome", "reason",
    "verification_method", "product_identity", "product_sha256",
    "observed_product_sha256", "marker_sha256",
}
SHA256 = re.compile(r"^[0-9a-f]{64}$")
GIT_REVISION = re.compile(r"^[0-9a-f]{40}$")
MAX_MARKER_BYTES = 16 * 1024


class VerificationError(ValueError):
    """The executable does not carry the requested build identity."""


def exact_keys(value: Any, expected: set[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        raise VerificationError(f"{label} has the wrong fields")


def regular_file_bytes(path: Path, label: str) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise VerificationError(f"{label} is not a readable regular file") from error
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise VerificationError(f"{label} is not a regular file")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                return b"".join(chunks)
            chunks.append(chunk)
    finally:
        os.close(descriptor)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def atomic_bytes(path: Path, data: bytes, mode: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("wb", dir=path.parent, delete=False) as handle:
        handle.write(data)
        temporary = Path(handle.name)
    temporary.chmod(mode)
    os.replace(temporary, path)


def atomic_json(path: Path, value: dict[str, Any]) -> None:
    atomic_bytes(
        path,
        (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8"),
        0o600,
    )


def validate_product_identity(value: Any) -> dict[str, Any]:
    exact_keys(value, IDENTITY_KEYS, "product identity")
    assert isinstance(value, dict)
    if (
        not isinstance(value["pulp_revision"], str)
        or GIT_REVISION.fullmatch(value["pulp_revision"]) is None
        or (
            value["forge_revision"] is not None
            and (
                not isinstance(value["forge_revision"], str)
                or GIT_REVISION.fullmatch(value["forge_revision"]) is None
            )
        )
        or any(
            not isinstance(value[field], str)
            or not value[field]
            or len(value[field]) > 256
            for field in ("build_id", "product_id", "product_name", "plugin_format")
        )
    ):
        raise VerificationError("product identity is invalid")
    return value


def marker_payload(identity: dict[str, Any]) -> bytes:
    validate_product_identity(identity)
    payload = json.dumps(
        {"schema": MARKER_SCHEMA, "version": 1, "product_identity": identity},
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    if len(payload) > MAX_MARKER_BYTES:
        raise VerificationError("embedded build identity is too large")
    return payload


def encode_marker(identity: dict[str, Any]) -> bytes:
    payload = marker_payload(identity)
    return (
        MARKER_PREFIX + f"{len(payload):08x}".encode("ascii") + b":"
        + payload + MARKER_SUFFIX
    )


def extract_marker(data: bytes) -> tuple[dict[str, Any], str]:
    starts: list[int] = []
    offset = 0
    while True:
        found = data.find(MARKER_PREFIX, offset)
        if found < 0:
            break
        starts.append(found)
        offset = found + 1
    if len(starts) != 1:
        raise VerificationError("product must contain exactly one embedded build identity")
    cursor = starts[0] + len(MARKER_PREFIX)
    length_text = data[cursor:cursor + 8]
    cursor += 8
    if len(length_text) != 8 or data[cursor:cursor + 1] != b":":
        raise VerificationError("embedded build identity length is malformed")
    try:
        length = int(length_text.decode("ascii"), 16)
    except (UnicodeDecodeError, ValueError) as error:
        raise VerificationError("embedded build identity length is malformed") from error
    if length > MAX_MARKER_BYTES:
        raise VerificationError("embedded build identity is too large")
    cursor += 1
    payload = data[cursor:cursor + length]
    if len(payload) != length or data[cursor + length:cursor + length + len(MARKER_SUFFIX)] != MARKER_SUFFIX:
        raise VerificationError("embedded build identity is truncated")
    try:
        document = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise VerificationError("embedded build identity is not valid JSON") from error
    exact_keys(document, {"schema", "version", "product_identity"}, "embedded build identity")
    if document["schema"] != MARKER_SCHEMA or document["version"] != 1:
        raise VerificationError("embedded build identity has the wrong protocol")
    validate_product_identity(document["product_identity"])
    if payload != marker_payload(document["product_identity"]):
        raise VerificationError("embedded build identity is not canonical")
    return document["product_identity"], sha256_bytes(payload)


def verification_receipt(
    request: dict[str, Any], *, outcome: str, reason: str | None,
    observed_digest: str | None, marker_digest: str | None,
) -> dict[str, Any]:
    return {
        "schema": RECEIPT_SCHEMA,
        "version": 1,
        "attempt_nonce": request.get("attempt_nonce"),
        "control": request.get("control"),
        "outcome": outcome,
        "reason": reason,
        "verification_method": "embedded-canonical-build-identity",
        "product_identity": request.get("product_identity"),
        "product_sha256": request.get("product_sha256"),
        "observed_product_sha256": observed_digest,
        "marker_sha256": marker_digest,
    }


def verify(request_path: Path, receipt_path: Path) -> int:
    request: dict[str, Any] = {}
    observed_digest: str | None = None
    marker_digest: str | None = None
    try:
        request = json.loads(regular_file_bytes(request_path, "verification request"))
        exact_keys(request, REQUEST_KEYS, "verification request")
        if (
            request["schema"] != REQUEST_SCHEMA
            or request["version"] != 1
            or request["control"] not in {"real", "tampered-product", "wrong-source"}
            or not isinstance(request["attempt_nonce"], str)
            or not request["attempt_nonce"]
            or not isinstance(request["product_path"], str)
            or not Path(request["product_path"]).is_absolute()
            or not isinstance(request["product_sha256"], str)
            or SHA256.fullmatch(request["product_sha256"]) is None
            or (
                request["bundle_tree_sha256"] is not None
                and (
                    not isinstance(request["bundle_tree_sha256"], str)
                    or SHA256.fullmatch(request["bundle_tree_sha256"]) is None
                )
            )
        ):
            raise VerificationError("verification request has an invalid protocol identity")
        identity = validate_product_identity(request["product_identity"])
        product = regular_file_bytes(Path(request["product_path"]), "product executable")
        observed_digest = sha256_bytes(product)
        if observed_digest != request["product_sha256"]:
            raise VerificationError("product executable digest differs from the closed request")
        embedded_identity, marker_digest = extract_marker(product)
        if embedded_identity != identity:
            raise VerificationError("embedded build identity differs from the closed request")
        atomic_json(receipt_path, verification_receipt(
            request, outcome="pass", reason=None,
            observed_digest=observed_digest, marker_digest=marker_digest,
        ))
        return 0
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, VerificationError) as error:
        atomic_json(receipt_path, verification_receipt(
            request, outcome="fail", reason=str(error),
            observed_digest=observed_digest, marker_digest=marker_digest,
        ))
        return 1


def emit(identity_path: Path, output_path: Path) -> int:
    document = json.loads(regular_file_bytes(identity_path, "build identity input"))
    identity = document.get("product_identity") if isinstance(document, dict) and "product_identity" in document else document
    marker = encode_marker(validate_product_identity(identity))
    atomic_bytes(output_path, marker, 0o444)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--request", required=True, type=Path)
    verify_parser.add_argument("--receipt", required=True, type=Path)
    emit_parser = subparsers.add_parser("emit")
    emit_parser.add_argument("--identity", required=True, type=Path)
    emit_parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "verify":
        return verify(args.request.resolve(), args.receipt.resolve())
    return emit(args.identity.resolve(), args.output.resolve())


if __name__ == "__main__":
    raise SystemExit(main())
