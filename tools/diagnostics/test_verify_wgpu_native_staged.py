#!/usr/bin/env python3
"""The real pinned image must reject a modified linked-runtime control."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    for name in ("manifest", "asset", "source-tree", "linked-runtime", "executable",
                 "verifier"):
        parser.add_argument(f"--{name}", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pulp-wgpu-identity-negative-") as scratch:
        mutated = Path(scratch) / Path(args.linked_runtime).name
        receipt = Path(scratch) / "receipt.json"
        shutil.copy2(args.linked_runtime, mutated)
        with mutated.open("ab") as stream:
            stream.write(b"planted-runtime-mutation")
        run = subprocess.run(
            [sys.executable, args.verifier, "--manifest", args.manifest,
             "--asset", args.asset, "--source-tree", args.source_tree,
             "--linked-runtime", str(mutated), "--executable", args.executable,
             "--receipt", str(receipt)],
            capture_output=True, text=True, encoding="utf-8", timeout=30, check=False)
        if (run.returncode != 1 or "linked_runtime_not_pinned" not in run.stderr or
                receipt.exists()):
            print(f"negative control failed: {run.returncode} {run.stderr}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
