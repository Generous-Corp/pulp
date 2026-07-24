#!/usr/bin/env python3
"""Mint a short-lived GitHub App JWT without exposing the private key."""

from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


class JwtError(RuntimeError):
    pass


def _b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


def mint_jwt(*, app_id: int, private_key: str, now: int | None = None) -> str:
    if app_id <= 0:
        raise JwtError("GitHub App ID must be a positive integer")
    if "PRIVATE KEY" not in private_key:
        raise JwtError("GitHub App private key is not PEM encoded")
    issued_at = int(time.time() if now is None else now) - 60
    expires_at = issued_at + 600
    header = _b64url(
        json.dumps(
            {"alg": "RS256", "typ": "JWT"}, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")
    )
    payload = _b64url(
        json.dumps(
            {"exp": expires_at, "iat": issued_at, "iss": str(app_id)},
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    )
    signing_input = f"{header}.{payload}".encode("ascii")
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as key_file:
            os.chmod(key_file.name, 0o600)
            key_file.write(private_key)
            key_file.flush()
            result = subprocess.run(
                ["openssl", "dgst", "-sha256", "-sign", key_file.name],
                input=signing_input,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
    except OSError as error:
        raise JwtError(f"cannot invoke OpenSSL: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise JwtError(f"cannot sign GitHub App JWT: {detail}")
    return f"{header}.{payload}.{_b64url(result.stdout)}"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-id", required=True, type=int)
    parser.add_argument(
        "--private-key-env", default="GITHUB_APP_PRIVATE_KEY",
        help="environment variable containing the PEM private key",
    )
    parser.add_argument("--github-output", type=Path, required=True)
    parser.add_argument("--output-name", default="app_jwt")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        private_key = os.environ.get(args.private_key_env)
        if not private_key:
            raise JwtError(f"{args.private_key_env} is required")
        jwt = mint_jwt(app_id=args.app_id, private_key=private_key)
        if "\n" in args.output_name or "=" in args.output_name:
            raise JwtError("output name is invalid")
        print(f"::add-mask::{jwt}")
        with args.github_output.open("a", encoding="utf-8") as output:
            output.write(f"{args.output_name}={jwt}\n")
        return 0
    except (JwtError, OSError) as error:
        print(f"github-app-jwt: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
