#!/usr/bin/env python3
"""configure_only.py: a governed configure in a temporary tree, never a compile.

What must hold:
- the command runs against a configured `build-configure-only.*` directory
  under the checkout, and that directory is gone afterwards, whether the
  command passed, failed, or the configure failed;
- a real CMake configure of a project with an executable target leaves no
  `.ninja_log` and no object file outside CMake's own probe directories, while
  the CTest inventory the generators read is already complete;
- a configure that leaves a target object file, or a command that is a build
  (`cmake --build`, `ninja`, `make`), is refused with exit 3;
- the configure is governed: it runs under the governor as a background
  one-core lease;
- `changed_surface_script_families.py --configure-only` takes this path.

Run:
    python3 tools/scripts/test_configure_only.py
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
import configure_only  # noqa: E402

SCRIPT = HERE / "configure_only.py"

FAKE_CMAKE = r"""#!/bin/sh
echo "$*" >> "$FAKE_CMAKE_LOG"
dir=""
while [ $# -gt 0 ]; do
    [ "$1" = "-B" ] && { dir="$2"; shift; }
    shift
done
[ -n "$FAKE_CMAKE_FAILS" ] && { echo "CMake Error: planted" >&2; exit 1; }
mkdir -p "$dir/CMakeFiles/3.31.0/CompilerIdCXX" "$dir/CMakeFiles/_CMakeLTOTest-C/bin/CMakeFiles/foo.dir"
: > "$dir/CMakeFiles/_CMakeLTOTest-C/bin/CMakeFiles/foo.dir/foo.c.o"
: > "$dir/CMakeFiles/3.31.0/CompilerIdCXX/CMakeCXXCompilerId.o"
echo "# fake" > "$dir/CTestTestfile.cmake"
if [ -n "$FAKE_CMAKE_COMPILES" ]; then
    mkdir -p "$dir/core/CMakeFiles/pulp-core.dir"
    : > "$dir/core/CMakeFiles/pulp-core.dir/a.cpp.o"
fi
if [ -n "$FAKE_CMAKE_NINJA_LOG" ]; then
    : > "$dir/.ninja_log"
fi
"""

FAKE_GOVERNOR = r"""#!/bin/sh
echo "class=$PULP_BUILD_CLASS jobs=$PULP_BUILD_JOBS" >> "$FAKE_GOVERNOR_LOG"
exec "$@"
"""


class ConfigureOnlyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="configure-only-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.root = self.tmp / "repo"
        self.root.mkdir()
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        cmake = self.bin / "cmake"
        cmake.write_text(FAKE_CMAKE, encoding="utf-8")
        cmake.chmod(0o755)
        self.governor = self.tmp / "governed-build.sh"
        self.governor.write_text(FAKE_GOVERNOR, encoding="utf-8")
        self.governor.chmod(0o755)
        self.cmake_log = self.tmp / "cmake.log"
        self.governor_log = self.tmp / "governor.log"

    def env(self, **extra: str) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items() if not k.startswith(("PULP_", "FAKE_"))}
        env.update(CMAKE=str(self.bin / "cmake"), FAKE_CMAKE_LOG=str(self.cmake_log),
                   FAKE_GOVERNOR_LOG=str(self.governor_log), PULP_GATES_SETUP="",
                   PULP_CONFIGURE_ONLY_GOVERNOR=str(self.governor), PULP_CONFIGURE_ONLY_NPM="0")
        env.update(extra)
        return env

    def run_tool(self, *command: str, **extra: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(self.root), "--", *command],
                              env=self.env(**extra), capture_output=True, text=True, encoding="utf-8",
                              timeout=300)

    def temp_trees(self) -> list[str]:
        return sorted(p.name for p in self.root.glob("build-configure-only.*"))

    def test_command_sees_the_configured_tree_which_is_removed_afterwards(self) -> None:
        seen = self.tmp / "seen.txt"
        r = self.run_tool(sys.executable, "-c",
                          "import pathlib, sys; b = pathlib.Path(sys.argv[1]); "
                          "pathlib.Path(sys.argv[2]).write_text(str(b) + '\\n' + "
                          "str((b / 'CTestTestfile.cmake').is_file()) + '\\n' + "
                          "str((b / '.cmake/api/v1/query/codemodel-v2').is_file()))",
                          "{build}", str(seen))
        self.assertEqual(r.returncode, 0, r.stderr)
        build, testfile, query = seen.read_text(encoding="utf-8").splitlines()
        self.assertEqual(Path(build).parent, self.root.resolve())
        self.assertTrue(Path(build).name.startswith("build-configure-only."))
        self.assertEqual((testfile, query), ("True", "True"))
        self.assertEqual(self.temp_trees(), [])
        call = self.cmake_log.read_text(encoding="utf-8")
        self.assertIn("-G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=OFF",
                      call)
        self.assertNotIn("--build", call)

    def test_the_configure_is_governed_as_one_background_core(self) -> None:
        r = self.run_tool("true")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.governor_log.read_text(encoding="utf-8").strip(), "class=background jobs=1")

    def test_a_failing_command_keeps_its_exit_code_and_still_cleans_up(self) -> None:
        r = self.run_tool(sys.executable, "-c", "raise SystemExit(5)")
        self.assertEqual(r.returncode, 5, r.stderr)
        self.assertEqual(self.temp_trees(), [])

    def test_a_failed_configure_exits_2_without_running_the_command(self) -> None:
        marker = self.tmp / "ran"
        r = self.run_tool("touch", str(marker), FAKE_CMAKE_FAILS="1")
        self.assertEqual(r.returncode, configure_only.CONFIGURE_FAILED, r.stderr)
        self.assertIn("CMake Error: planted", r.stderr)
        self.assertFalse(marker.exists())
        self.assertEqual(self.temp_trees(), [])

    def test_a_configure_that_compiles_is_refused(self) -> None:
        for planted, expected in (({"FAKE_CMAKE_COMPILES": "1"}, "core/CMakeFiles/pulp-core.dir/a.cpp.o"),
                                  ({"FAKE_CMAKE_NINJA_LOG": "1"}, ".ninja_log")):
            with self.subTest(expected=expected):
                marker = self.tmp / "ran"
                r = self.run_tool("touch", str(marker), **planted)
                self.assertEqual(r.returncode, configure_only.COMPILED, r.stderr)
                self.assertIn(expected, r.stderr)
                self.assertFalse(marker.exists())
                self.assertEqual(self.temp_trees(), [])

    def test_probe_objects_alone_are_not_a_compile(self) -> None:
        # Control for the refusal above: the fake writes CMake's own probe
        # objects on every run, and they do not count.
        r = self.run_tool("true")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_a_build_command_is_refused_before_configuring(self) -> None:
        for command in (["cmake", "--build", "{build}"], ["/usr/bin/ninja", "-C", "{build}"],
                        ["make", "-C", "{build}"]):
            with self.subTest(command=command):
                r = self.run_tool(*command)
                self.assertEqual(r.returncode, configure_only.COMPILED, r.stderr)
                self.assertIn("REFUSED", r.stderr)
                self.assertFalse(self.cmake_log.exists())
        self.assertFalse(configure_only.is_build_command(["cmake", "-S", ".", "-B", "b"]))
        self.assertFalse(configure_only.is_build_command(["python3", "x.py", "--build-dir", "b"]))

    def test_the_jsx_runtime_install_runs_only_when_its_modules_are_missing(self) -> None:
        jsx = self.root / configure_only.JSX_RUNTIME
        jsx.mkdir(parents=True)
        (jsx / "package-lock.json").write_text("{}\n", encoding="utf-8")
        npm_log = self.tmp / "npm.log"
        npm = self.bin / "npm"
        npm.write_text(f'#!/bin/sh\necho "$*" >> "{npm_log}"\n', encoding="utf-8")
        npm.chmod(0o755)
        path = f"{self.bin}{os.pathsep}{os.environ.get('PATH', '')}"
        r = self.run_tool("true", PULP_CONFIGURE_ONLY_NPM="1", PATH=path)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"ci --prefix {jsx.resolve()}", npm_log.read_text(encoding="utf-8"))
        for module in configure_only.JSX_RUNTIME_MODULES:
            (jsx / "node_modules" / module).parent.mkdir(parents=True, exist_ok=True)
            (jsx / "node_modules" / module).write_text("{}\n", encoding="utf-8")
        npm_log.unlink()
        r = self.run_tool("true", PULP_CONFIGURE_ONLY_NPM="1", PATH=path)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(npm_log.exists())


@unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "cmake and ninja are required")
class RealConfigureTests(unittest.TestCase):
    """A real CMake configure of a project with a compiled target and a test."""

    def test_a_real_configure_compiles_no_target_and_lists_the_tests(self) -> None:
        tmp = Path(tempfile.mkdtemp(prefix="configure-only-real-"))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        (tmp / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(probe CXX)\n"
            "option(PULP_BUILD_TESTS \"\" OFF)\n"
            "include(CheckIPOSupported)\n"
            "check_ipo_supported(RESULT ipo OUTPUT ipo_out LANGUAGES CXX)\n"
            "add_executable(probe-bin main.cpp)\n"
            "enable_testing()\n"
            "add_test(NAME probe-test COMMAND probe-bin)\n", encoding="utf-8")
        (tmp / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        checker = tmp / "check.py"
        checker.write_text(
            "import json, pathlib, subprocess, sys\n"
            f"sys.path.insert(0, {str(HERE)!r})\n"
            "import configure_only\n"
            "b = pathlib.Path(sys.argv[1])\n"
            "inv = json.loads(subprocess.run(['ctest', '--test-dir', str(b), '--show-only=json-v1'],"
            " capture_output=True, text=True, check=True).stdout)\n"
            "reply = list((b / '.cmake/api/v1/reply').glob('codemodel-v2-*.json'))\n"
            "pathlib.Path(sys.argv[2]).write_text(json.dumps({\n"
            "  'tests': [t['name'] for t in inv['tests']],\n"
            "  'codemodel': bool(reply),\n"
            "  'compiled': configure_only.compile_evidence(b),\n"
            "  'objects_anywhere': sorted(str(p.relative_to(b)) for p in b.rglob('*.o')),\n"
            "}))\n", encoding="utf-8")
        out = tmp / "out.json"
        env = {k: v for k, v in os.environ.items() if not k.startswith("PULP_")}
        env.update(PULP_GATES_SETUP="", PULP_CONFIGURE_ONLY_GOVERNOR="", PULP_CONFIGURE_ONLY_NPM="0")
        r = subprocess.run([sys.executable, str(SCRIPT), "--repo-root", str(tmp), "--",
                            sys.executable, str(checker), "{build}", str(out)],
                           env=env, capture_output=True, text=True, encoding="utf-8", timeout=600)
        self.assertEqual(r.returncode, 0, r.stderr)
        facts = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(facts["tests"], ["probe-test"])
        self.assertTrue(facts["codemodel"])
        self.assertEqual(facts["compiled"], [])
        # No object for the project's own target exists: only CMake's probes compiled.
        self.assertFalse([p for p in facts["objects_anywhere"] if "probe-bin.dir" in p],
                         facts["objects_anywhere"])
        self.assertEqual(sorted(p.name for p in tmp.glob("build-configure-only.*")), [])


class FamiliesGeneratorFlagTests(unittest.TestCase):
    def test_configure_only_flag_routes_through_the_temporary_tree(self) -> None:
        import changed_surface_script_families as generator  # noqa: PLC0415
        seen: list[Path] = []

        class Configured:
            def __init__(self, root: Path) -> None:
                self.build = root / "build-configure-only.test"

            def __enter__(self):
                return self.build, "", 0

            def __exit__(self, *exc) -> None:
                return None

        original_configured = configure_only.configured
        original_run = generator.run_against
        configure_only.configured = Configured
        generator.run_against = lambda root, build, args: seen.append(build) or 0
        try:
            rc = generator.main(["--configure-only", "--write"])
        finally:
            configure_only.configured = original_configured
            generator.run_against = original_run
        self.assertEqual(rc, 0)
        self.assertEqual([p.name for p in seen], ["build-configure-only.test"])

    def test_configure_only_and_build_dir_are_exclusive(self) -> None:
        import changed_surface_script_families as generator  # noqa: PLC0415
        with self.assertRaises(SystemExit):
            with open(os.devnull, "w", encoding="utf-8") as sink:
                stderr, sys.stderr = sys.stderr, sink
                try:
                    generator.main(["--configure-only", "--build-dir", "b", "--write"])
                finally:
                    sys.stderr = stderr


if __name__ == "__main__":
    unittest.main(verbosity=2)
