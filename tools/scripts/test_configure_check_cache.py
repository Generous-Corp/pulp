#!/usr/bin/env python3
"""End-to-end test for tools/cmake/PulpConfigureCheckCache.cmake.

Configures a toy project whose "dependency" runs real configure checks, with
the module wrapped around it the way PulpDependencies.cmake wraps SDL3:

  1. A first fresh configure runs every check and records the results.
  2. A second fresh build directory replays them: the dependency's try_compiles
     no longer run, and every result matches the first configure's.
  3. Editing the dependency's CMake source changes the key, so nothing replays.
  4. A -D on the command line beats a recorded value.
  5. PULP_CONFIGURE_CHECK_CACHE=OFF records and replays nothing.

Exit 77 (ctest SKIP) off Apple, where the module is deliberately inert.
"""

from __future__ import annotations

import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MODULE = REPO / "tools" / "cmake" / "PulpConfigureCheckCache.cmake"

# Real checks of each kind SDL3 uses: a symbol that exists, one that does not,
# a header that exists, one that does not, and a source-compiles probe.
DEP_CMAKE = """\
cmake_minimum_required(VERSION 3.24)
project(fakedep C)
include(CheckSymbolExists)
include(CheckIncludeFile)
include(CheckCSourceCompiles)
check_symbol_exists(atan "math.h" FD_HAS_ATAN)
check_symbol_exists(pulp_no_such_symbol_xyz "stdio.h" FD_HAS_BOGUS)
check_include_file(stdint.h FD_HAVE_STDINT_H)
check_include_file(pulp_no_such_header_xyz.h FD_HAVE_BOGUS_H)
check_c_source_compiles("int main(void){return 0;}" FD_COMPILES)
set(FD_NOT_A_CHECK "1" CACHE INTERNAL "not a configure check")
set(FD_A_PATH "/some/where" CACHE INTERNAL "Test FD_A_PATH")
"""

TOP_CMAKE = """\
cmake_minimum_required(VERSION 3.24)
project(checkcache C)
include("{module}")
pulp_configure_check_cache_begin(FakeDep "${{CMAKE_CURRENT_SOURCE_DIR}}/fakedep")
add_subdirectory(fakedep)
pulp_configure_check_cache_end(FakeDep)
"""

CHECK_VARS = (
    "FD_HAS_ATAN",
    "FD_HAS_BOGUS",
    "FD_HAVE_STDINT_H",
    "FD_HAVE_BOGUS_H",
    "FD_COMPILES",
)


def configure(src: Path, build: Path, cache_dir: Path, *extra: str) -> str:
    shutil.rmtree(build, ignore_errors=True)
    env = dict(os.environ, PULP_CONFIGURE_CHECK_CACHE_DIR=str(cache_dir))
    args = ["cmake", "-S", str(src), "-B", str(build), *extra]
    if shutil.which("ninja"):
        args += ["-G", "Ninja"]
    proc = subprocess.run(args, env=env, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"configure failed:\n{proc.stdout}\n{proc.stderr}")
    return proc.stdout


def cache_values(build: Path) -> dict[str, str]:
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        m = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if m:
            values[m.group(1)] = m.group(2)
    return values


def dep_try_compiles(build: Path) -> int:
    log = build / "CMakeFiles" / "CMakeConfigureLog.yaml"
    if not log.exists():
        return 0
    text = log.read_text()
    return sum(
        1
        for entry in text.split('kind: "try_compile-v1"')[1:]
        if "fakedep/CMakeLists.txt" in entry.split("checks:")[0]
    )


