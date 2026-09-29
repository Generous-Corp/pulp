"""The GPU acceptance verifiers must not wedge against `git hash-object`.

`git hash-object --stdin-paths` answers each path before reading the next, so
a caller that writes the path list into a pipe while reading git's output
depends on neither pipe filling. Under kernel pipe-memory pressure macOS hands
out pipes with a few hundred bytes of buffer that still poll as writable while
full: subprocess then blocks writing git's stdin and stops draining git's
output, and git, blocked writing that output, never reads again.

Each probe runs the verifier's blob lookup in a child process (so a deadlock is
a timeout, not a hung suite) against a fake `git` that writes a large burst to
stderr before answering, with a selector that reports every registered writer
as writable, which is how the degraded pipe polls. Any version that feeds the
paths through a pipe blocks forever; one that hands git a file registers no
writer at all.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parent

# Enough paths that the request is far larger than an ordinary 64 KiB pipe.
PATH_COUNT = 20000

FAKE_GIT = r"""#!/usr/bin/env python3
import hashlib, sys
sys.stderr.write(("x" * 79 + "\n") * 200000)
sys.stderr.flush()
for line in sys.stdin:
    path = line.rstrip("\n")
    if path:
        sys.stdout.write(hashlib.sha1(path.encode()).hexdigest() + "\n")
sys.stdout.flush()
"""

PROBE = r"""
import hashlib, pathlib, selectors, subprocess, sys
sys.path.insert(0, sys.argv[1])

class DegradedPipeSelector(selectors.PollSelector):
    def select(self, timeout=None):
        writers = [key for key in self.get_map().values()
                   if key.events & selectors.EVENT_WRITE]
        ready = super().select(0 if writers else timeout)
        seen = {key.fd for key, _ in ready}
        return ready + [(key, selectors.EVENT_WRITE)
                        for key in writers if key.fd not in seen]

subprocess._PopenSelector = DegradedPipeSelector
paths = {"dir/file-%05d.txt" % i for i in range(COUNT)}
which = sys.argv[2]
if which == "trace-overhead":
    import gpu_trace_overhead_acceptance as module
    blobs = module.checkout_blobs(pathlib.Path("."), paths)
else:
    import verify_gpu_probe_acceptance as module
    blobs = module._checkout_blobs(paths)
expected = {p: hashlib.sha1(p.encode()).hexdigest() for p in paths}
print("OK" if blobs == expected else "WRONG %d" % len(blobs))
""".replace("COUNT", str(PATH_COUNT))


class HashObjectPipeDeadlockTest(unittest.TestCase):
    def run_probe(self, which: str) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = pathlib.Path(tmp)
            fake = bin_dir / "git"
            fake.write_text(FAKE_GIT)
            fake.chmod(0o755)
            env = dict(os.environ)
            env["PATH"] = f"{bin_dir}{os.pathsep}{env.get('PATH', '')}"
            try:
                completed = subprocess.run(
                    [sys.executable, "-c", PROBE, str(SCRIPTS), which],
                    cwd=tmp, env=env, text=True, capture_output=True, timeout=90,
                )
            except subprocess.TimeoutExpired:
                self.fail(f"{which} blob lookup deadlocked against git's pipes")
            self.assertEqual(completed.returncode, 0, completed.stderr[-2000:])
            self.assertEqual(completed.stdout.strip(), "OK")

    def test_trace_overhead_checkout_blobs_cannot_wedge(self) -> None:
        self.run_probe("trace-overhead")

    def test_probe_acceptance_checkout_blobs_cannot_wedge(self) -> None:
        self.run_probe("probe-acceptance")


if __name__ == "__main__":
    unittest.main()
