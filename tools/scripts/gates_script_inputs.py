#!/usr/bin/env python3
"""gates.sh lane: check test/ctest_script_inputs.json whenever the diff can drift it.

`script-test-inputs-drift` needs a configured build (it reads the ctest
inventory). Without one, gates.sh used to report it NOT CHECKED and exit 0, so
a stale or hand-edited list surfaced only on the pull-request head (pr-fast,
drift-fast) one CI roundtrip later. This lane closes that gap:

1. Ask `script_test_inputs.touch_reasons` whether the diff since BASE can put
   the list out of date (the list itself, a declared entry/input, a script
   under a declared input directory, or CMake that registers tests). If not,
   exit 0 at once: no configure, no cost.
2. Reuse a configured build the pr-fast lane would pick (PULP_GATES_BUILD_DIR,
   else the newest current Ninja `build*/`).
3. Otherwise configure `build-gate` exactly as the gate does (Ninja, Release,
   tests ON, examples OFF). Configure only: nothing is compiled, so no `-j` and
   no share of the host is claimed beyond one CMake process.
   PULP_GATES_NO_CONFIGURE=1 skips this and reports the check NOT CHECKED.
4. Run the diff-scoped `--check`; on drift print the exact `--write` command.

Lines starting `NOT CHECKED locally:` are collected by gates.sh into its
summary. Exit codes: 0 in sync, not reachable, or not checked (stated); 1 drift.

Env: PULP_GATES_NO_CONFIGURE, PULP_GATES_BUILD_DIR, CMAKE (default `cmake`),
PULP_GATES_SETUP (default `<root>/setup.sh`; empty skips the dependency step).
"""
from __future__ import annotations

import argparse
import contextlib
import io
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ci"))
import script_test_inputs as sti  # noqa: E402
import source_selftests as sst  # noqa: E402

GATE_DIR = "build-gate"
TEST = "script-test-inputs-drift"


def say(msg: str = "") -> None:
    print(msg, flush=True)


def configure(root: Path, build: Path) -> str:
    """Configure ``build`` like the gate; "" on success, else why not."""
    setup = os.environ.get("PULP_GATES_SETUP", str(root / "setup.sh"))
    steps = []
    if setup and Path(setup).is_file():
        steps.append(["bash", setup, "--deps-only", "--non-interactive"])
    steps.append([os.environ.get("CMAKE", "cmake"), "-S", str(root), "-B", str(build), "-G", "Ninja",
                  "-DCMAKE_BUILD_TYPE=Release", "-DPULP_BUILD_TESTS=ON", "-DPULP_BUILD_EXAMPLES=OFF"])
    for cmd in steps:
        say(f"  script-test-inputs: running {' '.join(cmd[:2] if cmd[0] == 'bash' else cmd)}")
        try:
            proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
        except OSError as exc:
            return f"{cmd[0]} could not run: {exc}"
        if proc.returncode != 0:
            tail = "\n".join((proc.stdout + proc.stderr).strip().splitlines()[-15:])
            say(tail)
            return f"`{' '.join(cmd[:3])} …` exited {proc.returncode}"
    why = sst.unusable_build_reason(build, root)
    return f"{build.name} after configure: {why}" if why else ""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True)
    ap.add_argument("--repo-root", default=str(HERE.parents[1]))
    a = ap.parse_args(argv[1:])
    root = Path(a.repo_root).resolve()

    try:
        reasons = sti.touch_reasons(root, a.base)
    except subprocess.CalledProcessError as exc:
        say(f"NOT CHECKED locally: {TEST} (could not diff against {a.base}: {exc})")
        return 0
    if not reasons:
        say("  script-test-inputs: diff touches no declared script input; nothing to check")
        return 0
    say(f"  script-test-inputs: the diff can drift test/ctest_script_inputs.json ({len(reasons)} path(s)):")
    for r in reasons[:8]:
        say(f"    {r}")

    build, why = sst.choose_build_dir(root, os.environ)
    if build is not None:
        say(f"  script-test-inputs: build dir {build} ({why})")
    elif os.environ.get("PULP_GATES_NO_CONFIGURE") == "1":
        say("  script-test-inputs: !!! PULP_GATES_NO_CONFIGURE=1 and no usable configured build "
            f"({why}). The list is NOT verified here; CI's pr-fast / drift-fast will fail this "
            "PR if it drifted. Unset PULP_GATES_NO_CONFIGURE to configure build-gate (~40 s, no compile).")
        say(f"NOT CHECKED locally: {TEST} (PULP_GATES_NO_CONFIGURE=1; no usable build: {why})")
        return 0
    else:
        declared = os.environ.get(sst.GATES_BUILD_DIR_ENV, "").strip()
        build = Path(declared).expanduser() if declared else root / GATE_DIR
        build = build if build.is_absolute() else root / build
        say(f"  script-test-inputs: no usable configured build ({why}); configuring {build} "
            "(configure only, no compile)")
        failed = configure(root, build)
        if failed:
            say(f"NOT CHECKED locally: {TEST} (configure failed: {failed})")
            return 0

    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = sti.main(["script_test_inputs.py", "--repo-root", str(root), "--build-dir", str(build),
                       "--check", "--base", a.base])
    for line in out.getvalue().splitlines():
        say(f"  {line}")
    try:
        shown = build.resolve().relative_to(root).as_posix()
    except ValueError:
        shown = str(build)
    if rc == 1:
        say("  script-test-inputs: DRIFT. Regenerate the list from this configured tree and commit it "
            "(never hand-edit test/ctest_script_inputs.json):")
        say(f"    python3 tools/scripts/script_test_inputs.py --build-dir {shown} --write")
        say("    git add test/ctest_script_inputs.json")
        return 1
    if rc != 0:
        say(f"NOT CHECKED locally: {TEST} (script_test_inputs.py --check exited {rc} on {shown})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
