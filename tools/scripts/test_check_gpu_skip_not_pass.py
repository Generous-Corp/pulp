#!/usr/bin/env python3
"""Self-test for check_gpu_skip_not_pass.py.

Two things here are load-bearing and neither is "the lint fires."

The first is polarity. `if (!gpu) return;` skips because the device is missing;
`if (node.gpu_available()) return;` skips because it is present, and converting
that one to SKIP() would delete a real assertion. A lint that cannot tell those
apart would force wrong conversions across the tree, so both directions are
asserted here.

The second is that the lint refuses to report a clean result when its own
patterns stop matching. That is how a source lint normally dies: the file it
guards is renamed or restructured, the scan quietly matches nothing, and the
gate stays green forever over zero coverage.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

LINT = pathlib.Path(__file__).with_name("check_gpu_skip_not_pass.py")

CLEAN = """
TEST_CASE("gpu thing", "[render][gpu]") {
    auto gpu = GpuSurface::create_dawn();
    if (!gpu) SKIP("no Dawn device");
    REQUIRE(gpu->is_available());
}
TEST_CASE("flow control is not a guard", "[render][gpu]") {
    // Skipping because the GPU IS present is the opposite situation.
    if (node.gpu_available()) return;
    if (call >= 8 && call < 20) return;
    if (c0 > 1.5) return;
    REQUIRE(node.cpu_path_ran());
}
TEST_CASE("a deliberate exemption", "[render][gpu]") {
    auto compute = GpuCompute::create();
    if (!compute) return;  // gpu-skip-lint: allow asserts graceful failure below
    REQUIRE_FALSE(compute->is_initialized());
}
"""

BARE_RETURN = CLEAN.replace('if (!gpu) SKIP("no Dawn device");', "if (!gpu) return;")
SUCCEEDED = CLEAN.replace(
    'if (!gpu) SKIP("no Dawn device");',
    'if (!gpu) { SUCCEED("no adapter -- skipped"); return; }',
)
WARNED = CLEAN.replace(
    'if (!gpu) SKIP("no Dawn device");',
    'if (!gpu) { WARN("no adapter; skipping"); return; }',
)
# Every guard shape removed: the population control must fire.
EMPTY = "TEST_CASE(\"nothing to guard\", \"[render][gpu]\") { REQUIRE(1 == 1); }\n"


def run(source: str, name: str = "test_gpu_selftest.cpp") -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as d:
        f = pathlib.Path(d) / name
        f.write_text(source)
        return subprocess.run(
            [sys.executable, "-B", str(LINT), "--file", str(f)],
            capture_output=True,
            text=True,
        )


def main() -> int:
    failures = []

    r = run(CLEAN)
    if r.returncode != 0:
        failures.append(f"clean source rejected: {r.stderr.strip()}")

    # Assert the MESSAGE, not just the exit code. Replacing the fixture's only
    # SKIP() also empties the guard population, so a lint with the bare-return
    # rule deleted still exits non-zero here -- via the population control. That
    # would leave this case passing for a reason unrelated to what it tests.
    r = run(BARE_RETURN)
    if r.returncode == 0:
        failures.append("a bare `return;` on an availability guard was accepted")
    elif "bare `return;`" not in r.stderr:
        failures.append(
            "the bare-return rule did not fire -- this case failed for some "
            f"other reason: {r.stderr.strip()}"
        )

    r = run(SUCCEEDED)
    if r.returncode == 0:
        failures.append("a SUCCEED() on an unmet precondition was accepted")
    elif "SUCCEED" not in r.stderr:
        failures.append(f"violation did not name the macro: {r.stderr.strip()}")

    r = run(WARNED)
    if r.returncode == 0:
        failures.append("a WARN() on an unmet precondition was accepted")

    # Polarity: the CLEAN case already contains three non-guard `return;`s, so a
    # lint that ignored polarity would have failed CLEAN above. Assert directly
    # that the positive-polarity line is never named, so a future loosening of
    # the predicate is caught here rather than by a wrong conversion.
    r = run(BARE_RETURN)
    if "gpu_available()) return" in r.stderr:
        failures.append(
            "`if (node.gpu_available()) return;` was reported -- the lint would "
            "force SKIP() onto a case that skips when the GPU is PRESENT"
        )

    r = run(EMPTY)
    if r.returncode == 0:
        failures.append(
            "a file with no guards at all was reported clean -- the population "
            "control did not fire"
        )

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    print("check_gpu_skip_not_pass selftest: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
