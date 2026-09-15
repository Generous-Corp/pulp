#!/usr/bin/env python3
"""Self-test for check_skip_not_pass.py.

Three things here are load-bearing and none of them is "the lint fires."

The first is polarity. `if (!gpu) return;` skips because the device is missing;
`if (node.gpu_available()) return;` skips because it is present, and converting
that one to SKIP() would delete a real assertion. A lint that cannot tell those
apart would force wrong conversions across the tree, so both directions are
asserted here.

The second is the message classifier. The rule cannot key on syntax, because
SUCCEED("no crash across repeated attach/detach") states a real observed outcome
while SUCCEED("skipped: pulp not built") states that nothing was observed. Run
whole-tree without that split the rule reports every informational assertion in
the tree, so both polarities of the message test are asserted here too.

The third is that the lint refuses to report a clean result when its own
patterns stop matching. That is how a source lint normally dies: the file it
guards is renamed or restructured, the scan quietly matches nothing, and the
gate stays green forever over zero coverage.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

LINT = pathlib.Path(__file__).with_name("check_skip_not_pass.py")

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
# A SUCCEED that states a real observed outcome using words the skip vocabulary
# also matches ("unavailable"). Only the positive-outcome override rescues it,
# so this fixture actually exercises that override -- a message the skip
# vocabulary never matched would pass this case no matter what the override did.
# Without the rescue, widening the scan to the whole test tree turns the rule
# into a false-positive generator and it gets disabled instead of fixed.
INFORMATIONAL = CLEAN.replace(
    'if (!gpu) SKIP("no Dawn device");',
    'if (!gpu) SKIP("no Dawn device");\n'
    '    SUCCEED("unavailable is the only state in which the warning is true");',
)
# The second skip vocabulary: a platform/build precondition, no "skip" word.
PLATFORM_ONLY = CLEAN.replace(
    'if (!gpu) SKIP("no Dawn device");',
    'SUCCEED("Linux AT-SPI provider is a Linux-only runtime backend");',
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

    r = run(INFORMATIONAL)
    if r.returncode != 0:
        failures.append(
            "SUCCEED(\"unavailable is the only state ...\") was reported -- the "
            "positive-outcome override no longer rescues a stated outcome whose "
            f"wording the skip vocabulary also matches: {r.stderr.strip()}"
        )

    r = run(PLATFORM_ONLY)
    if r.returncode == 0:
        failures.append(
            "SUCCEED(\"... is a Linux-only runtime backend\") was accepted -- the "
            "classifier only recognises the word `skip`, so the platform-only "
            "half of the population stays invisible"
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
    print("check_skip_not_pass selftest: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
