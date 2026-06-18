#!/usr/bin/env python3
"""Tests for check_portable_binary.py (the shipped-app portability guard)."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GUARD = os.path.join(HERE, "check_portable_binary.py")


def _run(blob, prefixes, strict):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(blob)
        path = f.name
    try:
        cmd = [sys.executable, GUARD, path]
        for p in prefixes:
            cmd += ["--forbid-prefix", p]
        if strict:
            cmd += ["--strict"]
        return subprocess.run(cmd, capture_output=True, text=True)
    finally:
        os.unlink(path)


def main():
    src = "/Users/dev/Code/myapp"
    # 1. Binary baking the source dir → flagged; --strict → exit 1.
    bad = (b"\x00\x01some binary prelude\x00"
           + f"{src}/assets/panel.svg".encode() + b"\x00\xff more bytes")
    r = _run(bad, [src, "/build/xyz"], strict=True)
    assert r.returncode == 1, f"expected strict failure, got {r.returncode}: {r.stderr}"
    assert "NOT portable" in r.stderr

    # 2. Same finding, warn-only (no --strict) → exit 0 but still reports.
    r = _run(bad, [src], strict=False)
    assert r.returncode == 0, f"warn-only should exit 0, got {r.returncode}"
    assert "WARNING" in r.stderr and "NOT portable" in r.stderr

    # 3. Clean binary (no forbidden prefix) → exit 0, no report.
    clean = b"\x00\x01@loader_path/libwgpu_native.dylib\x00embedded svg bytes\xff"
    r = _run(clean, [src, "/build/xyz"], strict=True)
    assert r.returncode == 0, f"clean binary should pass, got {r.returncode}: {r.stderr}"
    assert "NOT portable" not in r.stderr

    # 4. No forbidden prefixes given → succeeds quietly (no heuristic guessing).
    r = _run(bad, [], strict=True)
    assert r.returncode == 0, f"no prefixes should pass, got {r.returncode}"

    print("OK — check_portable_binary.py: 4 cases passed")


if __name__ == "__main__":
    main()
