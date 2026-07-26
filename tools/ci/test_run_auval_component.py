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


if __name__ == "__main__":
    unittest.main()
