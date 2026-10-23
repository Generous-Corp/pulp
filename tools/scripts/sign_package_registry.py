#!/usr/bin/env python3
"""Sign the remote package registry so `pulp search` can verify it.

The registry owner runs this once per registry change:

    sign_package_registry.py keygen --out ~/.config/pulp/secrets/package-registry.seed
    sign_package_registry.py sign --key ~/.config/pulp/secrets/package-registry.seed registry.json
    sign_package_registry.py verify --public-key <hex> registry.json registry.json.sig

`keygen` prints the public key to pin as `kPinnedRegistrySignerPublicKeyHex`
in tools/cli/package_registry_trust.hpp. `sign` writes `registry.json.sig`
beside the registry; publish both at the same URL prefix. The CLI fetches
`<registry-url>.sig`, recomputes the signed message from the registry bytes
it received, and refuses the registry unless the signature verifies.

Signed message: "pulp-package-registry-sig-v1\\n" + sha256_hex(bytes) + "\\n".
Signature file:  the schema line, then the 64-byte signature as hex.

Ed25519 is implemented here from RFC 8032 (reference arithmetic, pure
Python) so the owner needs nothing beyond the interpreter. It is slow by
design-free-of-dependencies standards (well under a second per signature)
and never used on the user side; the CLI verifies with pulp::runtime.
"""

from __future__ import annotations

import argparse
import hashlib
import secrets
import stat
import sys
from pathlib import Path

SCHEMA = "pulp-package-registry-sig-v1"

# ── Ed25519 (RFC 8032 §5.1 reference arithmetic) ───────────────────────────

_P = 2**255 - 19
_Q = 2**252 + 27742317777372353535851937790883648493


def _modp_inv(x: int) -> int:
    return pow(x, _P - 2, _P)


_D = -121665 * _modp_inv(121666) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)

Point = tuple[int, int, int, int]  # extended coordinates (X, Y, Z, T)


def _point_add(a: Point, b: Point) -> Point:
    aa = (a[1] - a[0]) * (b[1] - b[0]) % _P
    bb = (a[1] + a[0]) * (b[1] + b[0]) % _P
    cc = 2 * a[3] * b[3] * _D % _P
    dd = 2 * a[2] * b[2] % _P
    e, f, g, h = bb - aa, dd - cc, dd + cc, bb + aa
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _point_mul(s: int, point: Point) -> Point:
    result: Point = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            result = _point_add(result, point)
        point = _point_add(point, point)
        s >>= 1
    return result


def _point_equal(a: Point, b: Point) -> bool:
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _recover_x(y: int, sign: int) -> int | None:
    if y >= _P:
        return None
    x2 = (y * y - 1) * _modp_inv(_D * y * y + 1)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_G_Y = 4 * _modp_inv(5) % _P
_G_X = _recover_x(_G_Y, 0)
assert _G_X is not None
_G: Point = (_G_X, _G_Y, 1, _G_X * _G_Y % _P)


def _point_compress(point: Point) -> bytes:
    zinv = _modp_inv(point[2])
    x = point[0] * zinv % _P
    y = point[1] * zinv % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _point_decompress(data: bytes) -> Point | None:
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % _P)


def _secret_expand(seed: bytes) -> tuple[int, bytes]:
    if len(seed) != 32:
        raise ValueError("Ed25519 seed must be 32 bytes")
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def _sha512_modq(data: bytes) -> int:
    return int.from_bytes(hashlib.sha512(data).digest(), "little") % _Q


def public_key(seed: bytes) -> bytes:
    a, _ = _secret_expand(seed)
    return _point_compress(_point_mul(a, _G))


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    a, prefix = _secret_expand(seed)
    public = _point_compress(_point_mul(a, _G))
    r = _sha512_modq(prefix + message)
    rs = _point_compress(_point_mul(r, _G))
    h = _sha512_modq(rs + public + message)
    s = (r + h * a) % _Q
    return rs + int.to_bytes(s, 32, "little")


