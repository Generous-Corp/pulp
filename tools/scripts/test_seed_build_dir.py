#!/usr/bin/env python3
"""Tests for seed_build_dir.py against a real CMake + Ninja fixture.

The fixture is a two-target project whose executable bakes its source dir in
through a ``-D`` define, which is the case a naive retarget gets wrong: the
object compiles fine, the command hash can be rewritten to match, and the
binary still points at the donor. Every case that clones also audits the
donor (inode + mtime of every file outside ``.git``) so a retarget that
writes back into the donor cannot pass.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import seed_build_dir as sbd  # noqa: E402

SCRIPT = HERE / "seed_build_dir.py"
CMAKE = shutil.which("cmake")
NINJA = shutil.which("ninja")
GIT_ENV = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}

FIXTURE = {
    "CMakeLists.txt": """cmake_minimum_required(VERSION 3.24)
project(seedfix CXX)
# CONFIGURE_DEPENDS adds the always-dirty glob check Pulp's build dirs carry,
# which a plain `ninja -n` reports instead of the real plan.
file(GLOB LIB_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/a/*.cpp)
add_library(lib STATIC ${LIB_SRCS})
target_include_directories(lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/a)
add_executable(app main.cpp)
target_link_libraries(app lib)
target_compile_definitions(app PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
enable_testing()
add_test(NAME app COMMAND app)
# A Ninja build nested in the build dir, logged with absolute paths, the way
# FetchContent's _deps/<name>-subbuild is.
set(N ${CMAKE_BINARY_DIR}/nested)
file(WRITE ${N}-src/CMakeLists.txt "cmake_minimum_required(VERSION 3.24)
project(nested NONE)
add_custom_command(OUTPUT ${N}/stamp COMMAND \\${CMAKE_COMMAND} -E touch ${N}/stamp)
add_custom_target(s ALL DEPENDS ${N}/stamp)
")
execute_process(COMMAND ${CMAKE_COMMAND} -S ${N}-src -B ${N} -G Ninja
                OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${CMAKE_COMMAND} --build ${N} OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
""",
    "a/lib.hpp": "int f();\n",
    "a/lib.cpp": '#include "lib.hpp"\nint f(){return 1;}\n',
    "main.cpp": '#include "lib.hpp"\n#include <cstdio>\n'
                'int main(){ std::printf("%s\\n", FIXTURE_DIR); return f()==1?0:1; }\n',
    ".gitignore": "build/\n",
}


def run(cmd, cwd=None, check=True, env=None):
    return subprocess.run(cmd, cwd=cwd, check=check, capture_output=True, text=True,
                          env=env or GIT_ENV)


def audit(root: Path) -> dict[str, tuple[int, int]]:
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for n in filenames:
            p = Path(dirpath) / n
            st = os.lstat(p)
            out[str(p.relative_to(root))] = (st.st_ino, st.st_mtime_ns)
    return out


def dirty_edges(build: Path) -> list[str]:
    # A copy of the manifest under another name has no regeneration edge, so
    # the dry run plans the build instead of stopping at the glob check.
    shutil.copyfile(build / "build.ninja", build / "t.ninja")
    try:
        out = run([NINJA, "-C", str(build), "-f", "t.ninja", "-n"]).stdout
    finally:
        (build / "t.ninja").unlink()
    names = []
    for line in out.splitlines():
        if line.startswith("[") and "] " in line:
            names.append(line.split("] ", 1)[1])
    return names


def clonefile_supported(path: Path) -> bool:
    probe = path / "probe.txt"
    probe.write_text("x")
    try:
        sbd.clonefile(probe, path / "probe.clone")
        return True
    except OSError:
        return False
    finally:
        for p in (probe, path / "probe.clone"):
            if p.exists():
                p.unlink()


@unittest.skipUnless(sys.platform == "darwin", "APFS clonefile is macOS-only")
@unittest.skipUnless(CMAKE and NINJA, "needs cmake and ninja on PATH")
class SeedBuildDirTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Same volume as the checkout, so clonefile is exercised for real.
        cls.tmp = Path(tempfile.mkdtemp(prefix="seed-build-", dir=str(HERE)))
        if not clonefile_supported(cls.tmp):
            shutil.rmtree(cls.tmp)
            raise unittest.SkipTest("clonefile unsupported on this volume")
        cls.donor = cls.tmp / "donor"
        cls.donor.mkdir()
        for rel, text in FIXTURE.items():
            p = cls.donor / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        run(["git", "init", "-q"], cwd=cls.donor)
        run(["git", "add", "."], cwd=cls.donor)
        run(["git", "commit", "-qm", "fixture"], cwd=cls.donor)
        run([CMAKE, "-S", str(cls.donor), "-B", str(cls.donor / "build"), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release"])
        run([NINJA, "-C", str(cls.donor / "build")])
        cls.donor_audit = audit(cls.donor)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def worktree(self, name: str) -> Path:
        path = self.tmp / name
        run(["git", "worktree", "add", "-q", str(path), "HEAD"], cwd=self.donor)
        return path

    def seed(self, target: Path, *extra: str, donor: str | None = None) -> tuple[int, dict]:
        cmd = [sys.executable, str(SCRIPT), "--from", donor or str(self.donor),
               "--to", str(target), "--json", *extra]
        r = run(cmd, check=False)
        receipt = json.loads(r.stdout) if r.returncode == 0 and r.stdout.strip() else {}
        return r.returncode, receipt if r.returncode == 0 else {"stderr": r.stderr}

    def assert_donor_untouched(self):
        self.assertEqual(audit(self.donor), self.donor_audit, "donor was written to")

    # -- hashing --------------------------------------------------------------

    def test_hasher_reproduces_the_fixtures_own_log(self):
        control = sbd.log_hash_match_rate(self.donor / "build", NINJA)
        self.assertGreater(control["checked"], 0)
        self.assertEqual(control["matched"], control["checked"], control)

    def test_rapidhash_matches_ninja_across_command_lengths(self):
        # Ninja prints the hash unpadded, so a leading-zero hash is the case a
        # 16-digit string compare gets wrong; sweep lengths across the 16/48-
        # byte rapidhash regimes and compare as integers.
        base = self.tmp / "lengths"
        base.mkdir()
        mismatches = []
        for k in range(0, 70, 3):
            d = base / f"p{k}"
            d.mkdir()
            (d / "x.cpp").write_text("int main(){}\n")
            cmd = f"/usr/bin/c++ -DPAD={'P' * k} -c x.cpp -o x.o"
            (d / "build.ninja").write_text(f"rule cc\n  command = {cmd}\nbuild x.o: cc x.cpp\n")
            run([NINJA, "-C", str(d)])
            _, entries = sbd.parse_ninja_log(d / ".ninja_log")
            if not sbd.log_hash_equal(entries[0][4], sbd.rapidhash(cmd.encode())):
                mismatches.append((len(cmd), entries[0][4]))
        self.assertEqual(mismatches, [])
        self.assertNotEqual(sbd.murmur_hash64a(b"a"), sbd.murmur_hash64a(b"b"))

    def test_build_statement_outputs_expand_top_level_bindings(self):
        # CMake spells a custom command's implicit output through a binding,
        # and the log records the expanded absolute path, so the mapper must
        # expand it or every generated file reads as "command line changed".
        ninja = self.tmp / "bindings.ninja"
        ninja.write_text(
            "cmake_ninja_workdir = /w/build/\n"
            "rule R\n  command = touch $out\n"
            "build gen/a.cpp | ${cmake_ninja_workdir}gen/a.cpp: R src$ x.in\n"
            "build b.o c.o: R $\n    gen/a.cpp\n"
        )
        outs = list(sbd.ninja_build_outputs(ninja))
        self.assertEqual(outs[0], (["gen/a.cpp", "/w/build/gen/a.cpp"], "gen/a.cpp"))
        self.assertEqual(outs[1], (["b.o", "c.o"], "b.o"))

    def test_deps_log_round_trips_byte_identical_under_identity(self):
        src = self.donor / "build" / ".ninja_deps"
        copy = self.tmp / "deps.copy"
        shutil.copyfile(src, copy)
        stats = sbd.rewrite_ninja_deps(copy, lambda p: p)
        self.assertEqual(copy.read_bytes(), src.read_bytes())
        self.assertGreater(stats["path_records"], 0)
        self.assertGreater(stats["deps_records"], 0)
        copy.unlink()

    # -- seeding ----------------------------------------------------------------

    def test_same_commit_rebuilds_only_the_path_bearing_object(self):
        target = self.worktree("same")
        rc, receipt = self.seed(target)
        self.assertEqual(rc, 0, receipt)
        self.assertEqual(receipt["hash_control"]["hasher"], "rapidhash" if
                         sbd.hasher_for_log((self.donor / "build" / ".ninja_log")
                                            .read_text().splitlines()[0]) == "rapidhash"
                         else "murmur64a")
        # main.cpp.o bakes FIXTURE_DIR in; it and its link are all that is left.
        self.assertEqual(sorted(receipt["tainted"]),
                         sorted(["CMakeFiles/app.dir/main.cpp.o", "app"]))
        self.assertEqual(receipt["dirty_edges"], 2)
        edges = dirty_edges(target / "build")
        self.assertEqual(len(edges), 2, edges)
        self.assertTrue(any("main.cpp.o" in e for e in edges), edges)
        self.assertTrue(any("Linking CXX executable app" in e for e in edges), edges)
        self.assert_donor_untouched()
        # No text file still names the donor.
        leftovers = run(["grep", "-rlF", "--exclude=" + sbd.RECEIPT_NAME,
                         str(self.donor) + "/", str(target / "build")], check=False).stdout
        self.assertEqual(leftovers, "")
        run([NINJA, "-C", str(target / "build")])
        out = run([str(target / "build" / "app")]).stdout.strip()
        self.assertEqual(out, str(target))
        self.assertTrue((target / "build" / sbd.RECEIPT_NAME).exists())
        self.assert_donor_untouched()

    def test_new_commit_rebuilds_only_what_changed(self):
        target = self.worktree("changed")
        (target / "a" / "lib.hpp").write_text("int f();\nint g();\n")
        (target / "a" / "lib.cpp").write_text('#include "lib.hpp"\nint f(){return 1;}\nint g(){return 2;}\n')
        run(["git", "commit", "-qam", "add g"], cwd=target)
        rc, receipt = self.seed(target)
        self.assertEqual(rc, 0, receipt)
        self.assertEqual(receipt["sources_differ_or_dirty"], 2)
        edges = dirty_edges(target / "build")
        self.assertEqual(receipt["dirty_edges"], len(edges), edges)
        # lib.cpp changed; main.cpp includes lib.hpp (via the retargeted deps
        # log) and is path-tainted anyway; both links follow.
        self.assertEqual(len(edges), 4, edges)
        run([NINJA, "-C", str(target / "build")])
        r = run(["ctest", "--test-dir", str(target / "build")])
        self.assertIn("100% tests passed", r.stdout)
        self.assert_donor_untouched()

    def test_changed_cmakelists_is_reported_as_a_pending_cmake_rerun(self):
        target = self.worktree("rerun")
        (target / "CMakeLists.txt").write_text(FIXTURE["CMakeLists.txt"] + "# changed\n")
        run(["git", "commit", "-qam", "touch cmake"], cwd=target)
        rc, receipt = self.seed(target)
        self.assertEqual(rc, 0, receipt)
        self.assertEqual(receipt["cmake_rerun_pending"], [str(target / "CMakeLists.txt")])
        # The unchanged case must report nothing, or the flag means nothing.
        same = self.worktree("rerun-control")
        rc, receipt = self.seed(same)
        self.assertEqual(rc, 0, receipt)
        self.assertEqual(receipt["cmake_rerun_pending"], [])
        run([NINJA, "-C", str(target / "build")])
        self.assert_donor_untouched()

    def test_unchanged_sources_get_the_donors_mtime_and_changed_do_not(self):
        target = self.worktree("mtimes")
        (target / "main.cpp").write_text(FIXTURE["main.cpp"] + "// changed\n")
        run(["git", "commit", "-qam", "touch main"], cwd=target)
        rc, receipt = self.seed(target)
        self.assertEqual(rc, 0, receipt)
        self.assertEqual(os.stat(target / "a" / "lib.cpp").st_mtime_ns,
                         os.stat(self.donor / "a" / "lib.cpp").st_mtime_ns)
        self.assertGreater(os.stat(target / "main.cpp").st_mtime_ns,
                           os.stat(self.donor / "main.cpp").st_mtime_ns)
        # git must still see a clean tree after the mtime rewrite.
        self.assertEqual(run(["git", "status", "--porcelain"], cwd=target).stdout, "")

    def test_dirty_donor_source_is_not_trusted(self):
        # A donor whose working tree differs from HEAD built its objects from
        # content the target does not have; that file must stay "new".
        target = self.worktree("dirty-donor")
        lib = self.donor / "a" / "lib.cpp"
        original = lib.read_text()
        lib.write_text(original + "// local edit\n")
        try:
            rc, receipt = self.seed(target)
            self.assertEqual(rc, 0, receipt)
            self.assertGreater(os.stat(target / "a" / "lib.cpp").st_mtime_ns,
                               os.stat(self.donor / "build" / "CMakeFiles" / "lib.dir" /
                                       "a" / "lib.cpp.o").st_mtime_ns)
            self.assertTrue(any("lib.cpp.o" in e for e in dirty_edges(target / "build")))
        finally:
            lib.write_text(original)
            os.utime(lib, ns=(self.donor_audit["a/lib.cpp"][1], self.donor_audit["a/lib.cpp"][1]))

    def test_auto_picks_the_closest_eligible_worktree(self):
        far = self.worktree("far")
        for i in range(3):
            (far / f"f{i}.txt").write_text("x")
            run(["git", "add", "."], cwd=far)
            run(["git", "commit", "-qm", f"far {i}"], cwd=far)
        run([CMAKE, "-S", str(far), "-B", str(far / "build"), "-G", "Ninja"])
        run([NINJA, "-C", str(far / "build")])
        target = self.worktree("auto")
        rc, receipt = self.seed(target, donor="auto")
        self.assertEqual(rc, 0, receipt)
        self.assertEqual(receipt["donor_build"], str(self.donor / "build"))

    # -- refusals ---------------------------------------------------------------

    def test_refuses_when_target_build_exists(self):
        target = self.worktree("exists")
        (target / "build").mkdir()
        rc, out = self.seed(target)
        self.assertEqual(rc, sbd.UNSUPPORTED, out)
        self.assertEqual(sorted(os.listdir(target / "build")), [])

    def test_refuses_a_makefiles_donor(self):
        mk = self.worktree("makefiles")
        run([CMAKE, "-S", str(mk), "-B", str(mk / "build"), "-G", "Unix Makefiles"])
        target = self.worktree("from-makefiles")
        rc, out = self.seed(target, donor=str(mk))
        self.assertEqual(rc, sbd.UNSUPPORTED, out)
        self.assertIn("not Ninja", out["stderr"])
        self.assertFalse((target / "build").exists())

    def test_refuses_a_configured_but_unbuilt_donor(self):
        cfg = self.worktree("configured-only")
        run([CMAKE, "-S", str(cfg), "-B", str(cfg / "build"), "-G", "Ninja"])
        target = self.worktree("from-configured-only")
        rc, out = self.seed(target, donor=str(cfg))
        self.assertEqual(rc, sbd.UNSUPPORTED, out)
        self.assertIn("never built", out["stderr"])
        self.assertFalse((target / "build").exists())

    def test_refuses_a_build_type_mismatch(self):
        target = self.worktree("debug-wanted")
        rc, out = self.seed(target, "--build-type", "Debug")
        self.assertEqual(rc, sbd.UNSUPPORTED, out)
        self.assertIn("build type", out["stderr"])
        self.assertFalse((target / "build").exists())

    def test_refuses_cross_volume(self):
        other = Path(tempfile.mkdtemp(prefix="seed-xvol-"))
        try:
            if os.stat(other).st_dev == os.stat(self.tmp).st_dev:
                self.skipTest("no second volume to test against")
            target = other / "target"
            target.mkdir()
            run(["git", "init", "-q"], cwd=target)
            rc, out = self.seed(target)
            self.assertEqual(rc, sbd.UNSUPPORTED, out)
            self.assertIn("different volume", out["stderr"])
            self.assertFalse((target / "build").exists())
        finally:
            shutil.rmtree(other, ignore_errors=True)

    def test_refuses_a_busy_donor(self):
        target = self.worktree("busy")
        proc = subprocess.Popen(["sleep", "30"], cwd=str(self.donor / "build"))
        try:
            rc, out = self.seed(target)
            self.assertEqual(rc, sbd.UNSUPPORTED, out)
            self.assertIn("live process", out["stderr"])
        finally:
            proc.kill()
            proc.wait()

    def test_dry_run_creates_nothing(self):
        target = self.worktree("dry")
        rc, receipt = self.seed(target, "--dry-run")
        self.assertEqual(rc, 0, receipt)
        self.assertTrue(receipt["dry_run"])
        self.assertFalse((target / "build").exists())


if __name__ == "__main__":
    unittest.main()
