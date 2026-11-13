#!/usr/bin/env python3
"""gates.sh lane: check the generated test manifests whenever the diff can drift them.

Four files are generated from a configured build and drift the same way, by
being hand-edited or not regenerated after a test, script or public header is
added:

- `test/ctest_script_inputs/` (`script_test_inputs.py`);
- `tools/ci/source_selftests.json` (`tools/ci/source_selftests.py write`),
  which must carry each registration's TIMEOUT, RESOURCE_LOCK and argv;
- the changed-surface script families file `.shipyard/changed-surface-families.toml`
  (`changed_surface_script_families.py --write`), which must map every new
  script and list every new reader of the skills tree;
- the consumption census `docs/status/consumption-profiles.json`
  (`consumption_census.py --write`), which records every public header under an
  exported include root, so a new, removed or renamed public header owes a
  regeneration (`consumption-census-drift`).

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
   On a host gate_host_policy.sh reserves for the gate VMs (gates.sh sets
   PULP_GATES_CONFIGURE_ONLY_TEMP=1), the configure goes instead to a temporary
   `build-configure-only.*` directory under the checkout, governed as a
   background one-core lease, checked for any compile, and removed when the
   lane ends (configure_only.py); the printed `--write` commands then use
   configure_only.py, since that directory is gone.
4. Run the diff-scoped `--check`; on drift print the exact `--write` command.

The families check also has a build-free half: `--static` predicts whether the
change maps or unmaps a script the families file was not regenerated for, and
runs first (about a second). The configured `--check` decides when it runs.
When it cannot run (PULP_GATES_NO_CONFIGURE=1, a failed configure, a skip) and
the change touches a surface a stale family could bound wrongly (a top-level
tools/scripts/*.py, a skill doc, the script-inputs list, the generator, the
config or the families file), the lane FAILS instead of reporting NOT CHECKED:
the required gate checks exactly that a full CI round trip later.

Lines starting `NOT CHECKED locally:` are collected by gates.sh into its
summary. Exit codes: 0 in sync, not reachable, or not checked (stated); 1 drift,
or the families check could not run on a change it blocks.

Env: PULP_GATES_NO_CONFIGURE, PULP_GATES_CONFIGURE_ONLY_TEMP, PULP_GATES_BUILD_DIR,
CMAKE (default `cmake`),
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
import script_inputs_store as store  # noqa: E402
import source_selftests as sst  # noqa: E402
import configure_only  # noqa: E402

GATE_DIR = "build-gate"
TEST = "script-test-inputs-drift"
LANE_TEST = "source-selftest-lane-contract"
FAMILIES_TEST = "changed-surface-script-families-drift"
CENSUS_TEST = "consumption-census-drift"
CENSUS_SCRIPT = Path("tools/scripts/consumption_census.py")
CENSUS_FILE = "docs/status/consumption-profiles.json"
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".inc")
FAMILIES = HERE / "changed_surface_script_families.py"
CODEMODEL_QUERY = Path(".cmake") / "api" / "v1" / "query" / "codemodel-v2"


def generated_manifest_reasons(changed: set[str]) -> list[str]:
    """Paths whose change can put the selftest manifest or the families out of date."""
    reasons = []
    for path in sorted(changed):
        if (path.endswith(".py") and path.startswith("tools/")) \
                or path.startswith("test/cmake/") or path.endswith("CMakeLists.txt") \
                or path in ("tools/ci/source_selftests.json", ".shipyard/config.toml",
                            ".shipyard/changed-surface-families.toml") \
                or store.is_store_path(path) \
                or (path.startswith(".agents/skills/") and path.endswith("/SKILL.md")):
            reasons.append(path)
    return reasons


def census_reasons(root: Path, base: str, changed: set[str]) -> list[str]:
    """Paths whose change can put the consumption census out of date.

    The census lists every public header under an exported include root, so a
    header ADDED, DELETED or RENAMED is a reason (an edit to an existing header
    is not). Any CMake file, the census file and its script can also move it.
    """
    reasons = {p for p in changed
               if p.endswith("CMakeLists.txt") or p.endswith(".cmake")
               or p in (CENSUS_FILE, CENSUS_SCRIPT.as_posix())}
    mb = subprocess.run(["git", "-C", str(root), "merge-base", base, "HEAD"],
                        capture_output=True, text=True, encoding="utf-8")
    anchor = mb.stdout.strip() if mb.returncode == 0 and mb.stdout.strip() else base
    # Uncommitted edits count too: gates.sh is often run before the commit.
    for rev in ((anchor, "HEAD"), ("HEAD",)):
        proc = subprocess.run(["git", "-C", str(root), "diff", "--name-status",
                               "--diff-filter=ADR", *rev], capture_output=True, text=True, encoding="utf-8")
        if proc.returncode != 0:
            continue
        for line in proc.stdout.splitlines():
            for path in line.split("\t")[1:]:
                if path.endswith(HEADER_SUFFIXES) and "/include/" in path:
                    reasons.add(path)
    return sorted(reasons)


def say(msg: str = "") -> None:
    print(msg, flush=True)


# How a printed regeneration command reaches a configured tree. A kept build is
# named directly; a temporary one is gone when the lane ends, so the command
# runs under configure_only.py, which configures a new one the same way.
REGEN_VIA_TEMP = False


def regen(cmd: str, shown: str) -> str:
    """The `--write` command for the build ``shown`` (``{build}`` marks it)."""
    if REGEN_VIA_TEMP:
        return "python3 tools/scripts/configure_only.py -- " + cmd
    return cmd.replace("{build}", shown)


def configure(root: Path, build: Path) -> str:
    """Configure ``build`` like the gate (configure_only.py: dependencies, the
    gate's switches, a codemodel query, governed); "" on success, else why not."""
    why = configure_only.prepare_dependencies(root) or configure_only.configure(root, build)
    if why:
        return why
    compiled = configure_only.compile_evidence(build)
    if compiled:
        return f"{build.name}: the configure compiled {', '.join(compiled[:3])}"
    why = sst.unusable_build_reason(build, root)
    return f"{build.name} after configure: {why}" if why else ""


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True)
    ap.add_argument("--repo-root", default=str(HERE.parents[1]))
    # The configured build to verify against, as merge_tree_script_state.py
    # passes it; the same thing PULP_GATES_BUILD_DIR declares.
    ap.add_argument("--build-dir", default=None)
    a = ap.parse_args(argv[1:])
    root = Path(a.repo_root).resolve()
    if a.build_dir:
        os.environ[sst.GATES_BUILD_DIR_ENV] = str(Path(a.build_dir).expanduser())

    try:
        reasons = sti.touch_reasons(root, a.base)
        changed_paths = sti.changed_files(root, a.base)
        manifest_reasons = generated_manifest_reasons(changed_paths)
        census = census_reasons(root, a.base, changed_paths)
    except subprocess.CalledProcessError as exc:
        for test in (TEST, LANE_TEST, FAMILIES_TEST, CENSUS_TEST):
            say(f"NOT CHECKED locally: {test} (could not diff against {a.base}: {exc})")
        return 0
    if not reasons and not manifest_reasons and not census:
        say("  script-test-inputs: diff touches no declared script input or generated test "
            "manifest input; nothing to check")
        return 0
    if reasons:
        say(f"  script-test-inputs: the diff can drift test/ctest_script_inputs/ ({len(reasons)} path(s)):")
        for r in reasons[:8]:
            say(f"    {r}")
    if manifest_reasons:
        say(f"  generated manifests: the diff can drift tools/ci/source_selftests.json and the "
            f"changed-surface families ({len(manifest_reasons)} path(s)):")
        for r in manifest_reasons[:8]:
            say(f"    {r}")

    if census:
        say(f"  consumption census: the diff can drift {CENSUS_FILE} ({len(census)} path(s)):")
        for r in census[:8]:
            say(f"    {r}")

    families_paths = families_surface(sti.changed_files(root, a.base)) if manifest_reasons else []
    static_rc = check_families_static(root, a.base) if families_paths else 0

    build, why = sst.choose_build_dir(root, os.environ)
    if build is not None:
        say(f"  script-test-inputs: build dir {build} ({why})")
    elif os.environ.get("PULP_GATES_NO_CONFIGURE") != "1" \
            and os.environ.get("PULP_GATES_CONFIGURE_ONLY_TEMP") == "1":
        global REGEN_VIA_TEMP
        REGEN_VIA_TEMP = True
        say(f"  script-test-inputs: no usable configured build ({why}); gate host: configuring a "
            "temporary build-configure-only.* tree (configure only, governed background share, "
            "removed when this lane ends)")
        with configure_only.temporary_build(root) as temp:
            failed = configure(root, temp)
            if failed:
                for test in (TEST, LANE_TEST) + ((CENSUS_TEST,) if census else ()):
                    say(f"NOT CHECKED locally: {test} (configure failed: {failed})")
                return families_unverified(families_paths, static_rc, f"configure failed: {failed}")
            return run_checks(root, temp, a.base, reasons, manifest_reasons, census,
                              families_paths, static_rc)
    elif os.environ.get("PULP_GATES_NO_CONFIGURE") == "1":
        say("  script-test-inputs: !!! PULP_GATES_NO_CONFIGURE=1 and no usable configured build "
            f"({why}). The list is NOT verified here; CI's pr-fast / drift-fast will fail this "
            "PR if it drifted. Unset PULP_GATES_NO_CONFIGURE to configure build-gate (~40 s, no compile).")
        for test in (TEST, LANE_TEST) + ((CENSUS_TEST,) if census else ()):
            say(f"NOT CHECKED locally: {test} (PULP_GATES_NO_CONFIGURE=1; no usable build: {why})")
        return families_unverified(families_paths, static_rc,
                                   f"PULP_GATES_NO_CONFIGURE=1; no usable build: {why}")
    else:
        declared = os.environ.get(sst.GATES_BUILD_DIR_ENV, "").strip()
        build = Path(declared).expanduser() if declared else root / GATE_DIR
        build = build if build.is_absolute() else root / build
        if declared and (build.name.startswith(sst.CONFIGURE_ONLY_PREFIX)
                         or build.resolve().name.startswith(sst.CONFIGURE_ONLY_PREFIX)):
            # Another run's temporary configure-only tree: never configure into it.
            for test in (TEST, LANE_TEST) + ((CENSUS_TEST,) if census else ()):
                say(f"NOT CHECKED locally: {test} ({why})")
            return families_unverified(families_paths, static_rc, why)
        say(f"  script-test-inputs: no usable configured build ({why}); configuring {build} "
            "(configure only, no compile)")
        failed = configure(root, build)
        if failed:
            for test in (TEST, LANE_TEST) + ((CENSUS_TEST,) if census else ()):
                say(f"NOT CHECKED locally: {test} (configure failed: {failed})")
            return families_unverified(families_paths, static_rc, f"configure failed: {failed}")
    return run_checks(root, build, a.base, reasons, manifest_reasons, census, families_paths, static_rc)


