#!/usr/bin/env python3
"""Tests for tools/scripts/check_shipyard_pin.py (D1).

Verifies that the workflow-pin-vs-shipyard.toml drift check exits
correctly. Uses temp directories so the test doesn't depend on the
exact current pin or the exact workflow inventory.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import textwrap
import unittest
import unittest.mock as mock

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "check_shipyard_pin.py"


def run_check_in_fake_repo(tmp: pathlib.Path) -> subprocess.CompletedProcess:
    """Run the script with REPO_ROOT pointed at `tmp` via a small
    wrapper. The script reads paths relative to its own __file__, so
    we re-execute it through a thin shim that overrides those globals
    AFTER the module body has finished initializing them."""
    shim = tmp / "shim.py"
    shim.write_text(textwrap.dedent(f"""
        import pathlib, sys, importlib.util
        sys.path.insert(0, {str(SCRIPT.parent)!r})
        spec = importlib.util.spec_from_file_location("csp", {str(SCRIPT)!r})
        csp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(csp)
        # Now that the module has loaded with its original globals,
        # override them and call main() so it sees the fake repo.
        csp.REPO_ROOT = pathlib.Path({str(tmp)!r})
        csp.PIN_FILE = csp.REPO_ROOT / "tools" / "shipyard.toml"
        csp.WORKFLOWS_DIR = csp.REPO_ROOT / ".github" / "workflows"
        sys.exit(csp.main())
    """).lstrip())
    return subprocess.run(
        [sys.executable, str(shim)],
        capture_output=True, text=True, check=False,
    )


class ShipyardPinCheckTests(unittest.TestCase):
    def _setup_fake_repo(self, tmp: pathlib.Path,
                         pin: str,
                         workflows: dict[str, str]) -> None:
        (tmp / "tools").mkdir(parents=True)
        (tmp / "tools" / "shipyard.toml").write_text(
            f'version = "{pin}"\n', encoding="utf-8",
        )
        wfdir = tmp / ".github" / "workflows"
        wfdir.mkdir(parents=True)
        for name, value in workflows.items():
            (wfdir / name).write_text(
                f"name: {name}\nenv:\n  SHIPYARD_VERSION: \"{value}\"\n",
                encoding="utf-8",
            )

    def test_passes_when_all_workflows_match(self) -> None:
        with mock.patch.dict(os.environ):
            import tempfile
            with tempfile.TemporaryDirectory() as td:
                tmp = pathlib.Path(td)
                self._setup_fake_repo(tmp, "v1.2.3", {
                    "a.yml": "1.2.3",
                    "b.yml": "1.2.3",
                })
                r = run_check_in_fake_repo(tmp)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("Shipyard pin OK", r.stdout)

    def test_fails_when_any_workflow_differs(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            self._setup_fake_repo(tmp, "v1.2.3", {
                "a.yml": "1.2.3",
                "b.yml": "9.9.9",  # drift!
            })
            r = run_check_in_fake_repo(tmp)
            self.assertEqual(r.returncode, 1)
            self.assertIn("drift detected", r.stderr)
            self.assertIn("b.yml", r.stderr)

    def test_strips_leading_v_when_comparing(self) -> None:
        # Pin file uses `v1.2.3`, workflows historically use `1.2.3`
        # without the `v`. Normalize both ways.
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            self._setup_fake_repo(tmp, "v1.2.3", {
                "a.yml": "v1.2.3",  # with v
                "b.yml": "1.2.3",   # without
            })
            r = run_check_in_fake_repo(tmp)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_missing_pin_file_is_config_error(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            # No tools/shipyard.toml at all.
            r = run_check_in_fake_repo(tmp)
            self.assertEqual(r.returncode, 2)
            self.assertIn("does not exist", r.stderr)

    def test_no_workflows_passes_quietly(self) -> None:
        """If the repo has the pin file but no workflows declare
        SHIPYARD_VERSION, there is no drift to report. Pass."""
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            (tmp / "tools").mkdir(parents=True)
            (tmp / "tools" / "shipyard.toml").write_text(
                'version = "v1.2.3"\n', encoding="utf-8",
            )
            (tmp / ".github" / "workflows").mkdir(parents=True)
            (tmp / ".github" / "workflows" / "unrelated.yml").write_text(
                "name: unrelated\nenv:\n  OTHER: \"x\"\n",
                encoding="utf-8",
            )
            r = run_check_in_fake_repo(tmp)
            self.assertEqual(r.returncode, 0)


class ScriptRunsAgainstRealRepoTests(unittest.TestCase):
    """Smoke-run the script against the actual checkout to catch the
    case where someone bumps the pin without updating both workflows."""

    def test_real_repo_check_succeeds(self) -> None:
        r = subprocess.run(
            [sys.executable, str(SCRIPT)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(
            r.returncode, 0,
            msg=f"stdout={r.stdout!r} stderr={r.stderr!r}",
        )


if __name__ == "__main__":
    unittest.main()
