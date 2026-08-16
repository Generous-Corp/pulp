#!/usr/bin/env python3
"""Controls for the interrupted-build sentinel and its wiring.

The behavioural tests matter because a sentinel that never fires and a sentinel
that fires every time are both catastrophic, in opposite directions: one leaves
the corruption it exists to catch, the other throws away a warm build dir on
every single validation. Both are asserted here.

The wiring test matters because `arm` and `clear` are a matched pair split
across two Shipyard stages. A lane that arms without clearing leaves the marker
set permanently, so every subsequent run wipes the dir and rebuilds cold — the
exact runaway the sentinel's own docs warn about, introduced by an edit that
looks locally reasonable.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SENTINEL = ROOT / "tools" / "ci" / "build-dir-sentinel.sh"
CONFIG = ROOT / ".shipyard" / "config.toml"
MARKER = ".pulp-build-incomplete"


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(SENTINEL), *args], text=True, capture_output=True
    )


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_stale_marker_recreates_the_directory() -> int:
    """An interrupted run left its marker, so the objects cannot be trusted."""
    with tempfile.TemporaryDirectory(prefix="pulp-sentinel-") as temp:
        build = pathlib.Path(temp) / "build"
        check(run("arm", str(build)).returncode == 0, "arm failed")
        # The build is killed here: a partial object survives, and so does the
        # marker, because nothing ran at kill time.
        (build / "partial.o").write_text("truncated")

        result = run("arm", str(build))
        check(result.returncode == 0, f"second arm failed: {result.stderr}")
        check(
            not (build / "partial.o").exists(),
            "partial object survived: the next build would link against it",
        )
        check((build / MARKER).exists(), "re-armed dir must carry the marker")
    return 1


def test_clean_run_preserves_the_warm_directory() -> int:
    """The control. A sentinel that wipes unconditionally is not a guard.

    Wiping a healthy warm dir costs a cold rebuild on every validation, which
    on this lane is the difference between fitting the timeout and not.
    """
    with tempfile.TemporaryDirectory(prefix="pulp-sentinel-") as temp:
        build = pathlib.Path(temp) / "build"
        run("arm", str(build))
        (build / "valid.o").write_text("complete")
        check(run("clear", str(build)).returncode == 0, "clear failed")
        check(not (build / MARKER).exists(), "clear must remove the marker")

        run("arm", str(build))
        check(
            (build / "valid.o").exists(),
            "a cleanly-finished build dir must survive the next arm",
        )
    return 1


def test_bad_usage_is_rejected_loudly() -> int:
    """Exit 2 for misuse, distinct from 0 (clean) and any build failure."""
    check(run().returncode == 2, "missing arguments must exit 2")
    check(run("arm").returncode == 2, "missing directory must exit 2")
    with tempfile.TemporaryDirectory(prefix="pulp-sentinel-") as temp:
        check(run("frobnicate", temp).returncode == 2, "unknown verb must exit 2")
    return 1


def _lane_stages() -> dict[str, dict]:
    config = tomllib.loads(CONFIG.read_text())
    lanes: dict[str, dict] = {}
    validation = config.get("validation", {})
    for name, body in validation.items():
        if isinstance(body, dict) and "configure" in body and "build" in body:
            lanes[name] = body
        for override, sub in (body.get("overrides", {}) or {}).items() if isinstance(body, dict) else []:
            if isinstance(sub, dict) and "configure" in sub and "build" in sub:
                lanes[f"{name}.overrides.{override}"] = sub
    return lanes


def test_guard_clears_on_a_clean_failure_but_not_on_a_kill() -> int:
    """A failed stage is not an interrupted one.

    A configure that exits non-zero on its own — a dependency-floor mismatch, a
    missing toolchain — produced no object files, so wiping its dir buys nothing
    and costs a cold rebuild, which on this lane is what pushes the next run
    over the cap. A KILLED stage must still stay armed, and the wrapper can
    survive a kill that only reaches its child, so "we got here" is not proof of
    a clean exit; the shell's 128+signum convention is.
    """
    with tempfile.TemporaryDirectory(prefix="pulp-sentinel-") as temp:
        build = pathlib.Path(temp) / "build"
        build.mkdir()
        (build / "warm.o").write_text("complete")

        result = run("guard", str(build), "exit 9")
        check(result.returncode == 9, f"guard must propagate rc, got {result.returncode}")
        check(
            not (build / MARKER).exists(),
            "a cleanly failed stage must clear the marker",
        )
        check(
            (build / "warm.o").exists(),
            "a cleanly failed stage must not cost the warm build dir",
        )

        result = run("guard", str(build), "kill -9 $$")
        check(result.returncode >= 128, f"expected a signal exit, got {result.returncode}")
        check(
            (build / MARKER).exists(),
            "a KILLED stage must leave the marker armed even though the wrapper "
            "survived to observe it",
        )

        run("arm", str(build))
        check(
            not (build / "warm.o").exists(),
            "the arm after a killed stage must recreate the dir",
        )
    return 1


def test_guard_leaves_the_marker_on_success() -> int:
    """Configure succeeding hands the marker to the build stage, which clears it."""
    with tempfile.TemporaryDirectory(prefix="pulp-sentinel-") as temp:
        build = pathlib.Path(temp) / "build"
        check(run("guard", str(build), "true").returncode == 0, "guard true failed")
        check((build / MARKER).exists(), "marker must survive a successful configure")
    return 1


def test_guard_without_a_command_is_rejected() -> int:
    with tempfile.TemporaryDirectory(prefix="pulp-sentinel-") as temp:
        check(run("guard", temp).returncode == 2, "guard with no command must exit 2")
    return 1


def test_every_armed_lane_also_clears() -> int:
    """Arming without clearing wipes the build dir on every run, forever."""
    unbalanced = []
    for name, stages in _lane_stages().items():
        configure = stages.get("configure", "")
        arms = ("build-dir-sentinel.sh arm" in configure
                or "build-dir-sentinel.sh guard" in configure)
        clears = "build-dir-sentinel.sh clear" in stages.get("build", "")
        if arms != clears:
            unbalanced.append(
                f"{name}: configure arms={arms}, build clears={clears}"
            )
    check(
        not unbalanced,
        "the sentinel must be armed and cleared as a pair:\n  "
        + "\n  ".join(unbalanced),
    )
    return 1


def test_armed_lanes_are_the_posix_governed_ones() -> int:
    """At least the local mac lane must be guarded, or the fix is inert.

    This is the lane that gets killed by a timeout and the lane whose warm dir
    is shared with the agent's own `build/`, so it is the one the guard exists
    for. Asserting it explicitly stops a refactor from silently dropping it.
    """
    lanes = _lane_stages()
    default = lanes.get("default")
    check(default is not None, "validation.default lane is missing")
    check(
        "build-dir-sentinel.sh guard build" in default["configure"],
        "the default (local mac) lane must run configure under the sentinel guard",
    )
    check(
        default["configure"].index("build-dir-sentinel.sh guard")
        < default["configure"].index("cmake -S ."),
        "the sentinel must be armed BEFORE cmake configure, so a run killed "
        "during configure is caught too",
    )
    return 1


def main() -> int:
    if not SENTINEL.exists():
        print(f"build-dir-sentinel: {SENTINEL} is missing", file=sys.stderr)
        return 1
    checks = 0
    for name, test in sorted(globals().items()):
        if name.startswith("test_") and callable(test):
            checks += test()
    print(f"build-dir-sentinel: {checks} sentinel controls passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
