#!/usr/bin/env python3
"""gates.sh configures a build to check test/ctest_script_inputs.json.

Each test builds a throwaway repository holding a copy of gates.sh, the
script-inputs lane and its two library modules, stubs for the gate scripts
gates.sh refuses to start without, and one script-driven test. `cmake` and
`ctest` are fakes on PATH: the fake cmake records its argv and writes the
files a configured Ninja build has; the fake ctest prints a json-v1 inventory.
PYTHON is a wrapper that runs the real interpreter for the lane under test and
exits 0 for every other gate, so the verdict read back is this lane's alone.

What must hold:
- a diff touching a declared input with no configured build configures
  build-gate (configure only) and runs the check;
- a diff touching no declared input does not configure;
- a current configured build is reused, not reconfigured;
- PULP_GATES_NO_CONFIGURE=1 does not configure and says NOT CHECKED loudly;
- drift fails gates.sh and prints the exact `--write` command.

Run:
    python3 tools/scripts/test_gates_script_inputs.py
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
COPIED = [
    "tools/scripts/gates.sh",
    "tools/scripts/gates_script_inputs.py",
    "tools/scripts/script_test_inputs.py",
    "tools/ci/source_selftests.py",
]
STUBS = ["tools/scripts/version_bump_check.py", "tools/scripts/skill_sync_check.py"]

FAKE_CMAKE = r"""#!/bin/sh
echo "$*" >> "$FAKE_CMAKE_LOG"
dir=""
while [ $# -gt 0 ]; do
    [ "$1" = "-B" ] && { dir="$2"; shift; }
    shift
done
mkdir -p "$dir"
printf 'CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Release\n' > "$dir/CMakeCache.txt"
echo "# fake" > "$dir/CTestTestfile.cmake"
"""

FAKE_CTEST = r"""#!/bin/sh
sed "s#@ROOT@#$FAKE_ROOT#g" "$FAKE_CTEST_INVENTORY"
"""

# Runs the real interpreter for the lane under test; every other gate passes.
FAKE_PYTHON = """#!/bin/sh
case "$1" in
    *gates_script_inputs.py|*script_test_inputs.py) exec "{real}" "$@" ;;
