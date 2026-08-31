#!/usr/bin/env python3
"""Pin the build plan `setup.sh` resolves for a fresh checkout.

`setup.sh` bootstraps a checkout; the build it performs on the way is a
convenience, not the point. Two properties of that build are easy to regress
silently because nothing else in the tree reads them:

  * it must be a Release build — Release is this repo's documented default, and
    a Debug GPU/JS UI build is dramatically slower for anyone who then runs it;
  * it must leave the example projects out — a from-scratch examples tree is
    many minutes of compile nobody asked for, and it hard-fails the
    PULP_HAS_SKIA gate on a checkout whose Skia is still an LFS pointer.

The resulting configure matches the required CI gate's own arguments, so this
guard also keeps the bootstrap path and the gate from drifting apart.

Both properties stay overridable (`--debug`, `--examples`); the guard asserts
the defaults and that the overrides still work.

Exit 0 when the plan holds, 1 with a diagnosis when it does not.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def resolve_plan(setup: Path, args: list[str]) -> dict[str, str]:
    """Ask setup.sh for the plan it would run, without running it."""
    # setup.sh ignores arguments it does not recognize, so a script without the
    # flag would run the entire bootstrap instead of printing. Refuse first.
    if "--print-plan)" not in setup.read_text():
        raise SystemExit(
            f"{setup} has no --print-plan handler; refusing to invoke it, because "
            "an unrecognized flag would run the full bootstrap"
        )
    proc = subprocess.run(
        ["bash", str(setup), *args, "--print-plan"],
        capture_output=True,
        text=True,
        cwd=str(setup.parent),
        timeout=120,
    )
    if proc.returncode != 0:
        raise SystemExit(
            f"setup.sh {' '.join([*args, '--print-plan'])} exited {proc.returncode}\n"
            f"{proc.stdout}{proc.stderr}"
        )
    plan: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            plan[key.strip()] = value.strip()
    if not plan:
        raise SystemExit("setup.sh --print-plan produced no key=value output")
    return plan


def check(setup: Path) -> list[str]:
    problems: list[str] = []

    default = resolve_plan(setup, [])
    if default.get("build_type") != "Release":
        problems.append(
            f"default build type is {default.get('build_type')!r}, expected 'Release'"
        )
    if default.get("examples") != "OFF":
        problems.append(
            f"default examples setting is {default.get('examples')!r}, expected 'OFF'"
        )

    # The declared plan is only worth asserting if the configure command it
    # advertises actually carries the same values.
    configure = default.get("configure", "")
    for expected in ("-DCMAKE_BUILD_TYPE=Release", "-DPULP_BUILD_EXAMPLES=OFF"):
        if expected not in configure:
            problems.append(f"configure command is missing {expected}: {configure}")

    # The overrides are the reason the defaults are safe to tighten.
    debug = resolve_plan(setup, ["--debug"])
    if debug.get("build_type") != "Debug":
        problems.append(f"--debug resolved to {debug.get('build_type')!r}, expected 'Debug'")

    examples = resolve_plan(setup, ["--examples"])
    if examples.get("examples") != "ON":
        problems.append(f"--examples resolved to {examples.get('examples')!r}, expected 'ON'")

    problems.extend(check_source(setup))

    return problems


def check_source(setup: Path) -> list[str]:
    """Assert the command setup.sh really runs, not just the one it prints.

    The printed plan is a separate string from the invocation at the bottom of
    the script, so the two can drift. Read the invocation itself.
    """
    problems: list[str] = []
    source = setup.read_text()

    configure = extract_command(source, "cmake -S ")
    if configure is None:
        problems.append("no `cmake -S` configure invocation found in setup.sh")
    else:
        for expected in ('-DCMAKE_BUILD_TYPE="$BUILD_TYPE"',
                         '-DPULP_BUILD_EXAMPLES="$BUILD_EXAMPLES"'):
            if expected not in configure:
                problems.append(
                    f"setup.sh's configure does not pass {expected}: {configure!r}"
                )

    build = extract_command(source, "cmake --build ")
    if build is None:
        problems.append("no `cmake --build` invocation found in setup.sh")
    elif "-j" in build or "--parallel" in build:
        # A shared host cannot absorb a bootstrap that claims every core; the
        # governor supplies the parallelism instead.
        problems.append(
            f"setup.sh's build sets its own parallelism instead of deferring to "
            f"the build governor: {build!r}"
        )

    return problems


def extract_command(source: str, prefix: str) -> str | None:
    """Return the first shell command containing `prefix`, line joins resolved.

    Comments and the script's own echoed help text mention these commands too,
    so skip anything that is not an invocation.
    """
    lines = source.splitlines()
    for index, line in enumerate(lines):
        stripped = line.strip()
        if prefix not in stripped:
            continue
        if stripped.startswith(("#", "echo ", "printf ")):
            continue
        command = stripped
        cursor = index
        while command.endswith("\\") and cursor + 1 < len(lines):
            cursor += 1
            command = command[:-1].rstrip() + " " + lines[cursor].strip()
        return command
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--setup",
        type=Path,
        default=REPO_ROOT / "setup.sh",
        help="path to the setup.sh under test (default: the repo's own)",
    )
    opts = parser.parse_args()

    setup = opts.setup.resolve()
    if not setup.is_file():
        print(f"setup-bootstrap-plan-guard: no setup.sh at {setup}", file=sys.stderr)
        return 1

    problems = check(setup)
    if problems:
        print("setup-bootstrap-plan-guard: FAIL", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\nsetup.sh bootstraps a checkout: its build must default to Release\n"
            "with examples off. Use --debug / --examples to opt back in.",
            file=sys.stderr,
        )
        return 1

    print("setup-bootstrap-plan-guard: OK — bootstrap builds Release, examples off.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
