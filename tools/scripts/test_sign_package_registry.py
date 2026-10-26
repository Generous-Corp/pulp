#!/usr/bin/env python3
"""Tests for sign_package_registry.py: RFC 8032 vectors, a sign/verify round
trip over registry bytes, and refusal of a tampered registry."""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sign_package_registry as spr  # noqa: E402

# RFC 8032 §7.1 test vectors 1 and 2.
RFC_VECTORS = [
    (
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    ),
    (
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    ),
]

# Seed bytes 1..32, the keypair the C++ registry tests derive with
# pulp::runtime::ed25519_keypair_from_seed.
SHARED_TEST_SEED = bytes(range(1, 33))
REGISTRY = b'{"registry_version": 2, "packages": {}}\n'


class Ed25519ReferenceTests(unittest.TestCase):
    def test_rfc8032_vectors(self) -> None:
        for seed_hex, public_hex, message_hex, signature_hex in RFC_VECTORS:
            seed = bytes.fromhex(seed_hex)
            message = bytes.fromhex(message_hex)
            self.assertEqual(spr.public_key(seed).hex(), public_hex)
            self.assertEqual(spr.ed25519_sign(seed, message).hex(), signature_hex)
            self.assertTrue(spr.ed25519_verify(bytes.fromhex(public_hex), message, bytes.fromhex(signature_hex)))
            self.assertFalse(spr.ed25519_verify(bytes.fromhex(public_hex), message + b"x", bytes.fromhex(signature_hex)))


class RegistrySignatureTests(unittest.TestCase):
    def test_sign_then_verify_round_trip(self) -> None:
        text = spr.sign_registry(SHARED_TEST_SEED, REGISTRY)
        self.assertEqual(text.splitlines()[0], spr.SCHEMA)
        self.assertTrue(spr.verify_registry(spr.public_key(SHARED_TEST_SEED), REGISTRY, text))

    def test_tampered_registry_byte_is_refused(self) -> None:
        text = spr.sign_registry(SHARED_TEST_SEED, REGISTRY)
        tampered = REGISTRY.replace(b"2", b"3", 1)
        self.assertNotEqual(tampered, REGISTRY)
        self.assertFalse(spr.verify_registry(spr.public_key(SHARED_TEST_SEED), tampered, text))

    def test_wrong_key_and_malformed_file_are_refused(self) -> None:
        text = spr.sign_registry(SHARED_TEST_SEED, REGISTRY)
        other = spr.public_key(bytes(range(2, 34)))
        self.assertFalse(spr.verify_registry(other, REGISTRY, text))
        self.assertFalse(spr.verify_registry(spr.public_key(SHARED_TEST_SEED), REGISTRY, "garbage\n"))
        self.assertFalse(spr.verify_registry(spr.public_key(SHARED_TEST_SEED), REGISTRY, ""))

    def test_cli_sign_and_verify(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            seed = root / "seed"
            self.assertEqual(spr.main(["keygen", "--out", str(seed)]), 0)
            self.assertEqual(spr.main(["keygen", "--out", str(seed)]), 1)  # never overwrite
            registry = root / "registry.json"
            registry.write_bytes(REGISTRY)
            self.assertEqual(spr.main(["sign", "--key", str(seed), str(registry)]), 0)
            sig = root / "registry.json.sig"
            self.assertTrue(sig.is_file())
            public = spr.public_key(bytes.fromhex(seed.read_text(encoding="ascii").strip())).hex()
            self.assertEqual(spr.main(["verify", "--public-key", public, str(registry), str(sig)]), 0)
            registry.write_bytes(REGISTRY + b" ")
            self.assertEqual(spr.main(["verify", "--public-key", public, str(registry), str(sig)]), 1)


if __name__ == "__main__":
    unittest.main()
