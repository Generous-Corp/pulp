#!/usr/bin/env python3
"""Contract tests for run-auval-component.sh's GUI worker."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HELPER = Path(__file__).with_name("run-auval-component.sh")


class AuvalWorkerTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.fake_auval = self.root / "auval"
        self.fake_auval.write_text(
            """#!/usr/bin/env bash
if [[ $1 == -a ]]; then
  if [[ -n ${PULP_FAKE_AUVAL_COUNT_FILE:-} ]]; then
    count=0
    [[ ! -f $PULP_FAKE_AUVAL_COUNT_FILE ]] ||
      count=$(<"$PULP_FAKE_AUVAL_COUNT_FILE")
    count=$((count + 1))
    printf '%s\n' "$count" >"$PULP_FAKE_AUVAL_COUNT_FILE"
    (( count > 1 )) || exit 0
  fi
  echo 'aufx PEfx Pulp  -  Pulp: PulpEffect'
  exit 0
fi
echo 'AU VALIDATION SUCCEEDED'
""",
            encoding="utf-8",
        )
        self.fake_auval.chmod(0o755)

    def tearDown(self):
        self.tempdir.cleanup()

    def run_worker(self, subtype, extra_env=None):
        inventory = self.root / f"{subtype}.inventory"
        validation = self.root / f"{subtype}.validation"
        status = self.root / f"{subtype}.status"
        env = os.environ.copy()
        env["PULP_AUVAL_BIN"] = str(self.fake_auval)
        env["PULP_AU_DISCOVERY_DEADLINE_SECONDS"] = "0"
        if extra_env:
            env.update(extra_env)
        result = subprocess.run(
            [
                "bash",
                str(HELPER),
                "--gui-worker",
                "aufx",
                subtype,
                "Pulp",
                str(inventory),
                str(validation),
                str(status),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=env,
            timeout=40,
        )
        return result, inventory, validation, status

    def test_matching_inventory_runs_validation(self):
        result, inventory, validation, status = self.run_worker("PEfx")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("aufx PEfx Pulp", inventory.read_text(encoding="utf-8"))
        self.assertIn(
            "AU VALIDATION SUCCEEDED", validation.read_text(encoding="utf-8")
        )
        self.assertEqual(status.read_text(encoding="utf-8").strip(), "0")

    def test_wrong_tuple_fails_before_validation(self):
        result, _inventory, validation, status = self.run_worker("BADx")
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "AU inventory did not contain: aufx BADx Pulp",
            validation.read_text(encoding="utf-8"),
        )
        self.assertEqual(status.read_text(encoding="utf-8").strip(), "1")

    def test_inventory_retries_until_component_appears(self):
        count_file = self.root / "inventory-count"
        result, _inventory, validation, status = self.run_worker(
            "PEfx",
            {
                # Keep the helper's production-style wall-clock deadline well
                # above transient scheduler stalls on saturated CI hosts. The
                # subprocess timeout still bounds a broken retry path.
                "PULP_AU_DISCOVERY_DEADLINE_SECONDS": "30",
                "PULP_AU_DISCOVERY_POLL_SECONDS": "0.05",
                "PULP_FAKE_AUVAL_COUNT_FILE": str(count_file),
            },
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(count_file.read_text(encoding="utf-8").strip(), "2")
        self.assertIn(
            "AU VALIDATION SUCCEEDED", validation.read_text(encoding="utf-8")
        )
        self.assertEqual(status.read_text(encoding="utf-8").strip(), "0")




class AuvalExecFailedPredicateTests(unittest.TestCase):
    """The launchd-exec predicate must fire ONLY on a genuine exec failure.

    A false positive would turn a real validation failure into a skip, which is
    the whole risk of reporting a skip at all — so both directions are pinned.
    """

    LIB = Path(__file__).parent / "lib" / "auval-exec-check.sh"
    SCRIPT = "/Volumes/Work/pulp/tools/ci/run-auval-component.sh"

    def predicate(self, stderr_text):
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "launchd.stderr"
            log.write_text(stderr_text, encoding="utf-8")
            result = subprocess.run(
                [
                    "bash",
                    "-c",
                    f'source "{self.LIB}"; '
                    f'auval_worker_exec_failed "{log}" "{self.SCRIPT}"',
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=20,
            )
            return result.returncode == 0

    def test_permission_denied_on_the_worker_is_an_exec_failure(self):
        # The real signature: a checkout launchd is not allowed to read.
        self.assertTrue(
            self.predicate(f"/bin/bash: {self.SCRIPT}: Operation not permitted\n")
        )

    def test_missing_worker_is_an_exec_failure(self):
        self.assertTrue(
            self.predicate(f"/bin/bash: {self.SCRIPT}: No such file or directory\n")
        )

    def test_empty_stderr_is_not_an_exec_failure(self):
        self.assertFalse(self.predicate(""))

    def test_validation_noise_is_not_an_exec_failure(self):
        # A plug-in failing validation must never be reported as a skip.
        self.assertFalse(
            self.predicate(
                "AU VALIDATION FAILED\n"
                "* * FAIL\n"
                "ERROR: initialization failed\n"
            )
        )

    def test_permission_error_naming_another_path_is_not_an_exec_failure(self):
        # Requires BOTH the worker path and an exec-failure message, so an
        # unrelated permission complaint from the plug-in does not qualify.
        self.assertFalse(
            self.predicate(
                "/bin/bash: /some/other/tool: Operation not permitted\n"
            )
        )


class AuvalComponentIdentityTests(unittest.TestCase):
    LIB = Path(__file__).parent / "lib" / "auval-component-identity.sh"

    def isolated_id(self, source_id, run_token):
        return subprocess.run(
            [
                "bash",
                "-c",
                'source "$1"; auval_isolated_bundle_id "$2" "$3"',
                "bash",
                str(self.LIB),
                source_id,
                run_token,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def test_appends_invocation_specific_identity(self):
        result = self.isolated_id("com.pulp.spectr.au", "Spec-123-456")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip(),
            "com.pulp.spectr.au.pulp-auvaltest.Spec-123-456",
        )

    def test_rejects_empty_source_identity(self):
        result = self.isolated_id("", "Spec-123-456")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")

    def test_rejects_unsafe_run_token(self):
        result = self.isolated_id("com.pulp.spectr.au", "Spec/123")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")

if __name__ == "__main__":
    unittest.main()