def run_checks(root: Path, build: Path, base: str, reasons: list[str], manifest_reasons: list[str],
               census: list[str], families_paths: list[str], static_rc: int) -> int:
    """Every check the diff can drift, against the configured ``build``."""
    try:
        shown = build.resolve().relative_to(root).as_posix()
    except ValueError:
        shown = str(build)
    drift = 0
    if reasons:
        drift |= check_script_inputs(root, build, shown, base)
    if manifest_reasons:
        drift |= check_selftest_manifest(root, build, shown)
        drift |= check_families(root, build, shown, base, families_paths, static_rc)
    if census:
        drift |= check_census(root, build, shown)
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
            "(never hand-edit test/ctest_script_inputs/):")
        say("    " + regen("python3 tools/scripts/script_test_inputs.py --build-dir {build} --write", shown))
        say("    git add test/ctest_script_inputs/")
        return 1
    if rc != 0:
        say(f"NOT CHECKED locally: {TEST} (script_test_inputs.py --check exited {rc} on {shown})")
    return 0


def run_tool(cmd: list[str], root: Path) -> tuple[int, str]:
    try:
        proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True, encoding="utf-8",
                              errors="replace")
    except OSError as exc:
        return 2, f"{cmd[1]} could not run: {exc}"
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def check_census(root: Path, build: Path, shown: str) -> int:
    """`consumption-census-drift`: the census lists the headers the tree has."""
    script = root / CENSUS_SCRIPT
    if not script.is_file():
        say(f"NOT CHECKED locally: {CENSUS_TEST} ({CENSUS_SCRIPT.as_posix()} is not in this checkout)")
        return 0
    rc, out = run_tool([sys.executable, str(script), "--check", "--build-dir", str(build)], root)
    for line in out.splitlines()[-12:]:
        say(f"  {line}")
    if rc == 0:
        return 0
    if rc == 1:
        say("  consumption-census: DRIFT. Regenerate the census from this configured tree and commit it "
            "(stage a new public header first: the header list is read from git's index):")
        say("    " + regen("python3 tools/scripts/consumption_census.py --build-dir {build} --write", shown))
        say(f"    git add {CENSUS_FILE}")
        return 1
    say(f"NOT CHECKED locally: {CENSUS_TEST} (consumption_census.py --check exited {rc} on {shown})")
    return 0


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
        say("    " + regen("python3 tools/ci/source_selftests.py write --build-dir {build}", shown))
        say("    git add tools/ci/source_selftests.json")
        return 1
    say(f"NOT CHECKED locally: {LANE_TEST} (source_selftests.py check exited {rc} on {shown})")
    return 0


