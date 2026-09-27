#!/usr/bin/env python3
"""Fail when an installed Mach-O binary has a dependency dyld cannot find.

A release archive can be correct and the INSTALLED layout still broken: the
installer may leave a member out, or put it somewhere the binaries do not look.
Unpacking the archive flat hides that, because every binary finds its sibling.
This check reads the installed tree instead:

- ``otool -L`` gives each Mach-O file's dependencies,
- ``otool -l`` gives its ``LC_RPATH`` entries,
- every ``@rpath/``, ``@loader_path/`` and ``@executable_path/`` dependency, and
  every absolute one outside the OS (``/usr/lib``, ``/System``), must resolve
  to a file that exists.

Usage::

    check_installed_rpaths.py <install-dir> [--json]

Exit 0 when every dependency resolves, 1 when any does not, 2 on bad usage,
77 when the host has no ``otool`` (not macOS), so a ctest can report a skip.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SKIP_RC = 77

# Mach-O thin and fat (universal) magic numbers, both byte orders.
_MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
}

# Resolved by dyld from the shared cache, so they need not exist on disk.
_SYSTEM_PREFIXES = ("/usr/lib/", "/System/")


@dataclass(frozen=True)
class Unresolved:
    binary: str
    dependency: str
    searched: tuple[str, ...]


def is_macho(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) in _MACHO_MAGICS
    except OSError:
        return False


def _otool(*args: str) -> str:
    return subprocess.run(
        ["otool", *args], check=True, capture_output=True, text=True
    ).stdout


def rpaths(path: Path) -> list[str]:
    """``LC_RPATH`` entries, in load-command order."""
    found: list[str] = []
    lines = _otool("-l", str(path)).splitlines()
    for index, line in enumerate(lines):
        if line.strip() != "cmd LC_RPATH":
            continue
        for follow in lines[index + 1 : index + 4]:
            follow = follow.strip()
            if follow.startswith("path "):
                found.append(follow[len("path ") :].rsplit(" (offset", 1)[0])
                break
    return found


def dependencies(path: Path) -> list[str]:
    """Load commands from ``otool -L``, without the file's own install id."""
    own_id = _otool("-D", str(path)).splitlines()[1:]
    own = own_id[0].strip() if own_id else None
    deps: list[str] = []
    seen_own = False
    for line in _otool("-L", str(path)).splitlines()[1:]:
        name = line.strip().split(" (compatibility", 1)[0]
        if not name:
            continue
        if own is not None and name == own and not seen_own:
            seen_own = True
            continue
        deps.append(name)
    return deps


def _expand(token_path: str, loader_dir: Path) -> str:
    for token in ("@loader_path", "@executable_path"):
        if token_path.startswith(token):
            return str(loader_dir) + token_path[len(token) :]
    return token_path


def candidates(dependency: str, binary: Path) -> tuple[str, ...] | None:
    """Paths dyld would try, or ``None`` when the dependency is the OS's."""
    loader_dir = binary.parent
    if dependency.startswith("@rpath/"):
        leaf = dependency[len("@rpath/") :]
        return tuple(
            os.path.normpath(os.path.join(_expand(rpath, loader_dir), leaf))
            for rpath in rpaths(binary)
        )
    if dependency.startswith(("@loader_path", "@executable_path")):
        return (os.path.normpath(_expand(dependency, loader_dir)),)
    if dependency.startswith(_SYSTEM_PREFIXES):
        return None
    return (dependency,)


def check_tree(root: Path) -> tuple[int, list[Unresolved]]:
    """Return (Mach-O files checked, unresolved dependencies)."""
    checked = 0
    unresolved: list[Unresolved] = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not path.is_file() or not is_macho(path):
            continue
        checked += 1
        for dependency in dependencies(path):
            tried = candidates(dependency, path)
            if tried is None:
                continue
            if not any(os.path.isfile(candidate) for candidate in tried):
                unresolved.append(
                    Unresolved(str(path.relative_to(root)), dependency, tried)
                )
    return checked, unresolved


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("install_dir", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    if shutil.which("otool") is None:
        print("check_installed_rpaths: SKIP — no otool on this host", file=sys.stderr)
        return SKIP_RC
    if not args.install_dir.is_dir():
        print(f"check_installed_rpaths: not a directory: {args.install_dir}", file=sys.stderr)
        return 2
    checked, unresolved = check_tree(args.install_dir.resolve())
    if args.json:
        print(
            json.dumps(
                {
                    "checked": checked,
                    "unresolved": [u.__dict__ for u in unresolved],
                },
                indent=2,
            )
        )
    else:
        for item in unresolved:
            print(
                f"UNRESOLVED {item.binary}: {item.dependency} "
                f"(tried: {', '.join(item.searched) or 'no LC_RPATH entries'})"
            )
        print(f"check_installed_rpaths: {checked} Mach-O file(s), {len(unresolved)} unresolved")
    if checked == 0:
        print("check_installed_rpaths: no Mach-O files found — nothing was checked", file=sys.stderr)
        return 1
    return 1 if unresolved else 0


if __name__ == "__main__":
    sys.exit(main())
