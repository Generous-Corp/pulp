#!/usr/bin/env python3
"""Run a test command in a private temp directory and fail if it leaves anything.

Tests that create scratch under the shared temp directory and never remove it
accumulate on the boot volume: on one host 1,505 ``pulp-*`` directories (about
a gigabyte) built up in the per-user temp dir in ten days, none of them visible
to any test result. This wrapper makes that a test failure.

    tmp_leak_guard.py [--ignore GLOB]... -- COMMAND [ARG]...

The command runs with ``TMPDIR``, ``TMP`` and ``TEMP`` pointing at a fresh
private directory. When it exits, every entry still inside that directory is a
leak, unless it matches an ``--ignore`` glob (for scratch the runtime itself
creates and owns, not the test). The private directory is always removed.

Exit codes:
    the command's own nonzero exit code, when it failed (leaks are reported too)
    1  the command passed but left entries in its temp directory
    0  the command passed and left nothing
    2  the wrapper was used wrongly (no command)
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import shutil
import signal
import subprocess
import sys
import tempfile

PREFIX = "pulp-tmp-leak-guard-"
LIST_LIMIT = 20


def leftovers(root: str, ignore: list[str]) -> list[str]:
    names = sorted(os.listdir(root))
    return [name for name in names
            if not any(fnmatch.fnmatch(name, pattern) for pattern in ignore)]


def remove_tree(root: str) -> None:
    # A test can leave read-only scratch (an installed pack, git objects);
    # make each directory writable so the removal cannot be refused.
    def make_writable_and_retry(function, path, _exc):
        try:
            os.chmod(os.path.dirname(path), 0o700)
            os.chmod(path, 0o700)
        except OSError:
            pass
        function(path)

    if sys.version_info >= (3, 12):
        shutil.rmtree(root, onexc=make_writable_and_retry)
    else:  # pragma: no cover - hosts below 3.12
        shutil.rmtree(root, onerror=make_writable_and_retry)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ignore", action="append", default=[],
                        help="glob for entries the runtime creates and owns")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        print("tmp-leak-guard: no command given", file=sys.stderr)
        return 2

    root = tempfile.mkdtemp(prefix=PREFIX)
    env = dict(os.environ, TMPDIR=root, TMP=root, TEMP=root)
    child = subprocess.Popen(command, env=env)

    def forward(signum, _frame):
        child.send_signal(signum)

    previous = {sig: signal.signal(sig, forward)
                for sig in (signal.SIGTERM, signal.SIGINT)}
    try:
        code = child.wait()
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        left = leftovers(root, args.ignore)
        remove_tree(root)

    if left:
        shown = ", ".join(left[:LIST_LIMIT])
        more = f" (+{len(left) - LIST_LIMIT} more)" if len(left) > LIST_LIMIT else ""
        print(f"tmp-leak-guard: the command left {len(left)} entr"
              f"{'y' if len(left) == 1 else 'ies'} in its temp directory: "
              f"{shown}{more}", file=sys.stderr)
    if code < 0:  # ended by a signal
        return 128 - code
    if code != 0:
        return code
    return 1 if left else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