def families_surface(changed: set[str]) -> list[str]:
    """Changed paths a stale family could bound wrongly (the generator's blocks_on)."""
    return sorted(p for p in changed
                  if p in {".shipyard/config.toml", ".shipyard/changed-surface-families.toml",
                           "tools/scripts/changed_surface_script_families.py"}
                  or store.is_store_path(p)
                  or (p.startswith("tools/scripts/") and p.endswith(".py"))
                  or (p.startswith(".agents/skills/") and p.endswith("/SKILL.md")))


def check_families_static(root: Path, base: str) -> int:
    """The build-free prediction: 1 when the change maps or unmaps a script the
    families file does not carry, else 0 (2 when it could not predict)."""
    if not FAMILIES.is_file():
        return 2
    rc, out = run_tool([sys.executable, str(FAMILIES), "--repo-root", str(root), "--static",
                        "--base", base], root)
    for line in out.splitlines()[-14:]:
        say(f"  {line}")
    return rc if rc in (0, 1) else 2


def families_unverified(families_paths: list[str], static_rc: int, why: str) -> int:
    """The configured families check did not run. A change to a surface it
    blocks fails here rather than one CI round trip later; any other change
    keeps the check NOT CHECKED."""
    if not families_paths:
        say(f"NOT CHECKED locally: {FAMILIES_TEST} ({why})")
        return 0
    verdict = {0: "the build-free prediction found no mapping flip, but reader sets and build "
                  "targets need the configured check",
               1: "the build-free prediction above found a mapping flip",
               2: "the build-free prediction could not run"}[static_rc]
    say(f"  changed-surface script families: FAILED, not verified ({why}). This change touches "
        f"{len(families_paths)} path(s) the required gate checks against the families file "
        f"({families_paths[0]}{', ...' if len(families_paths) > 1 else ''}); {verdict}.")
    say("  Verify it before pushing, from any configured Ninja build (configure only, no compile):")
    say("    tools/scripts/gates.sh origin/main          # configures one when none is usable")
    say("    PULP_GATES_BUILD_DIR=<configured dir> tools/scripts/gates.sh origin/main")
    say("  and on drift regenerate and commit the file (any fleet host, gate hosts included):")
    say("    python3 tools/scripts/changed_surface_script_families.py --configure-only --write")
    say("    git add .shipyard/changed-surface-families.toml")
    return 1


