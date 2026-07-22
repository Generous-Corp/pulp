#!/usr/bin/env python3
"""Tests for schema_drift_check.py.

Exercises the gate's logic without a C++ build by substituting a fake generator
(--emit-cmd) whose output we control. The key assertion is confirm-the-failure:
the check must PASS when the artifact matches the generator and FAIL (nonzero)
when it does not — otherwise the gate proves nothing.
"""

from __future__ import annotations

import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

_SCRIPT = Path(__file__).resolve().with_name("schema_drift_check.py")

# A fake generator: prints the bytes of the file named in argv[1]. Lets each
# case pin exactly what "freshly generated" means.
_FAKE_EMIT = (
    "import sys,pathlib;"
    "sys.stdout.buffer.write(pathlib.Path(sys.argv[1]).read_bytes())"
)


def _emit_cmd(payload: Path) -> str:
    parts = [sys.executable, "-c", _FAKE_EMIT, str(payload)]
    return " ".join(shlex.quote(p) for p in parts)


def _run(artifact: Path, payload: Path, *extra: str) -> int:
    cmd = [
        sys.executable,
        str(_SCRIPT),
        "--artifact",
        str(artifact),
        "--emit-cmd",
        _emit_cmd(payload),
        *extra,
    ]
    return subprocess.run(cmd, capture_output=True).returncode


def main() -> int:
    failures: list[str] = []

    def check(name: str, condition: bool) -> None:
        if condition:
            print(f"ok - {name}")
        else:
            failures.append(name)
            print(f"not ok - {name}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        generated = root / "generated.json"
        generated.write_bytes(b'{"$defs":{"a":1}}\n')

        # In sync: committed artifact byte-identical to generator output.
        artifact = root / "committed.json"
        artifact.write_bytes(generated.read_bytes())
        check("passes when artifact matches generator", _run(artifact, generated) == 0)

        # Drift: mutate the committed artifact — must be caught (nonzero).
        artifact.write_bytes(b'{"$defs":{"a":2}}\n')
        check("fails when artifact is stale", _run(artifact, generated) == 1)

        # Whitespace-only mutation is still drift (byte-exact gate).
        artifact.write_bytes(generated.read_bytes() + b" ")
        check("fails on trailing-byte drift", _run(artifact, generated) == 1)

        # Missing artifact is a failure, not a pass.
        missing = root / "does_not_exist.json"
        check("fails when artifact missing", _run(missing, generated) == 1)

        # --update regenerates, after which the check passes.
        fresh = root / "fresh.json"
        check("update writes artifact", _run(fresh, generated, "--update") == 0)
        check("check passes after update", _run(fresh, generated) == 0)
        check("update produced matching bytes", fresh.read_bytes() == generated.read_bytes())

        # Generator failure surfaces as an operational error (exit 2).
        bad_cmd = [
            sys.executable,
            str(_SCRIPT),
            "--artifact",
            str(artifact),
            "--emit-cmd",
            f"{shlex.quote(sys.executable)} -c 'import sys;sys.exit(3)'",
        ]
        check(
            "generator failure is exit 2",
            subprocess.run(bad_cmd, capture_output=True).returncode == 2,
        )

    if failures:
        print(f"\nFAILED: {len(failures)} case(s): {', '.join(failures)}", file=sys.stderr)
        return 1
    print("\nall schema_drift_check cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
