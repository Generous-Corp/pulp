#!/usr/bin/env python3
"""A precompiled header built in one build tree is never served to another.

Clang records the absolute path of every input in a .pch, and a consumer that
loads it re-opens those paths. With ccache's ``base_dir`` covering several
worktrees, the PCH compile's key is path-relative, so a second tree used to be
handed the first tree's .pch; once the first tree was deleted every consumer
failed with "malformed or corrupted precompiled file: could not find file".

The fixture is a real CMake + Ninja project that includes Pulp's
``tools/cmake/Ccache.cmake`` and builds a PCH carrier plus a consumer that
reuses it, exactly the shape of ``pulp-test-pch-cxx20``. Two build trees under
one isolated ccache (``base_dir`` = their common parent, ``hash_dir`` off, the
PCH sloppiness Pulp sets) reproduce the cross-tree scenario:

  build tree A -> delete A -> build tree B

With ``pulp_ccache_key_on_build_path`` applied to the carrier, B compiles its
own PCH and the consumer builds. The same scenario with the call left out is
run as a negative control and must fail with the stale-PCH error, which proves
the fixture can see the defect at all.

Exit 77 (ctest SKIP_RETURN_CODE) when ccache, ninja, cmake or a clang C++
compiler is unavailable.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
CCACHE_CMAKE = REPO / "tools" / "cmake" / "Ccache.cmake"

CMAKE = shutil.which("cmake")
NINJA = shutil.which("ninja")
CCACHE = shutil.which("ccache")
SKIP = 77

FIXTURE_CMAKE = """cmake_minimum_required(VERSION 3.24)
project(pchfix CXX)
include("{ccache_cmake}")
add_library(carrier STATIC carrier.cpp)
target_compile_options(carrier PRIVATE -Xclang -fno-pch-timestamp)
# Angle-bracket headers only, as Pulp's carriers use: a quoted absolute path
# would land in cmake_pch.hxx and make every tree's PCH key differ anyway.
target_include_directories(carrier PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}/inc")
target_precompile_headers(carrier PRIVATE <vector> <string> <common.hpp>)
if(PCHFIX_ISOLATE)
    pulp_ccache_key_on_build_path(carrier)
endif()
add_executable(consumer consumer.cpp)
target_compile_options(consumer PRIVATE -Xclang -fno-pch-timestamp)
target_include_directories(consumer PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}/inc")
target_precompile_headers(consumer REUSE_FROM carrier)
"""

SOURCES = {
    "carrier.cpp": "int carrier_stub() { return 0; }\n",
    "inc/common.hpp": "#pragma once\ninline int common_value() { return 42; }\n",
    "consumer.cpp": "int main() { std::vector<int> v{common_value()}; return v[0] == 42 ? 0 : @EXIT@; }\n",
}

STALE_PCH = "malformed or corrupted precompiled file"


def clang_cxx() -> str | None:
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        return None
    r = subprocess.run([cxx, "--version"], capture_output=True, text=True)
    return cxx if r.returncode == 0 and "clang" in r.stdout.lower() else None


CXX = clang_cxx()


class CcachePchIsolationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="pch-isolation-")).resolve()
        self.cache = self.root / "ccache"
        self.cache.mkdir()
        # The host's own ccache settings must not leak in: an isolated cache
        # dir and config, base_dir covering both trees, hash_dir off, and the
        # sloppiness Pulp's launcher requires for PCH consumers.
        conf = self.cache / "ccache.conf"
        conf.write_text(f"base_dir = {self.root}\nhash_dir = false\n"
                        "sloppiness = pch_defines,time_macros\n")
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("CCACHE_")}
        self.env.update(CCACHE_DIR=str(self.cache), CCACHE_CONFIGPATH=str(conf),
                        CXX=CXX or "c++")

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def run_cmd(self, cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, env=self.env)

    def tree(self, name: str, isolate: bool, exit_code: int = 1) -> Path:
        src = self.root / name
        src.mkdir()
        (src / "CMakeLists.txt").write_text(FIXTURE_CMAKE.format(ccache_cmake=CCACHE_CMAKE))
        for rel, text in SOURCES.items():
            (src / rel).parent.mkdir(parents=True, exist_ok=True)
            (src / rel).write_text(text.replace("@EXIT@", str(exit_code)))
        r = self.run_cmd([CMAKE, "-S", str(src), "-B", str(src / "build"), "-G", "Ninja",
                          "-DCMAKE_BUILD_TYPE=Release",
                          f"-DPCHFIX_ISOLATE={'ON' if isolate else 'OFF'}"])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("ccache enabled", r.stdout, "the fixture did not pick up ccache")
        return src

    def build(self, src: Path) -> subprocess.CompletedProcess:
        # Two edges that matter (the PCH, then the consumer); -j1 keeps the
        # fixture off a shared host's cores.
        return self.run_cmd([NINJA, "-C", str(src / "build"), "-j1", "consumer"])

    def pch(self, src: Path) -> bytes:
        return (src / "build" / "CMakeFiles" / "carrier.dir" / "cmake_pch.hxx.pch").read_bytes()

    def stats(self) -> str:
        return self.run_cmd([CCACHE, "--print-stats"]).stdout

    def cross_tree(self, isolate: bool) -> tuple[subprocess.CompletedProcess, Path, Path]:
        a = self.tree("a", isolate)
        r = self.build(a)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(str(a).encode(), self.pch(a), "tree A's PCH should name tree A")
        # B's consumer differs from A's, so its object cannot be a cache hit
        # of A's and has to load whatever PCH B was given, as a new test in a
        # fresh worktree does.
        b = self.tree("b", isolate, exit_code=2)
        shutil.rmtree(a)
        return self.build(b), a, b

    def test_pch_is_not_served_across_build_trees(self) -> None:
        r, a, b = self.cross_tree(isolate=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        pch = self.pch(b)
        self.assertNotIn(str(a).encode(), pch, "tree B was served tree A's PCH")
        self.assertIn(str(b).encode(), pch)
        # Reusing the PCH within its own tree still hits: the key is per tree,
        # not disabled.
        (b / "build" / "CMakeFiles" / "carrier.dir" / "cmake_pch.hxx.pch").unlink()
        self.run_cmd([CCACHE, "--zero-stats"])
        r = self.build(b)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        stats = dict(ln.split("\t", 1) for ln in self.stats().splitlines() if "\t" in ln)
        hits = int(stats.get("direct_cache_hit", 0)) + int(stats.get("preprocessed_cache_hit", 0))
        self.assertGreaterEqual(hits, 1, stats)

    def test_negative_control_without_isolation_serves_the_stale_pch(self) -> None:
        r, _, _ = self.cross_tree(isolate=False)
        self.assertNotEqual(r.returncode, 0, "control: the unisolated fixture should fail")
        self.assertIn(STALE_PCH, r.stdout + r.stderr)


def main() -> int:
    missing = [n for n, v in (("cmake", CMAKE), ("ninja", NINJA), ("ccache", CCACHE),
                              ("a clang C++ compiler", CXX)) if not v]
    if missing:
        print(f"SKIP: ccache PCH isolation needs {', '.join(missing)}")
        return SKIP
    prog = unittest.main(exit=False, argv=[sys.argv[0], "-v"])
    return 0 if prog.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
