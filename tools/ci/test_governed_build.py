#!/usr/bin/env python3
"""Tests for tools/ci/governed-build.sh.

The wrapper exists so Shipyard's `local` mac backend — which runs the build
string directly on the host, bypassing the pulp CLI's lease integration —
cannot oversubscribe a shared Mac. Its load-bearing contract is what it does
when the lease store says NO: a denial reports real contention, so the only
safe responses are a smaller lease sized from the store's own reported
capacity, or a leaseless build at a conservative floor. Falling back to the
tier-0 bound is not safe — on a big-RAM host the memory axis never binds and
tier-0 degrades to the full core count, i.e. the denial would be answered by
running wider than the request that was just refused.

These tests drive the script with a stub `tartci` on PATH, so they need no
compile, no real lease store, and no particular host size. The stub is written
once and steered by env vars: some hosts security-scan a newly written
executable on first exec (seconds), so a per-test stub file would pay that
repeatedly.

Run:  python3 tools/ci/test_governed_build.py
"""
from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("governed-build.sh")

# Deliberately unlike any plausible core count so an assertion can tell a
# granted lease size apart from a host-derived one.
PROFILE_JOBS = 12
FREE_CORES = 6
FLOOR = 2

# A stub tartci steered entirely by env vars:
#   STUB_PROFILE_JOBS  PULP_BUILD_JOBS to report; unset → host-profile fails
#   STUB_FREE_CORES    non_gate_available_cores to report; unset → status fails
#   STUB_MAX_GRANT     grant `leases acquire` iff --cores <= this; unset → deny
#   STUB_FAIL_ALL      any value → every subcommand fails (a wedged store)
STUB = r"""#!/usr/bin/env bash
[ -n "${STUB_FAIL_ALL:-}" ] && exit 3
if [ "$1" = "host-profile" ]; then
  [ -n "${STUB_PROFILE_JOBS:-}" ] || exit 1
  echo "TARTCI_AGENT_QOS=normal"
  echo "PULP_BUILD_JOBS=${STUB_PROFILE_JOBS}"
  exit 0
fi
if [ "$1" = "leases" ] && [ "$2" = "status" ]; then
  [ -n "${STUB_FREE_CORES:-}" ] || exit 1
  printf '{"capacity":{"non_gate_available_cores":%s,' "${STUB_FREE_CORES}"
  printf '"non_gate_limit_cores":12,"total_cores":26},"leases":[],"schema":2}\n'
  exit 0
fi
if [ "$1" = "leases" ] && [ "$2" = "acquire" ]; then
  [ -n "${STUB_MAX_GRANT:-}" ] || exit 1
  cores=""
  while [ "$#" -gt 0 ]; do
    [ "$1" = "--cores" ] && cores="$2"
    shift
  done
  [ -n "$cores" ] || exit 1
  [ "$cores" -le "${STUB_MAX_GRANT}" ] && exit 0
  exit 1
fi
exit 0
"""


class GovernedBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.TemporaryDirectory()
        cls.bindir = Path(cls._tmp.name)
        stub = cls.bindir / "tartci"
        stub.write_text(STUB)
        stub.chmod(0o755)
        # Pay any first-exec security scan once, outside the timed assertions.
        subprocess.run([str(stub), "host-profile"], capture_output=True,
                       check=False)

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def _run(self, **stub_env: str) -> subprocess.CompletedProcess:
        """Run the wrapper over a command that reports the granted parallelism."""
        env = {
            **os.environ,
            "PATH": f"{self.bindir}{os.pathsep}{os.environ['PATH']}",
            "PULP_GOVERNED_BUILD_MIN_JOBS": str(FLOOR),
        }
        for k in ("PULP_TARTCI_BIN", "PULP_TARTCI_LEASES", "STUB_PROFILE_JOBS",
                  "STUB_FREE_CORES", "STUB_MAX_GRANT", "STUB_FAIL_ALL"):
            env.pop(k, None)
        env.update(stub_env)
        return subprocess.run(
            ["bash", str(SCRIPT), "sh", "-c",
             'echo "JOBS=$CMAKE_BUILD_PARALLEL_LEVEL CTEST=$CTEST_PARALLEL_LEVEL"'],
            capture_output=True, text=True, check=False, env=env,
        )

    def _granted(self, r: subprocess.CompletedProcess) -> int:
        for tok in r.stdout.split():
            if tok.startswith("JOBS="):
                return int(tok.split("=", 1)[1])
        self.fail(f"wrapper never ran the build command\n{r.stdout}\n{r.stderr}")

    # --- the denial path (the reason this wrapper exists) --------------------

    def test_denial_retries_at_available_capacity(self) -> None:
        """Refused at profile size → retry at the cores the store reports free."""
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS),
                      STUB_FREE_CORES=str(FREE_CORES),
                      STUB_MAX_GRANT=str(FREE_CORES))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self._granted(r), FREE_CORES, r.stderr)

    def test_denial_never_exceeds_available_capacity(self) -> None:
        """However big the host, a denial must not widen the build."""
        # Every acquire denied, including the backoff retry (a lost race).
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS),
                      STUB_FREE_CORES=str(FREE_CORES))
        self.assertEqual(r.returncode, 0, r.stderr)
        granted = self._granted(r)
        self.assertLessEqual(
            granted, FREE_CORES,
            f"answered a denial with -j{granted} while only {FREE_CORES} cores "
            f"were free (stderr: {r.stderr})")

    def test_lost_race_falls_back_to_floor(self) -> None:
        """Retry denied too → the floor, not the host's size."""
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS),
                      STUB_FREE_CORES=str(FREE_CORES))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self._granted(r), FLOOR, r.stderr)

    def test_zero_free_cores_uses_floor_not_core_count(self) -> None:
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS), STUB_FREE_CORES="0")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self._granted(r), FLOOR, r.stderr)

    def test_unreported_capacity_uses_floor(self) -> None:
        """An older schema / unhappy store is treated as no capacity."""
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self._granted(r), FLOOR, r.stderr)

    def test_floor_is_configurable(self) -> None:
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS), STUB_FREE_CORES="0",
                      PULP_GOVERNED_BUILD_MIN_JOBS="1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self._granted(r), 1, r.stderr)

    def test_garbage_floor_falls_back_to_a_sane_default(self) -> None:
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS), STUB_FREE_CORES="0",
                      PULP_GOVERNED_BUILD_MIN_JOBS="not-a-number")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self._granted(r), 2, r.stderr)

    # --- never fail the build over the lease store ---------------------------

    def test_wedged_lease_store_never_fails_the_build(self) -> None:
        r = self._run(STUB_FAIL_ALL="1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertGreaterEqual(self._granted(r), 1)

    def test_denial_never_fails_the_build(self) -> None:
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS), STUB_FREE_CORES="0")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    # --- the happy and no-tartci paths ---------------------------------------

    def test_granted_lease_uses_profile_size(self) -> None:
        r = self._run(STUB_PROFILE_JOBS=str(PROFILE_JOBS),
                      STUB_MAX_GRANT=str(PROFILE_JOBS))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self._granted(r), PROFILE_JOBS, r.stderr)

    def test_no_tartci_still_builds_bounded(self) -> None:
        """Plain checkout / build VM: tier-0 bound, still >= 1 and finite."""
        r = self._run(PULP_TARTCI_LEASES="0")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertGreaterEqual(self._granted(r), 1)

    def test_build_failure_propagates(self) -> None:
        r = subprocess.run(
            ["bash", str(SCRIPT), "sh", "-c", "exit 7"],
            capture_output=True, text=True, check=False,
            env={**os.environ, "PULP_TARTCI_LEASES": "0"},
        )
        self.assertEqual(r.returncode, 7, r.stderr)

    # --- script hygiene ------------------------------------------------------

    def test_script_exists_and_executable(self) -> None:
        self.assertTrue(SCRIPT.is_file(), SCRIPT)
        self.assertTrue(os.access(SCRIPT, os.X_OK), f"{SCRIPT} not executable")

    def test_syntax_is_valid(self) -> None:
        r = subprocess.run(["bash", "-n", str(SCRIPT)],
                           capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_no_args_is_a_usage_error(self) -> None:
        r = subprocess.run(["bash", str(SCRIPT)],
                           capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
