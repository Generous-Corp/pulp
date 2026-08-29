#!/usr/bin/env python3
"""Run the exact requested Pulp Rust trace analyzer from its clean source tree."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

GIT_REVISION = re.compile(r"^[0-9a-f]{40}$")


def main(argv: list[str]) -> int:
    root_value = os.environ.get("PULP_A3_PULP_ROOT")
    revision = os.environ.get("PULP_A3_PULP_REVISION")
    target_value = os.environ.get("PULP_A3_ANALYZER_CARGO_TARGET_ROOT")
    if not root_value or not revision or GIT_REVISION.fullmatch(revision) is None:
        print("A3 analyzer requires an exact Pulp root and revision", file=sys.stderr)
        return 2
    if not target_value:
        print("A3 analyzer requires PULP_A3_ANALYZER_CARGO_TARGET_ROOT", file=sys.stderr)
        return 2
    root = Path(root_value).resolve()
    target_root = Path(target_value).resolve()
    if not target_root.is_absolute() or target_root == root or root in target_root.parents:
        print("A3 analyzer Cargo target root must be outside the Pulp source tree", file=sys.stderr)
        return 2
    manifest = root / "experimental" / "pulp-rs" / "Cargo.toml"
    lockfile = root / "experimental" / "pulp-rs" / "Cargo.lock"
    if not manifest.is_file() or not lockfile.is_file():
        print("A3 analyzer source manifest or lockfile is unavailable", file=sys.stderr)
        return 2
    environment = dict(os.environ)
    for name in (
        "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER", "CARGO_BUILD_RUSTC_WRAPPER",
        "CARGO_ENCODED_RUSTFLAGS", "RUSTFLAGS",
    ):
        environment.pop(name, None)
    environment["CARGO_TARGET_DIR"] = str(target_root / revision)
    environment["PULP_RS_BUILD_VERSION"] = revision[:12]
    command = [
        "cargo", "run", "--quiet", "--release", "--locked", "--offline",
        "--manifest-path", str(manifest), "--", *argv,
    ]
    return subprocess.run(command, env=environment, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
