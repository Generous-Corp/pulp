#!/usr/bin/env python3
"""Tests for github_app_jwt.py."""

from __future__ import annotations

import base64
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("github_app_jwt.py")
SPEC = importlib.util.spec_from_file_location("github_app_jwt", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
jwt_tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(jwt_tool)


def decode_part(value: str) -> dict[str, object]:
    value += "=" * (-len(value) % 4)
    return json.loads(base64.urlsafe_b64decode(value))


class GitHubAppJwtTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        cls.private_key = Path(cls.temporary.name) / "private.pem"
        cls.public_key = Path(cls.temporary.name) / "public.pem"
        subprocess.run(
            [
                "openssl", "genpkey", "-algorithm", "RSA",
                "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(cls.private_key),
            ],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        subprocess.run(
            [
                "openssl", "pkey", "-in", str(cls.private_key),
                "-pubout", "-out", str(cls.public_key),
            ],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_claims_and_rs256_signature_are_exact(self) -> None:
        token = jwt_tool.mint_jwt(
            app_id=3878000,
            private_key=self.private_key.read_text(encoding="utf-8"),
            now=1_800_000_000,
        )
        header, payload, signature = token.split(".")
        self.assertEqual(decode_part(header), {"alg": "RS256", "typ": "JWT"})
        self.assertEqual(
            decode_part(payload),
            {"exp": 1_800_000_540, "iat": 1_799_999_940, "iss": "3878000"},
        )
        signature += "=" * (-len(signature) % 4)
        with tempfile.NamedTemporaryFile() as signature_file:
            signature_file.write(base64.urlsafe_b64decode(signature))
            signature_file.flush()
            result = subprocess.run(
                [
                    "openssl", "dgst", "-sha256", "-verify", str(self.public_key),
                    "-signature", signature_file.name,
                ],
                input=f"{header}.{payload}".encode("ascii"),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"Verified OK", result.stdout)

    def test_invalid_id_and_key_fail_closed(self) -> None:
        with self.assertRaisesRegex(jwt_tool.JwtError, "positive"):
            jwt_tool.mint_jwt(app_id=0, private_key="PRIVATE KEY", now=1)
        with self.assertRaisesRegex(jwt_tool.JwtError, "PEM"):
            jwt_tool.mint_jwt(app_id=1, private_key="not-a-key", now=1)


if __name__ == "__main__":
    unittest.main()