def check_families(root: Path, build: Path, shown: str, base: str,
                   families_paths: list[str] | None = None, static_rc: int = 0) -> int:
    """`changed-surface-script-families-drift`, diff-scoped like the ctest."""
    families_paths = families_paths or []
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
            return families_unverified(families_paths, static_rc,
                                       f"reconfigure for the codemodel reply exited {rc} on {shown}")
    rc, out = run_tool([sys.executable, str(FAMILIES), "--repo-root", str(root), "--build-dir",
                        str(build), "--check", "--base", base], root)
    for line in out.splitlines()[-12:]:
        say(f"  {line}")
    if rc == 0:
        if static_rc == 1:
            say("  changed-surface script families: the configured check is in sync, so the "
                "build-free prediction above was wrong for this change; the configured check decides.")
        return 0
    if rc == 1:
        say("  changed-surface script families: DRIFT. Regenerate the families file from this "
            "configured tree and commit it (never hand-edit .shipyard/changed-surface-families.toml):")
        say("    " + (regen("python3 tools/scripts/changed_surface_script_families.py "
                         "--build-dir {build} --write", shown) if not REGEN_VIA_TEMP else
                         "python3 tools/scripts/changed_surface_script_families.py --configure-only --write"))
        say("    git add .shipyard/changed-surface-families.toml")
        return 1
    return families_unverified(families_paths, static_rc,
                               f"changed_surface_script_families.py --check exited {rc} on {shown}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))
