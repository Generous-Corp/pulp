#!/usr/bin/env python3
"""Prove tools/cmake/PulpLinkFloor.cmake can tell a bounded link closure from a
breached one.

The checker reads CMake's resolved link graph, so its selftest has to be a real
configure rather than a string comparison: each case writes a small project with
a known graph, runs the checker over it, and asserts the verdict and the reason
given. Both bounds are covered: TIER rejects a module that is reached and not
declared, REQUIRE rejects one that is declared and not reached. A green repo
says nothing on its own — `--mutate` then weakens the checker itself in eleven
ways and requires the battery to go red for each, so a pass cannot be the walk
quietly failing to arrive.

    link_floor_selftest.py            # run the battery
    link_floor_selftest.py --mutate   # also prove the battery is not vacuous
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


CHECKER = Path(__file__).resolve().parents[1] / "cmake" / "PulpLinkFloor.cmake"

# One graph serves every case, because every property worth testing is already
# in it: the consumer reaches `gamma` directly, `beta` through an INTERFACE
# library, and `alpha` only through a PRIVATE link to a static library — the
# edge CMake records as $<LINK_ONLY:> and the one a source-text reader loses.
# `delta` and `epsilon` link each other, so a walk that does not close over
# what it has seen hangs here rather than in the repo.
#
# Each library is declared in its own core/<module>/CMakeLists.txt, because the
# checker names a module from the target's SOURCE_DIR. Declaring them all in one
# file would give every target the same directory and let a broken mapping look
# correct.
GRAPH = """\
cmake_minimum_required(VERSION 3.24)
project(link_floor_fixture CXX)
set(PULP_LINK_FLOOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include("${PULP_LINK_FLOOR_CHECKER}")

add_subdirectory(core/alpha)
add_subdirectory(core/beta)
add_subdirectory(core/gamma)
add_subdirectory(core/zeta)
add_subdirectory(core/delta)
add_subdirectory(core/epsilon)

# Wired from here because the two targets name each other and neither
# subdirectory can see the one added after it.
target_link_libraries(pulp-delta PUBLIC pulp::epsilon)
target_link_libraries(pulp-epsilon PUBLIC pulp::delta)

add_library(probe MODULE consumer/unit.cpp)
target_link_libraries(probe PRIVATE pulp::gamma)
"""

# module -> the body of its core/<module>/CMakeLists.txt beyond declaring itself
MODULES = {
    "alpha": "",
    "beta": "target_link_libraries(pulp-beta PRIVATE pulp::alpha)\n",
    "gamma": "target_link_libraries(pulp-gamma INTERFACE pulp::beta)\n",
    "zeta": "",
    "delta": "",
    "epsilon": "",
}
# gamma is header-only, so the consumer reaches beta and alpha across an
# INTERFACE library rather than a compiled one.
INTERFACE_MODULES = {"gamma"}

PASS = "link-floor: probe within tier"


def case(tail: str) -> str:
    return GRAPH + tail


def tier(*modules: str) -> str:
    return f'set(PULP_LINK_FLOOR_TIER_fixture {" ".join(modules)})\n'


# (name, project tail, expected verdict, substring the output must contain)
CASES: list[tuple[str, str, bool, str]] = [
    (
        "bound_covers_closure",
        case(tier("alpha", "beta", "gamma")
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        True,
        PASS,
    ),
    (
        # The whole reason this is a configure-time walk: `beta` links `alpha`
        # PRIVATE, and a static archive cannot resolve that itself, so the edge
        # reaches `probe` anyway. A checker that believed the PRIVATE keyword
        # would call this bounded.
        "private_static_link_still_counts",
        case(tier("beta", "gamma")
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        False,
        "pulp/alpha is outside tier 'fixture', reached by probe -> pulp-gamma"
        " -> pulp-beta -> pulp-alpha",
    ),
    (
        "direct_link_outside_bound",
        case(tier("alpha", "beta")
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        False,
        "pulp/gamma is outside tier 'fixture'",
    ),
    (
        "interface_hop_outside_bound",
        case(tier("alpha", "gamma")
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        False,
        "pulp/beta is outside tier 'fixture'",
    ),
    (
        # The control, run as the checker's own case: the bound does not move,
        # a link is added, and the verdict flips. `link_added_is_caught` and
        # `bound_covers_closure` differ by one target_link_libraries line.
        "link_added_is_caught",
        case(tier("alpha", "beta", "gamma")
             + "target_link_libraries(probe PRIVATE pulp::zeta)\n"
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        False,
        "pulp/zeta is outside tier 'fixture'",
    ),
    (
        "debt_absorbs_a_recorded_edge",
        case(tier("beta", "gamma")
             + "set(PULP_LINK_FLOOR_DEBT_probe alpha)\n"
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        True,
        PASS,
    ),
    (
        "debt_cannot_outlive_its_edge",
        case(tier("alpha", "beta", "gamma")
             + "set(PULP_LINK_FLOOR_DEBT_probe zeta)\n"
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        False,
        "debt entry 'zeta' is no longer linked",
    ),
    (
        "debt_cannot_shadow_its_tier",
        case(tier("alpha", "beta", "gamma")
             + "set(PULP_LINK_FLOOR_DEBT_probe alpha)\n"
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        False,
        "debt entry 'alpha' is already inside tier",
    ),
    (
        # core/host links pulp::format while format reaches back through view,
        # so a walk that does not close over what it has seen hangs on the real
        # repo. Reproduced here in two targets; the case is bounded by the
        # subprocess timeout, so a hang fails rather than stalls.
        "cycle_terminates",
        case(tier("alpha", "beta", "gamma", "delta", "epsilon")
             + "target_link_libraries(probe PRIVATE pulp::delta)\n"
             + "pulp_assert_link_floor(probe TIER fixture)\n"),
        True,
        PASS,
    ),
    (
        # The lower bound. A tier alone cannot express this: "reaches nothing
        # extra" is satisfied most easily by reaching nothing at all, so a tier
        # naming a module the target does not link stays green while asserting a
        # link that does not exist.
        "require_is_satisfied_by_a_reached_module",
        case(tier("alpha", "beta", "gamma")
             + "pulp_assert_link_floor(probe TIER fixture REQUIRE alpha gamma)\n"),
        True,
        PASS,
    ),
    (
        # `zeta` is in the tier and is not linked, which is exactly the state the
        # upper bound cannot see. Without REQUIRE this case passes.
        "require_names_an_unlinked_module",
        case(tier("alpha", "beta", "gamma", "zeta")
             + "pulp_assert_link_floor(probe TIER fixture REQUIRE zeta)\n"),
        False,
        "pulp/zeta is required by this target but is not linked at all",
    ),
    (
        # REQUIRE grants no reach: a module named there but declared in neither
        # list could never satisfy both bounds, so the contradiction is reported
        # rather than quietly widening the tier.
        "require_outside_tier_and_debt_is_rejected",
        case(tier("alpha", "beta", "gamma")
             + "pulp_assert_link_floor(probe TIER fixture REQUIRE zeta)\n"),
        False,
        "required module 'zeta' is in neither tier 'fixture' nor",
    ),
    (
        "require_may_name_a_debt_entry",
        case(tier("beta", "gamma")
             + "set(PULP_LINK_FLOOR_DEBT_probe alpha)\n"
             + "pulp_assert_link_floor(probe TIER fixture REQUIRE alpha)\n"),
        True,
        PASS,
    ),
    (
        "unknown_tier_is_rejected",
        case("pulp_assert_link_floor(probe TIER nonexistent)\n"),
        False,
        "unknown tier 'nonexistent'",
    ),
    (
        "closure_report_names_the_edge",
        case("pulp_report_link_closure(probe)\n"),
        True,
        "link-floor: probe reaches [alpha;beta;gamma]",
    ),
]

# `$<COMPILE_ONLY:>` is a usage requirement that is deliberately not linked, so
# keeping its operand would invent an edge no linker sees. Only asserted where
# the running CMake understands the expression.
COMPILE_ONLY_CASE = (
    "compile_only_is_not_a_link",
    case(tier("alpha", "beta", "gamma")
         + "target_link_libraries(probe PRIVATE $<COMPILE_ONLY:pulp::zeta>)\n"
         + "pulp_assert_link_floor(probe TIER fixture)\n"),
    True,
    PASS,
)

# Each entry weakens the checker in one specific way. A battery that still
# passes after any of these is not measuring what it claims to measure.
MUTATIONS: list[tuple[str, str, str]] = [
    (
        "keeps generator-expression scaffolding, so $<LINK_ONLY:> hides an edge",
        'string(REGEX REPLACE "\\\\$<[A-Za-z_]+:" ";" _candidates "${_candidates}")',
        "",
    ),
    (
        "stops after the first hop",
        'set(_frontier "${_next}")',
        'set(_frontier "")',
    ),
    (
        "never asks a dependency what it propagates",
        'get_target_property(_interface "${_current}" INTERFACE_LINK_LIBRARIES)',
        'set(_interface "")',
    ),
    (
        "accepts any module it finds",
        "if(_module IN_LIST _allowed OR _module IN_LIST _debt)",
        "if(TRUE)",
    ),
    (
        "lets a debt entry outlive the edge it records",
        "elseif(NOT _entry IN_LIST _modules)",
        "elseif(FALSE)",
    ),
    (
        "lets a debt entry shadow its own tier",
        "if(_entry IN_LIST _allowed)",
        "if(FALSE)",
    ),
    (
        "treats a compile-only usage requirement as a link",
        'if(_entry MATCHES "COMPILE_ONLY")',
        "if(FALSE)",
    ),
    (
        "reports the module without the edge that brought it in",
        'string(REPLACE ">" " -> " _chain "${_chain}")',
        'set(_chain "unknown")',
    ),
    (
        "reads a module name off the target name instead of its directory",
        'if(_relative MATCHES "^core/([A-Za-z0-9_.+-]+)")',
        "if(FALSE)",
    ),
    (
        "never checks that a required module is actually reached",
        "if(NOT _required IN_LIST _modules)",
        "if(FALSE)",
    ),
    (
        "lets REQUIRE name a module no tier or debt list declares",
        "if(NOT _required IN_LIST _allowed AND NOT _required IN_LIST _debt)",
        "if(FALSE)",
    ),
]


def write_tree(root: Path) -> None:
    for module, body in MODULES.items():
        directory = root / "core" / module
        directory.mkdir(parents=True, exist_ok=True)
        if module in INTERFACE_MODULES:
            declaration = f"add_library(pulp-{module} INTERFACE)\n"
        else:
            (directory / "unit.cpp").write_text(f"int {module}_unit() {{ return 0; }}\n")
            declaration = f"add_library(pulp-{module} STATIC unit.cpp)\n"
        (directory / "CMakeLists.txt").write_text(
            declaration + f"add_library(pulp::{module} ALIAS pulp-{module})\n" + body)
    (root / "consumer").mkdir(parents=True, exist_ok=True)
    (root / "consumer" / "unit.cpp").write_text("int consumer_unit() { return 0; }\n")


def configure(root: Path, build: Path, checker: Path, project: str) -> tuple[bool, str]:
    (root / "CMakeLists.txt").write_text(project)
    completed = subprocess.run(
        ["cmake", "-S", str(root), "-B", str(build),
         f"-DPULP_LINK_FLOOR_CHECKER={checker}"],
        capture_output=True, text=True, timeout=180,
    )
    return completed.returncode == 0, completed.stdout + completed.stderr


def run_battery(checker: Path, label: str, verbose: bool) -> list[str]:
    """Run every case against `checker`; return the human-readable failures."""
    cases = list(CASES)
    if subprocess.run(["cmake", "--version"], capture_output=True, text=True,
                      check=True).stdout.split()[2] >= "3.27":
        cases.append(COMPILE_ONLY_CASE)

    failures: list[str] = []
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary) / "src"
        build = Path(temporary) / "build"
        root.mkdir()
        write_tree(root)
        for name, project, expect_pass, expect_text in cases:
            # A rejected configure leaves the cache behind; start each case from
            # a clean one so a stale entry cannot decide the next verdict.
            shutil.rmtree(build, ignore_errors=True)
            try:
                succeeded, output = configure(root, build, checker, project)
            except subprocess.TimeoutExpired:
                failures.append(f"{name}: configure did not terminate")
                continue
            if succeeded != expect_pass:
                failures.append(
                    f"{name}: expected {'pass' if expect_pass else 'failure'}, "
                    f"got {'pass' if succeeded else 'failure'}")
                if verbose:
                    failures.append(output)
                continue
            if expect_text not in output:
                failures.append(f"{name}: missing expected reason {expect_text!r}")
                if verbose:
                    failures.append(output)
    if verbose and not failures:
        print(f"{label}: {len(cases)} cases as expected")
    return failures


def run_mutations(verbose: bool) -> int:
    source = CHECKER.read_text()
    status = 0
    with tempfile.TemporaryDirectory() as temporary:
        for description, original, replacement in MUTATIONS:
            if source.count(original) != 1:
                print(f"mutation anchor is not unique: {original!r}")
                status = 1
                continue
            mutated = Path(temporary) / "PulpLinkFloor.cmake"
            mutated.write_text(source.replace(original, replacement))
            if not run_battery(mutated, description, verbose=False):
                print(f"battery still passes with a checker that {description}")
                status = 1
            elif verbose:
                print(f"  killed: a checker that {description}")
    return status


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mutate", action="store_true",
                        help="also prove the battery fails against a weakened checker")
    parser.add_argument("--verbose", action="store_true")
    arguments = parser.parse_args()

    if not CHECKER.is_file():
        print(f"missing checker: {CHECKER}")
        return 1

    failures = run_battery(CHECKER, "checker", arguments.verbose)
    for failure in failures:
        print(failure)
    if failures:
        return 1

    if arguments.mutate and run_mutations(arguments.verbose):
        return 1

    print("link_floor_selftest=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
