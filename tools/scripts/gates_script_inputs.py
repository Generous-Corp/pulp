#!/usr/bin/env python3
"""gates.sh lane: check the generated test manifests whenever the diff can drift them.

Three files are generated from a configured build and drift the same way, by
being hand-edited or not regenerated after a test or script is added:

- `test/ctest_script_inputs.json` (`script_test_inputs.py`);
- `tools/ci/source_selftests.json` (`tools/ci/source_selftests.py write`),
  which must carry each registration's TIMEOUT, RESOURCE_LOCK and argv;
- the changed-surface script families file `.shipyard/changed-surface-families.toml`
  (`changed_surface_script_families.py --write`), which must map every new
  script and list every new reader of the skills tree.

Each surfaced only on the pull-request head (pr-fast, drift-fast) one CI
roundtrip later. This lane checks all three from one configured build; the
script-inputs mechanics below apply to each.

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
LANE_TEST = "source-selftest-lane-contract"
FAMILIES_TEST = "changed-surface-script-families-drift"
FAMILIES = HERE / "changed_surface_script_families.py"
CODEMODEL_QUERY = Path(".cmake") / "api" / "v1" / "query" / "codemodel-v2"


def generated_manifest_reasons(changed: set[str]) -> list[str]:
    """Paths whose change can put the selftest manifest or the families out of date."""
    reasons = []
    for path in sorted(changed):
        if (path.endswith(".py") and path.startswith("tools/")) \
                or path.startswith("test/cmake/") or path.endswith("CMakeLists.txt") \
                or path in ("tools/ci/source_selftests.json", ".shipyard/config.toml",
                            ".shipyard/changed-surface-families.toml",
                            "test/ctest_script_inputs.json") \
                or (path.startswith(".agents/skills/") and path.endswith("/SKILL.md")):
            reasons.append(path)
    return reasons


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
    (build / CODEMODEL_QUERY).parent.mkdir(parents=True, exist_ok=True)
    (build / CODEMODEL_QUERY).touch()
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
        manifest_reasons = generated_manifest_reasons(sti.changed_files(root, a.base))
    except subprocess.CalledProcessError as exc:
        for test in (TEST, LANE_TEST, FAMILIES_TEST):
            say(f"NOT CHECKED locally: {test} (could not diff against {a.base}: {exc})")
        return 0
    if not reasons and not manifest_reasons:
        say("  script-test-inputs: diff touches no declared script input or generated test "
            "manifest input; nothing to check")
        return 0
    if reasons:
        say(f"  script-test-inputs: the diff can drift test/ctest_script_inputs.json ({len(reasons)} path(s)):")
        for r in reasons[:8]:
            say(f"    {r}")
    if manifest_reasons:
        say(f"  generated manifests: the diff can drift tools/ci/source_selftests.json and the "
            f"changed-surface families ({len(manifest_reasons)} path(s)):")
        for r in manifest_reasons[:8]:
            say(f"    {r}")

    build, why = sst.choose_build_dir(root, os.environ)
    if build is not None:
        say(f"  script-test-inputs: build dir {build} ({why})")
    elif os.environ.get("PULP_GATES_NO_CONFIGURE") == "1":
        say("  script-test-inputs: !!! PULP_GATES_NO_CONFIGURE=1 and no usable configured build "
            f"({why}). The list is NOT verified here; CI's pr-fast / drift-fast will fail this "
            "PR if it drifted. Unset PULP_GATES_NO_CONFIGURE to configure build-gate (~40 s, no compile).")
        for test in (TEST, LANE_TEST, FAMILIES_TEST):
            say(f"NOT CHECKED locally: {test} (PULP_GATES_NO_CONFIGURE=1; no usable build: {why})")
        return 0
    else:
        declared = os.environ.get(sst.GATES_BUILD_DIR_ENV, "").strip()
        build = Path(declared).expanduser() if declared else root / GATE_DIR
        build = build if build.is_absolute() else root / build
        say(f"  script-test-inputs: no usable configured build ({why}); configuring {build} "
            "(configure only, no compile)")
        failed = configure(root, build)
        if failed:
            for test in (TEST, LANE_TEST, FAMILIES_TEST):
                say(f"NOT CHECKED locally: {test} (configure failed: {failed})")
            return 0

    try:
        shown = build.resolve().relative_to(root).as_posix()
    except ValueError:
        shown = str(build)
    drift = 0
    if reasons:
        drift |= check_script_inputs(root, build, shown, a.base)
    if manifest_reasons:
        drift |= check_selftest_manifest(root, build, shown)
        drift |= check_families(root, build, shown, a.base)
    return drift


def check_script_inputs(root: Path, build: Path, shown: str, base: str) -> int:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = sti.main(["script_test_inputs.py", "--repo-root", str(root), "--build-dir", str(build),
                       "--check", "--base", base])
    for line in out.getvalue().splitlines():
        say(f"  {line}")
    if rc == 1:
        say("  script-test-inputs: DRIFT. Regenerate the list from this configured tree and commit it "
            "(never hand-edit test/ctest_script_inputs.json):")
        say(f"    python3 tools/scripts/script_test_inputs.py --build-dir {shown} --write")
        say("    git add test/ctest_script_inputs.json")
        return 1
    if rc != 0:
        say(f"NOT CHECKED locally: {TEST} (script_test_inputs.py --check exited {rc} on {shown})")
    return 0


def run_tool(cmd: list[str], root: Path) -> tuple[int, str]:
    try:
        proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
    except OSError as exc:
        return 2, f"{cmd[1]} could not run: {exc}"
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def check_selftest_manifest(root: Path, build: Path, shown: str) -> int:
    """`source-selftest-lane-contract`: the manifest matches the registrations."""
    rc, out = run_tool([sys.executable, str(root / "tools/ci/source_selftests.py"), "check",
                        "--build-dir", str(build)], root)
    for line in out.splitlines()[-12:]:
        say(f"  {line}")
    if rc == 0:
        return 0
    if rc == 1 and "source-selftests:" in out:
        say("  source-selftests: DRIFT. Regenerate the manifest from this configured tree and commit it "
            "(never hand-edit tools/ci/source_selftests.json; add a new test with --add <name>):")
        say(f"    python3 tools/ci/source_selftests.py write --build-dir {shown}")
        say("    git add tools/ci/source_selftests.json")
        return 1
    say(f"NOT CHECKED locally: {LANE_TEST} (source_selftests.py check exited {rc} on {shown})")
    return 0


def check_families(root: Path, build: Path, shown: str, base: str) -> int:
    """`changed-surface-script-families-drift`, diff-scoped like the ctest."""
    if not FAMILIES.is_file():
        say(f"NOT CHECKED locally: {FAMILIES_TEST} ({FAMILIES.name} is not in this checkout)")
        return 0
    if not (build / CODEMODEL_QUERY).exists():
        # A build configured before the query existed has no codemodel reply;
        # one more configure of the same tree produces it (no compile).
        (build / CODEMODEL_QUERY).parent.mkdir(parents=True, exist_ok=True)
        (build / CODEMODEL_QUERY).touch()
        rc, out = run_tool([os.environ.get("CMAKE", "cmake"), "-S", str(root), "-B", str(build)], root)
        if rc != 0:
            say(f"NOT CHECKED locally: {FAMILIES_TEST} (reconfigure for the codemodel reply "
                f"exited {rc} on {shown})")
            return 0
    rc, out = run_tool([sys.executable, str(FAMILIES), "--repo-root", str(root), "--build-dir",
                        str(build), "--check", "--base", base], root)
    for line in out.splitlines()[-12:]:
        say(f"  {line}")
    if rc == 0:
        return 0
    if rc == 1:
        say("  changed-surface script families: DRIFT. Regenerate the families file from this "
            "configured tree and commit it (never hand-edit .shipyard/changed-surface-families.toml):")
        say(f"    python3 tools/scripts/changed_surface_script_families.py --build-dir {shown} --write")
        say("    git add .shipyard/changed-surface-families.toml")
        return 1
    say(f"NOT CHECKED locally: {FAMILIES_TEST} (changed_surface_script_families.py --check "
        f"exited {rc} on {shown})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
