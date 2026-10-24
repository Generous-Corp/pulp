#!/usr/bin/env python3
"""Assert a host-owned GPU device build carries no Dawn proc-table setter call.

With PULP_DAWN_PROCS_HOST_OWNED the device library must not reference
dawnProcSetProcs at all, so no Pulp code path can install Dawn's native procs.
The default-configured library is the control: it must reference the setter,
which proves the symbol scan can see the reference it is looking for.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

SETTER = "dawnProcSetProcs"


def undefined_symbols(nm: str, archive: Path) -> set[str]:
    output = subprocess.run([nm, "-u", str(archive)], check=True, text=True,
                            encoding="utf-8", capture_output=True).stdout
    # Mach-O prefixes C symbols with an underscore; ELF does not.
    return {line.split()[-1].lstrip("_") for line in output.splitlines()
            if line.strip() and not line.endswith(":")}


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_dawn_proc_setter_absent.py <host-owned-lib> <default-lib>")
    nm = shutil.which("nm")
    if nm is None:
        print("SKIP: no nm on PATH")
        return 77
    host_owned, default = map(Path, sys.argv[1:])
    if SETTER not in undefined_symbols(nm, default):
        print(f"INCONCLUSIVE: the default device library does not reference {SETTER}; "
              "the scan cannot see the reference it checks for", file=sys.stderr)
        return 2
    if SETTER in undefined_symbols(nm, host_owned):
        print(f"FAIL: the host-owned device library references {SETTER}", file=sys.stderr)
        return 1
    print(f"dawn-proc-setter-absent: PASS (host-owned library has no {SETTER} reference; "
          "default library control has one)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
