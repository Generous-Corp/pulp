#!/usr/bin/env python3
"""The visual-analysis install step resolves the interpreter CMake configured.

The step installs the hash-pinned resolution of
tools/motion/visual/requirements.txt (requirements.lock) into the interpreter
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
import re
import textwrap
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github/workflows/build.yml"
STEP_NAME = "Install visual-analysis Python dependencies"
REQUIREMENTS = REPO / "tools/motion/visual/requirements.txt"
LOCK = REPO / "tools/motion/visual/requirements.lock"

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


def _canonical(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def _version(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in re.findall(r"\d+", text)[:3])


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

    def _run(
        self,
        cache_body: str,
        *,
        pep668: bool = False,
        index_failures: int = 0,
        wheelhouse: str | None = None,
        wheelhouse_ok: bool = True,
    ):
        """Run the step with a fixture cache. Returns (proc, pip_argv_lines).

        `cache_body` may contain {py}, replaced by the stub interpreter's path.
        The stub answers the step's PEP 668 probe per `pep668`, fails the first
        `index_failures` index installs, and fails a --no-index install unless
        `wheelhouse_ok`. `wheelhouse` is "present", "missing" or None (unset).
        """
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            build_dir = tmp_path / "build-fixture"
            build_dir.mkdir()
            argv_log = tmp_path / "argv.log"
            counter = tmp_path / "index-attempts"

            stub = tmp_path / "stub-python3"
            stub.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/bash
                    if [ "$1" = "-c" ]; then exit {0 if pep668 else 1}; fi
                    printf '%s\\n' "$*" >> {argv_log}
                    case " $* " in
                      *" --no-index "*) exit {0 if wheelhouse_ok else 1} ;;
                    esac
                    n=$(( $(cat {counter} 2>/dev/null || echo 0) + 1 ))
                    echo "$n" > {counter}
                    [ "$n" -gt {index_failures} ]
                    """
                ),
                encoding="utf-8",
            )
            stub.chmod(0o755)

            (build_dir / "CMakeCache.txt").write_text(
                cache_body.format(py=stub) + "\n", encoding="utf-8"
            )

            env = {
                **os.environ,
                "PULP_BUILD_DIR": str(build_dir),
                "PULP_PIP_RETRY_DELAY_SECS": "0",
            }
            env.pop("TARTCI_PIP_WHEELHOUSE", None)
            if wheelhouse is not None:
                house = tmp_path / "wheelhouse"
                if wheelhouse == "present":
                    house.mkdir()
                env["TARTCI_PIP_WHEELHOUSE"] = str(house)

            proc = subprocess.run(
                ["bash", "-c", self.script],
                cwd=REPO,
                env=env,
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
        self.assertIn("--require-hashes", invocations[0])
        self.assertIn("-r tools/motion/visual/requirements.lock", invocations[0])

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

    def test_pep668_interpreter_gets_the_override_on_the_first_install(self) -> None:
        """Homebrew and Debian refuse a plain --user install.

        The step probes for EXTERNALLY-MANAGED once instead of retrying every
        install, so an index outage is not paid twice per attempt.
        """
        proc, invocations = self._run(
            "_Python3_EXECUTABLE:INTERNAL={py}", pep668=True
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(invocations), 1, invocations)
        self.assertIn("--break-system-packages", invocations[0])

    def test_unmanaged_interpreter_gets_no_override(self) -> None:
        proc, invocations = self._run("_Python3_EXECUTABLE:INTERNAL={py}")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(invocations), 1, invocations)
        self.assertNotIn("--break-system-packages", invocations[0])

    def test_host_wheelhouse_installs_without_the_index(self) -> None:
        proc, invocations = self._run(
            "_Python3_EXECUTABLE:INTERNAL={py}", wheelhouse="present"
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(invocations), 1, invocations)
        self.assertIn("--no-index", invocations[0])
        self.assertIn("--find-links", invocations[0])
        self.assertIn("--require-hashes", invocations[0])

    def test_unusable_wheelhouse_falls_back_to_the_index(self) -> None:
        proc, invocations = self._run(
            "_Python3_EXECUTABLE:INTERNAL={py}",
            wheelhouse="present",
            wheelhouse_ok=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(invocations), 2, invocations)
        self.assertIn("--no-index", invocations[0])
        self.assertNotIn("--no-index", invocations[1])
        self.assertIn("--require-hashes", invocations[1])
        self.assertIn("falling back to the package index", proc.stdout)

    def test_declared_but_missing_wheelhouse_uses_the_index(self) -> None:
        proc, invocations = self._run(
            "_Python3_EXECUTABLE:INTERNAL={py}", wheelhouse="missing"
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(invocations), 1, invocations)
        self.assertNotIn("--no-index", invocations[0])

    def test_transient_index_failures_are_retried(self) -> None:
        proc, invocations = self._run(
            "_Python3_EXECUTABLE:INTERNAL={py}", index_failures=2
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(invocations), 3, invocations)

    def test_a_persistent_index_failure_fails_the_step(self) -> None:
        proc, invocations = self._run(
            "_Python3_EXECUTABLE:INTERNAL={py}", index_failures=99
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(len(invocations), 3, invocations)
        self.assertIn("after 3 attempts", proc.stderr)

    def test_the_lock_pins_every_declared_requirement(self) -> None:
        """A requirement added to requirements.txt must be regenerated here.

        Otherwise CI would keep installing the old closure and the new
        dependency would be missing from exactly the lane meant to prove it.
        """
        pins: dict[str, str] = {}
        hashed: set[str] = set()
        current = None
        for raw in LOCK.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            match = re.match(r"^([A-Za-z0-9_.-]+)==([^\s\\]+)", line)
            if match:
                current = _canonical(match.group(1))
                pins[current] = match.group(2)
            elif line.startswith("--hash=sha256:") and current:
                hashed.add(current)
        self.assertTrue(pins, f"{LOCK} pins nothing")
        self.assertEqual(set(pins), hashed, "every pin must carry a sha256 hash")
        for raw in REQUIREMENTS.read_text(encoding="utf-8").splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            match = re.match(r"^([A-Za-z0-9_.-]+)\s*>=\s*([0-9.]+)$", line)
            self.assertIsNotNone(match, f"unrecognised requirement {line!r}")
            name, floor = _canonical(match.group(1)), match.group(2)
            self.assertIn(name, pins, f"{name} is declared but not locked")
            self.assertGreaterEqual(
                _version(pins[name]), _version(floor),
                f"{name}=={pins[name]} does not satisfy >={floor}",
            )

    def test_the_declared_requirements_file_exists(self) -> None:
        """The step installs by path; a rename must break here, not in CI."""
        self.assertTrue(REQUIREMENTS.is_file(), f"{REQUIREMENTS} is missing")
        self.assertTrue(REQUIREMENTS.read_text(encoding="utf-8").strip())


if __name__ == "__main__":
    if shutil.which("bash") is None:  # pragma: no cover - bash is a step requirement
        raise SystemExit("bash is required to run the step under test")
    unittest.main(verbosity=2)