def main() -> int:
    if platform.system() != "Darwin":
        print("SKIP: the configure-check cache is Apple-only")
        return 77
    failures: list[str] = []

    def expect(cond: bool, what: str) -> None:
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    with tempfile.TemporaryDirectory(prefix="pulp-check-cache-") as tmp:
        root = Path(tmp)
        src = root / "src"
        (src / "fakedep").mkdir(parents=True)
        (src / "CMakeLists.txt").write_text(TOP_CMAKE.format(module=MODULE.as_posix()))
        (src / "fakedep" / "CMakeLists.txt").write_text(DEP_CMAKE)
        cache_dir = root / "checks"

        out1 = configure(src, root / "b1", cache_dir)
        first = cache_values(root / "b1")
        recorded = sorted(cache_dir.glob("FakeDep-*.cmake"))
        expect(len(recorded) == 1, "exactly one results file is written")
        body = recorded[0].read_text() if recorded else ""
        seeded = re.findall(r"^pulp_configure_check_cache_seed\((\w+) ", body, re.M)
        expect(sorted(seeded) == sorted(CHECK_VARS), f"first configure records exactly the five check results ({seeded})")
        expect(f"recorded {len(seeded)} configure-check results" in out1, "the record is reported")
        expect("FD_NOT_A_CHECK" not in body, "an INTERNAL boolean without a check help string is not recorded")
        expect("FD_A_PATH" not in body, "a non-boolean INTERNAL value is not recorded")
        expect("CMAKE_" not in body.replace("PulpConfigureCheckCache.cmake", ""), "no CMAKE_* variable is recorded")
        expect(first.get("FD_HAS_ATAN") == "1" and first.get("FD_HAS_BOGUS") == "",
               "control: the real checks produced both a positive and a negative result")
        expect(dep_try_compiles(root / "b1") == 5, "control: the first configure ran all five try_compiles")

        out2 = configure(src, root / "b2", cache_dir)
        second = cache_values(root / "b2")
        expect("replayed configure checks" in out2, "a fresh build dir replays the recorded results")
        expect(dep_try_compiles(root / "b2") == 0, "the replayed configure runs none of the dependency's try_compiles")
        for var in CHECK_VARS:
            expect(second.get(var) == first.get(var), f"{var} replays as {first.get(var)!r}")

        out3 = configure(src, root / "b3", cache_dir, "-DFD_HAS_BOGUS=forced")
        expect(cache_values(root / "b3").get("FD_HAS_BOGUS") == "forced", "a -D value beats a recorded result")
        expect("replayed configure checks" in out3, "the other results still replay alongside the -D")

        # An existing build dir keeps its own answers: re-configure b1 after
        # changing one of its cached results, with the recorded file present.
        cache_file = root / "b1" / "CMakeCache.txt"
        cache_file.write_text(cache_file.read_text().replace(
            "FD_HAS_ATAN:INTERNAL=1", "FD_HAS_ATAN:INTERNAL=kept"))
        env = dict(os.environ, PULP_CONFIGURE_CHECK_CACHE_DIR=str(cache_dir))
        subprocess.run(["cmake", str(root / "b1")], env=env, check=True, capture_output=True)
        expect(cache_values(root / "b1").get("FD_HAS_ATAN") == "kept",
               "re-configuring an existing build dir never overwrites its cached results")

        (src / "fakedep" / "CMakeLists.txt").write_text(DEP_CMAKE + "# edited\n")
        out4 = configure(src, root / "b4", cache_dir)
        expect("replayed configure checks" not in out4, "editing the dependency's CMake source changes the key")
        expect(dep_try_compiles(root / "b4") == 5, "the edited dependency re-runs its checks")
        expect(len(list(cache_dir.glob("FakeDep-*.cmake"))) == 2, "the new key gets its own results file")

        off_dir = root / "checks-off"
        out5 = configure(src, root / "b5", off_dir, "-DPULP_CONFIGURE_CHECK_CACHE=OFF")
        expect(not off_dir.exists() and "recorded" not in out5, "PULP_CONFIGURE_CHECK_CACHE=OFF writes nothing")

    if failures:
        print(f"\n{len(failures)} failure(s)")
        return 1
    print("\nall configure-check cache checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
