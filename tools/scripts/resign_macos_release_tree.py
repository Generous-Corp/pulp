#!/usr/bin/env python3
"""Ad-hoc re-sign every installed Mach-O executable/dylib in an SDK tree."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


class SigningError(RuntimeError):
    pass


def is_macho(path: Path) -> bool:
    result = subprocess.run(
        ["file", str(path)], capture_output=True, text=True, check=True
    )
    return "Mach-O" in result.stdout


def macho_files(root: Path) -> list[Path]:
    candidates: list[Path] = []
    for relative in ("bin", "lib"):
        directory = root / relative
        if not directory.is_dir():
            raise SigningError(f"missing SDK directory: {directory}")
        for path in sorted(directory.rglob("*")):
            if path.is_file() and is_macho(path):
                candidates.append(path)
    libexec = root / "libexec"
    if libexec.is_dir():
        for path in sorted(libexec.rglob("*")):
            if path.is_file() and is_macho(path):
                candidates.append(path)
    if not candidates:
        raise SigningError(f"no Mach-O products found below {root}")
    return candidates


def resign(root: Path) -> list[Path]:
    signed = macho_files(root)
    for path in signed:
        subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=True)
        subprocess.run(
            ["codesign", "--verify", "--strict", "--verbose=2", str(path)],
            check=True,
        )
    return signed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk_root", type=Path)
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        print("FAIL: Mach-O re-signing requires macOS", file=sys.stderr)
        return 2
    try:
        signed = resign(args.sdk_root)
    except (SigningError, OSError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"Re-signed and verified {len(signed)} installed Mach-O product(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