def ed25519_verify(public: bytes, message: bytes, signature: bytes) -> bool:
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _point_decompress(public)
    rs = signature[:32]
    r = _point_decompress(rs)
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _Q:
        return False
    h = _sha512_modq(rs + public + message)
    return _point_equal(_point_mul(s, _G), _point_add(r, _point_mul(h, a)))


# ── Registry signature format ──────────────────────────────────────────────


def signed_message(registry_bytes: bytes) -> bytes:
    return f"{SCHEMA}\n{hashlib.sha256(registry_bytes).hexdigest()}\n".encode("ascii")


def signature_file_text(signature: bytes) -> str:
    return f"{SCHEMA}\n{signature.hex()}\n"


def parse_signature_file(text: str) -> bytes:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if len(lines) != 2 or lines[0] != SCHEMA:
        raise ValueError(f"signature file must be two lines: `{SCHEMA}` and the hex signature")
    signature = bytes.fromhex(lines[1])
    if len(signature) != 64:
        raise ValueError("signature must be 64 bytes of hex")
    return signature


def sign_registry(seed: bytes, registry_bytes: bytes) -> str:
    return signature_file_text(ed25519_sign(seed, signed_message(registry_bytes)))


def verify_registry(public: bytes, registry_bytes: bytes, signature_text: str) -> bool:
    try:
        signature = parse_signature_file(signature_text)
    except ValueError:
        return False
    return ed25519_verify(public, signed_message(registry_bytes), signature)


# ── CLI ────────────────────────────────────────────────────────────────────


def _read_seed(path: Path) -> bytes:
    seed = bytes.fromhex(path.read_text(encoding="ascii").strip())
    if len(seed) != 32:
        raise SystemExit(f"{path}: seed must be 32 bytes of hex")
    return seed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    keygen = sub.add_parser("keygen", help="write a new 32-byte seed (hex) and print its public key")
    keygen.add_argument("--out", type=Path, required=True, help="seed file to create; refuses to overwrite")

    pubkey = sub.add_parser("pubkey", help="print the public key for a seed file")
    pubkey.add_argument("--key", type=Path, required=True)

    sign = sub.add_parser("sign", help="write <registry>.sig beside the registry")
    sign.add_argument("--key", type=Path, required=True, help="seed file from keygen")
    sign.add_argument("registry", type=Path)
    sign.add_argument("--out", type=Path, help="signature path (default: <registry>.sig)")

    verify = sub.add_parser("verify", help="check a registry against its signature and a public key")
    verify.add_argument("--public-key", required=True, help="32-byte public key as hex")
    verify.add_argument("registry", type=Path)
    verify.add_argument("signature", type=Path)

    args = parser.parse_args(argv)

    if args.command == "keygen":
        if args.out.exists():
            print(f"refusing to overwrite existing seed {args.out}", file=sys.stderr)
            return 1
        seed = secrets.token_bytes(32)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(seed.hex() + "\n", encoding="ascii")
        args.out.chmod(stat.S_IRUSR | stat.S_IWUSR)
        print(f"seed written to {args.out} (keep it private; back it up)")
        print(f"public key: {public_key(seed).hex()}")
        print("pin it as kPinnedRegistrySignerPublicKeyHex in tools/cli/package_registry_trust.hpp")
        return 0

    if args.command == "pubkey":
        print(public_key(_read_seed(args.key)).hex())
        return 0

    if args.command == "sign":
        seed = _read_seed(args.key)
        out = args.out or args.registry.with_name(args.registry.name + ".sig")
        text = sign_registry(seed, args.registry.read_bytes())
        out.write_text(text, encoding="ascii")
        print(f"wrote {out} (signer {public_key(seed).hex()})")
        return 0

    if args.command == "verify":
        public = bytes.fromhex(args.public_key)
        ok = verify_registry(public, args.registry.read_bytes(), args.signature.read_text(encoding="ascii"))
        print("OK" if ok else "FAIL: signature does not verify", file=sys.stdout if ok else sys.stderr)
        return 0 if ok else 1

    raise AssertionError(args.command)


if __name__ == "__main__":
    raise SystemExit(main())