esac
exit 0
"""


def git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)


class GatesScriptInputsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="gates-script-inputs-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.root = self.tmp / "repo"
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        for rel in COPIED:
            dst = self.root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, dst)
        for rel in STUBS:
            (self.root / rel).write_text("import sys\nsys.exit(0)\n", encoding="utf-8")
        (self.root / "tools/scripts/versioning.json").write_text("{}\n", encoding="utf-8")
        self.write("tools/scripts/test_alpha.py", "print('alpha')\n")
        self.write("tools/scripts/alpha_lib.py", "X = 1\n")
        self.write("test/CMakeLists.txt", "# registrations\n")
        self.write("README.md", "readme\n")
        inventory = {"tests": [{"name": "alpha", "properties": [],
                                "command": ["/usr/bin/python3", "@ROOT@/tools/scripts/test_alpha.py"]}]}
        self.inventory = self.tmp / "inventory.json"
        self.inventory.write_text(json.dumps(inventory), encoding="utf-8")
        self.cmake_log = self.tmp / "cmake.log"
        for name, text in (("cmake", FAKE_CMAKE), ("ctest", FAKE_CTEST),
                           ("python", FAKE_PYTHON.format(real=sys.executable))):
            p = self.bin / name
            p.write_text(text, encoding="utf-8")
            p.chmod(0o755)
        git(self.root, "init", "-q", "-b", "main")
        git(self.root, "config", "user.email", "t@example.com")
        git(self.root, "config", "user.name", "t")
        # The base carries a list regenerated the right way: from a configure.
        scratch = self.tmp / "scratch-build"
        scratch.mkdir()
        subprocess.run([sys.executable, str(self.root / "tools/scripts/script_test_inputs.py"),
                        "--repo-root", str(self.root), "--build-dir", str(scratch), "--write"],
                       check=True, capture_output=True, env=self.env())
        git(self.root, "add", ".")
        git(self.root, "commit", "-q", "-m", "base")
        git(self.root, "checkout", "-q", "-b", "topic")

    def write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def commit(self, msg: str = "change") -> None:
        git(self.root, "add", ".")
        git(self.root, "commit", "-q", "-m", msg)

    def env(self, **extra: str) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("PULP_GATES", "PULP_SCRIPT_INPUTS", "GITHUB_"))}
        env.update(PATH=f"{self.bin}{os.pathsep}{env.get('PATH', '')}", FAKE_ROOT=str(self.root),
                   FAKE_CTEST_INVENTORY=str(self.inventory), FAKE_CMAKE_LOG=str(self.cmake_log),
                   PYTHON=str(self.bin / "python"), PULP_GATES_SETUP="",
                   PULP_SKIP_SOURCE_SELFTESTS="1")
        env.update(extra)
        return env

    def gates(self, **extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", str(self.root / "tools/scripts/gates.sh"), "main"], cwd=self.root,
                              env=self.env(**extra), capture_output=True, text=True, timeout=300)

    def configures(self) -> list[str]:
        return self.cmake_log.read_text().splitlines() if self.cmake_log.exists() else []

    def test_touched_input_without_a_build_configures_build_gate_and_checks(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates()
        self.assertEqual(r.returncode, 0, r.stderr)
        calls = self.configures()
        self.assertEqual(len(calls), 1, r.stderr)
        self.assertIn(f"-B {self.root.resolve() / 'build-gate'}", calls[0])
        self.assertIn("-DPULP_BUILD_EXAMPLES=OFF", calls[0])
        self.assertNotIn("--build", calls[0])  # configure only, never a compile
        self.assertIn("script-test-inputs: OK", r.stderr)
        self.assertNotIn("[script-inputs] NOT CHECKED", r.stderr)

    def test_untouched_inputs_do_not_configure(self) -> None:
        self.write("README.md", "edited\n")
        self.commit()
        r = self.gates()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.configures(), [])
        self.assertIn("nothing to check", r.stderr)

    def test_a_current_configured_build_is_reused(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        subprocess.run([str(self.bin / "cmake"), "-S", str(self.root), "-B", str(self.root / "build-gate")],
                       check=True, env=self.env(FAKE_CMAKE_LOG=str(self.tmp / "pre.log")))
        r = self.gates()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.configures(), [])
        self.assertIn("script-test-inputs: OK", r.stderr)

    def test_opt_out_keeps_not_checked_and_says_so_loudly(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates(PULP_GATES_NO_CONFIGURE="1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.configures(), [])
        self.assertIn("!!! PULP_GATES_NO_CONFIGURE=1", r.stderr)
        self.assertIn("[script-inputs] NOT CHECKED locally: script-test-inputs-drift", r.stderr)
        self.assertIn("PASSED WITH 1 NOT CHECKED", r.stderr)

    def test_hand_edited_list_fails_and_prints_the_write_command(self) -> None:
        self.write("tools/scripts/test_alpha.py", "import alpha_lib\nprint('alpha')\n")
        self.commit("add an import without regenerating the list")
        r = self.gates()
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(len(self.configures()), 1)
        self.assertIn("stale entry: alpha", r.stderr)
        self.assertIn("python3 tools/scripts/script_test_inputs.py --build-dir build-gate --write",
                      r.stderr)


class TouchReasonsTests(unittest.TestCase):
    """The configure decision itself, without gates.sh."""

    def setUp(self) -> None:
        sys.path.insert(0, str(HERE))
        import script_test_inputs
        self.sti = script_test_inputs
        self.tmp = Path(tempfile.mkdtemp(prefix="touch-reasons-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        root = self.root = self.tmp
        for rel, text in {"tools/lib/a.py": "x\n", "tools/lib/data.json": "{}\n",
                          "tools/test_a.py": "x\n", "core/x.cpp": "x\n",
                          "core/CMakeLists.txt": "add_library(x)\n",
                          "tools/CMakeLists.txt": "add_test(NAME a COMMAND a)\n"}.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        lst = {"tests": {"a": {"entry": "tools/test_a.py", "inputs": ["tools/lib", "tools/test_a.py"],
                               "kind": "python"}}}
        (root / "test").mkdir()
        (root / "test/ctest_script_inputs.json").write_text(json.dumps(lst))
        git(root, "init", "-q", "-b", "main")
        git(root, "config", "user.email", "t@example.com")
        git(root, "config", "user.name", "t")
        git(root, "add", ".")
        git(root, "commit", "-q", "-m", "base")

    def reasons_after(self, rel: str) -> list[str]:
        (self.root / rel).write_text("changed\n" + ("add_test(\n" if rel == "tools/CMakeLists.txt" else ""))
        return self.sti.touch_reasons(self.root, "main")

    def test_each_path_class(self) -> None:
        self.assertTrue(self.reasons_after("tools/test_a.py"))            # declared entry (uncommitted)
        git(self.root, "checkout", "-q", ".")
        self.assertTrue(self.reasons_after("tools/lib/a.py"))             # script under an input dir
        git(self.root, "checkout", "-q", ".")
        self.assertEqual(self.reasons_after("tools/lib/data.json"), [])   # data under an input dir
        git(self.root, "checkout", "-q", ".")
        self.assertEqual(self.reasons_after("core/x.cpp"), [])
        git(self.root, "checkout", "-q", ".")
        self.assertEqual(self.reasons_after("core/CMakeLists.txt"), [])   # registers no tests
        git(self.root, "checkout", "-q", ".")
        self.assertTrue(self.reasons_after("tools/CMakeLists.txt"))       # registers tests
        git(self.root, "checkout", "-q", ".")
        self.assertTrue(self.reasons_after("test/ctest_script_inputs.json"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
