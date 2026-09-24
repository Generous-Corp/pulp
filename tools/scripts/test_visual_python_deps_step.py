#!/usr/bin/env python3
"""The visual-analysis install step resolves the interpreter CMake configured.

The step installs tools/motion/visual/requirements.txt into the interpreter
ctest will launch, so a wrong answer here is silent: the install lands in some
other interpreter, every visual test goes on skipping, and the step reports
success. That makes a structural assertion on the step's text worthless — it
would only restate the extraction it is meant to check. So these cases run the
step's real script out of build.yml against fixture CMake caches, with a stub
interpreter standing in for the resolved one.

The shapes that matter are the two a configure can actually produce:

  _Python3_EXECUTABLE:INTERNAL=...   find_package(Python3 COMPONENTS Interpreter)
                                     writes this and leaves the public name a
                                     normal variable. Every configure that does
                                     not pin an interpreter looks like this,
                                     which is every GitHub Actions leg here.
  Python3_EXECUTABLE:UNINITIALIZED=  a configure that passed
                                     -DPython3_EXECUTABLE, as the local
                                     Shipyard lane does.

Reading only the public name resolves the pinned shape and finds nothing in the
unpinned one, so the step passes local validation and fails every hosted leg.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github/workflows/build.yml"
STEP_NAME = "Install visual-analysis Python dependencies"
REQUIREMENTS = REPO / "tools/motion/visual/requirements.txt"

# Entries a real cache carries that share the prefix but are not the
# interpreter. A fixture that omitted them could not catch a loosened pattern.
DECOYS = "\n".join(
    (
        "_Python3_Compiler_REASON_FAILURE:INTERNAL=",
        "_Python3_Development_REASON_FAILURE:INTERNAL=",
        "_Python3_INTERPRETER_SIGNATURE:INTERNAL=91e9a1eb6550ec9c57aea51c53bd7c63",
        "Python3_NumPy_INCLUDE_DIR:PATH=Python3_NumPy_INCLUDE_DIR-NOTFOUND",
    )
)


def _step_script() -> str:
    """Return the run: body of the install step, as build.yml declares it."""
    workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    for job in workflow["jobs"].values():
        for step in job.get("steps") or []:
            if step.get("name") == STEP_NAME:
                return step["run"]
    raise AssertionError(f"build.yml declares no step named {STEP_NAME!r}")


class VisualPythonDepsStepTest(unittest.TestCase):
    """Run the real step against each cache shape a configure can produce."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.script = _step_script()

    def _run(self, cache_body: str, *, pep668: bool = False,
             satisfied: bool = False):
        """Run the step with a fixture cache. Returns (proc, argv_lines).

        `cache_body` may contain {py}, replaced by the stub interpreter's path.
        With pep668, the stub refuses a plain --user install the way a
        Homebrew/Debian interpreter does, exercising the step's fallback.
        With satisfied, the stub answers the `--dry-run --no-index` probe
        successfully, standing in for a VM that already carries the wheels —
        the case where the step must not reach the network at all.
        """
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            build_dir = tmp_path / "build-fixture"
            build_dir.mkdir()
            argv_log = tmp_path / "argv.log"

            stub = tmp_path / "stub-python3"
            # The step probes with `--dry-run --no-index` before installing.
            # Default: report NOT satisfied, so the install paths below stay
            # exercised. With satisfied=True the probe succeeds and the step
            # must stop there.
            probe = (
                'case " $* " in *" --no-index "*) exit %d ;; esac\n'
                % (0 if satisfied else 1)
            )
            refuse = (
                'case " $* " in *" --break-system-packages "*) exit 0 ;; esac\nexit 1\n'
                if pep668
                else "exit 0\n"
            )
            stub.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/bash
                    printf '%s\\n' "$*" >> {argv_log}
                    """
                )
                + probe
                + refuse,
                encoding="utf-8",
            )
            stub.chmod(0o755)

            (build_dir / "CMakeCache.txt").write_text(
                cache_body.format(py=stub) + "\n", encoding="utf-8"
            )

            proc = subprocess.run(
                ["bash", "-c", self.script],
                cwd=REPO,
                env={**os.environ, "PULP_BUILD_DIR": str(build_dir)},
                capture_output=True,
                text=True,
                timeout=120,
            )
            invocations = (
                argv_log.read_text(encoding="utf-8").splitlines()
                if argv_log.exists()
                else []
            )
            return proc, invocations

    def test_resolves_the_interpreter_find_package_recorded(self) -> None:
        """The unpinned shape every hosted leg produces. The regression case."""
        proc, invocations = self._run(f"{DECOYS}\n_Python3_EXECUTABLE:INTERNAL={{py}}")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(invocations, "the resolved interpreter was never invoked")
        self.assertIn("-m pip install", invocations[0])
        self.assertIn("-r tools/motion/visual/requirements.txt", invocations[0])

    def test_resolves_an_explicitly_pinned_interpreter(self) -> None:
        """The shape a -DPython3_EXECUTABLE configure produces (Shipyard local)."""
        proc, invocations = self._run(
            f"{DECOYS}\nPython3_EXECUTABLE:UNINITIALIZED={{py}}"
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(invocations, "the resolved interpreter was never invoked")

    def test_prefers_the_interpreter_find_package_actually_resolved(self) -> None:
        """A pinned request is a request; the internal entry is the answer.

        find_package validates -DPython3_EXECUTABLE and records what it settled
        on, which is the value ${Python3_EXECUTABLE} carried when test/cmake
        registered these tests — so it is what ctest launches.
        """
        with tempfile.TemporaryDirectory() as tmp:
            stale = Path(tmp) / "interpreter-that-ctest-never-launches"
            stale.write_text("#!/bin/bash\nexit 1\n", encoding="utf-8")
            stale.chmod(0o755)
            proc, invocations = self._run(
                f"Python3_EXECUTABLE:UNINITIALIZED={stale}\n"
                "_Python3_EXECUTABLE:INTERNAL={py}"
            )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(invocations, "the resolved interpreter was never invoked")

    def test_fails_closed_when_the_cache_records_no_interpreter(self) -> None:
        """Absent means absent: never fall through to the shell's python3."""
        proc, invocations = self._run(DECOYS)
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(invocations, [])
        self.assertIn("_Python3_EXECUTABLE", proc.stderr)
        self.assertIn("Python3_EXECUTABLE", proc.stderr)

    def test_falls_back_to_the_pep668_override(self) -> None:
        """Homebrew and Debian refuse a plain --user install."""
        proc, invocations = self._run(
            "_Python3_EXECUTABLE:INTERNAL={py}", pep668=True
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        # Assert the SHAPE of the install attempts, not a raw count: the
        # `--no-index` probe runs first and is not an install.
        installs = [i for i in invocations if "--no-index" not in i]
        self.assertEqual(len(installs), 2, invocations)
        self.assertNotIn("--break-system-packages", installs[0])
        self.assertIn("--break-system-packages", installs[1])

    def test_an_already_satisfied_set_never_reaches_the_index(self) -> None:
        """The poka-yoke: a provisioned VM must not need PyPI to be reachable.

        A required gate that fetches from a third-party service fails whenever
        that service is unreachable from the runner, which on 2026-09-23 took
        out every merge_group batch landing on one host while the same batch
        passed on another.
        """
        proc, invocations = self._run(
            "_Python3_EXECUTABLE:INTERNAL={py}", satisfied=True
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        probes = [i for i in invocations if "--no-index" in i]
        installs = [i for i in invocations if "--no-index" not in i]
        self.assertEqual(len(probes), 1, invocations)
        self.assertEqual(installs, [], "a satisfied set must install nothing")

    def test_the_declared_requirements_file_exists(self) -> None:
        """The step installs by path; a rename must break here, not in CI."""
        self.assertTrue(REQUIREMENTS.is_file(), f"{REQUIREMENTS} is missing")
        self.assertTrue(REQUIREMENTS.read_text(encoding="utf-8").strip())


if __name__ == "__main__":
    if shutil.which("bash") is None:  # pragma: no cover - bash is a step requirement
        raise SystemExit("bash is required to run the step under test")
    unittest.main(verbosity=2)
