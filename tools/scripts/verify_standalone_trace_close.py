#!/usr/bin/env python3
"""Prove a real StandaloneApp close flushes an environment Perfetto trace."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"standalone binary does not exist: {binary}")

    with tempfile.TemporaryDirectory(prefix="pulp-standalone-trace-close-") as temp:
        temp_path = Path(temp)
        trace = temp_path / "close.pftrace"
        screenshot = temp_path / "frame.png"
        env = os.environ.copy()
        env.update(
            {
                "PULP_HEADLESS": "1",
                "PULP_SCREENSHOT": str(screenshot),
                "PULP_FRAMES": "2",
                "PULP_TRACE_PATH": str(trace),
                "PULP_TRACE_SECONDS": "20",
            }
        )
        completed = subprocess.run(
            [str(binary)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=args.timeout,
            check=False,
        )
        if completed.returncode != 0:
            raise SystemExit(
                f"standalone exited {completed.returncode}\n{completed.stdout}"
            )
        if not screenshot.is_file() or screenshot.stat().st_size == 0:
            raise SystemExit(f"standalone close path wrote no screenshot\n{completed.stdout}")
        if not trace.is_file() or trace.stat().st_size == 0:
            raise SystemExit(f"standalone close path wrote no trace\n{completed.stdout}")
        print(
            f"standalone trace-close PASS: trace={trace.stat().st_size} bytes "
            f"screenshot={screenshot.stat().st_size} bytes"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
