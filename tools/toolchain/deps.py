#!/usr/bin/env python3
"""`pulp deps`: the toolchain pins in tools/toolchain/manifest.toml.

    pulp deps check [--format text|github|json]
        Every host tool that differs from the manifest, with the fix.
    pulp deps resolve
        Fetch what a build needs, once, before building: the checkout's pinned
        dependency sources (setup.sh --deps-only) and the npm packages from the
        committed lockfiles (npm ci), then report the host tools. It never
        installs a host tool; it names the command that does.
    pulp deps update <tool> --to <version> [--mise <pinned mise>]
        Move one pin, regenerate everything derived from it, and print the
        table of versions and digests that changed.

check runs on Python 3.9; update needs the Python 3.11+ standard library.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from typing import List, Optional

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import check_env  # noqa: E402

USAGE = __doc__.split("\n\n", 1)[1].rsplit("\n\n", 1)[0]


def default_mise(root: Path) -> Optional[Path]:
    """The installed pinned mise named by the manifest, when it is there."""
    try:
        pin = check_env.load_expected(root / "tools/toolchain/expected.json")["mise"]["path"]
    except (OSError, ValueError, KeyError):
        return None
    path = Path(pin.replace("~", os.path.expanduser("~"), 1))
    return path if path.is_file() else None


def resolve(root: Path, run=subprocess.call) -> int:
    if os.name == "nt":
        setup = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                 str(root / "setup.ps1"), "--deps-only", "--non-interactive"]
    else:
        setup = ["bash", str(root / "setup.sh"), "--deps-only", "--non-interactive"]
    print("pulp deps resolve: pinned dependency sources (setup --deps-only)", flush=True)
    rc = run(setup, cwd=str(root))
    if rc != 0:
        print(f"pulp deps resolve: setup --deps-only failed (exit {rc}); fix that first", file=sys.stderr)
        return rc
    expected = check_env.load_expected(root / "tools/toolchain/expected.json")
    stale = [f.tool.split(" ", 1)[1] for f in check_env.check_npm(expected, root)]
    for prefix in stale:
        print(f"pulp deps resolve: npm ci --prefix {prefix}", flush=True)
        rc = run(["npm", "ci", "--prefix", prefix, "--no-audit", "--no-fund"], cwd=str(root))
        if rc != 0:
            print(f"pulp deps resolve: npm ci --prefix {prefix} failed (exit {rc})", file=sys.stderr)
            return rc
    print("pulp deps resolve: host tools against the manifest", flush=True)
    return check_env.main(["--root", str(root)])


def main(argv: Optional[List[str]] = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    root = ROOT
    if not args or args[0] in ("-h", "--help", "help"):
        print(USAGE)
        return 0
    sub, rest = args[0], args[1:]
    if sub == "check":
        return check_env.main(["--root", str(root)] + rest)
    if sub == "resolve":
        if rest:
            print(f"pulp deps resolve takes no arguments, got {' '.join(rest)}", file=sys.stderr)
            return 2
        return resolve(root)
    if sub == "update":
        if sys.version_info < (3, 11):
            print(
                f"pulp deps update needs Python 3.11 or newer (found {sys.version.split()[0]}); "
                "run it with the pinned Python: MISE_ENV=toolchain mise exec -- python3 tools/toolchain/deps.py update ...",
                file=sys.stderr,
            )
            return 2
        import update  # noqa: PLC0415 - needs tomllib, so only on this path

        if "--mise" not in rest:
            mise = default_mise(root)
            if mise is not None:
                rest = rest + ["--mise", str(mise)]
        return update.main(rest + ["--root", str(root)])
    print(f"pulp deps: unknown subcommand {sub!r}\n\n{USAGE}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
