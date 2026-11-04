#!/usr/bin/env python3
"""Build Pulp's ARM64EC STL thunk archive from the MSVC ARM64 import lib.

The ARM64 msvcprt import library contains the EC helper definitions, but its
members also carry /MD directives.  Pulp's Windows Skia SDK is /MT, so copying
msvcprt.lib into a consumer creates a runtime ABI conflict.  This tool keeps
the ARM64EC members, removes only their .drectve payload, and omits the locale
implementation member whose full definition is supplied by the static CRT.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import subprocess
import tempfile


def strip_directives(path: pathlib.Path) -> None:
    data = bytearray(path.read_bytes())
    if len(data) < 20:
        raise RuntimeError(f"short COFF object: {path}")
    nsec = struct.unpack_from("<H", data, 2)[0]
    opt = struct.unpack_from("<H", data, 16)[0]
    base = 20 + opt
    for i in range(nsec):
        off = base + i * 40
        name = bytes(data[off : off + 8]).split(b"\0", 1)[0]
        if name != b".drectve":
            continue
        size = struct.unpack_from("<I", data, off + 16)[0]
        ptr = struct.unpack_from("<I", data, off + 20)[0]
        if ptr + size > len(data):
            raise RuntimeError(f"invalid .drectve bounds: {path}")
        data[ptr : ptr + size] = b"\0" * size
    path.write_bytes(data)


def select_arm64ec_members(listing: str) -> list[str]:
    """Return stable, unique ARM64EC object members from ``lib /list`` output."""
    members = set()
    for line in listing.splitlines():
        member = line.strip()
        lowered = member.lower()
        if "\\arm64ec\\" not in lowered or not lowered.endswith(".obj"):
            continue
        if lowered.endswith("\\locale0_implib.obj"):
            continue
        members.add(member)
    return sorted(members, key=str.casefold)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--msvcprt", type=pathlib.Path, required=True)
    parser.add_argument("--lib", dest="lib_exe", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    listing = subprocess.check_output(
        [str(args.lib_exe), "/list", str(args.msvcprt)],
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    members = select_arm64ec_members(listing)
    if not members:
        raise RuntimeError("msvcprt.lib has no ARM64EC members")

    with tempfile.TemporaryDirectory(prefix="pulp-arm64ec-thunks-") as temp:
        root = pathlib.Path(temp)
        objects = []
        for index, member in enumerate(members):
            obj = root / f"{index:03d}.obj"
            subprocess.run(
                [str(args.lib_exe), f"/extract:{member}", str(args.msvcprt), f"/out:{obj}"],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            strip_directives(obj)
            objects.append(obj)
        response = root / "objects.rsp"
        response.write_text("\n".join(str(obj) for obj in objects) + "\n", encoding="utf-8")
        subprocess.run(
            [str(args.lib_exe), "/machine:arm64ec", f"/out:{args.output}", f"@{response}"],
            check=True,
        )
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes, {len(members)} members)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
