#!/usr/bin/env python3
"""Static regression tests for Shipyard workflow selectors.

Shipyard passes configured cloud workflow selectors directly to `gh`.
`gh run list --workflow` accepts a workflow file name (`build.yml`), a
workflow display name (`Build and Test`), or an id; it does not resolve
bare file stems like `build`. Keep the tracked Shipyard config on file
names so local display-name changes do not break CI dispatch.

Run:
    python3 tools/scripts/test_shipyard_config.py
"""

from __future__ import annotations

import tempfile
import tomllib
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def _load_toml(path: Path) -> dict:
    with path.open("rb") as f:
        return tomllib.load(f)


def _workflow_files(root: Path) -> set[str]:
    workflow_dir = root / ".github" / "workflows"
    return {
        path.name
        for pattern in ("*.yml", "*.yaml")
        for path in workflow_dir.glob(pattern)
    }


def _configured_cloud_workflows(config: dict) -> list[tuple[str, str]]:
    workflows: list[tuple[str, str]] = []
    cloud = config.get("cloud") or {}
    if workflow := cloud.get("workflow"):
        workflows.append(("[cloud].workflow", workflow))

    targets = config.get("targets") or {}
    for target_name, target in sorted(targets.items()):
        if target.get("backend") != "cloud":
            continue
        if workflow := target.get("workflow"):
            workflows.append((f"[targets.{target_name}].workflow", workflow))
    return workflows


def _workflow_selector_errors(root: Path, config: dict) -> list[str]:
    workflow_files = _workflow_files(root)
    errors: list[str] = []
    for label, workflow in _configured_cloud_workflows(config):
        if not workflow.endswith((".yml", ".yaml")):
            errors.append(
                f"{label} must use a workflow file name such as build.yml; "
                f"got {workflow!r}",
            )
            continue
        if workflow not in workflow_files:
            errors.append(f"{label} references missing .github/workflows/{workflow}")
    return errors


class ShipyardConfigWorkflowTests(unittest.TestCase):
    def test_real_shipyard_cloud_workflows_reference_files(self) -> None:
        config = _load_toml(REPO_ROOT / ".shipyard" / "config.toml")
        errors = _workflow_selector_errors(REPO_ROOT, config)
        self.assertEqual(errors, [], "\n".join(errors))

    def test_rejects_bare_workflow_stem_for_cloud_targets(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / ".github" / "workflows").mkdir(parents=True)
            (root / ".github" / "workflows" / "build.yml").write_text(
                "name: Build and Test\n",
                encoding="utf-8",
            )
            config = {
                "cloud": {"workflow": "build"},
                "targets": {
                    "mac": {"backend": "cloud", "workflow": "build"},
                    "linux": {"backend": "ssh", "workflow": "build"},
                },
            }
            errors = _workflow_selector_errors(root, config)
            self.assertEqual(len(errors), 2)
            self.assertIn("[cloud].workflow must use a workflow file name", errors[0])
            self.assertIn("[targets.mac].workflow must use a workflow file name", errors[1])

    def test_accepts_existing_workflow_file_name(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / ".github" / "workflows").mkdir(parents=True)
            (root / ".github" / "workflows" / "build.yml").write_text(
                "name: Build and Test\n",
                encoding="utf-8",
            )
            config = {
                "cloud": {"workflow": "build.yml"},
                "targets": {
                    "mac": {"backend": "cloud", "workflow": "build.yml"},
                },
            }
            self.assertEqual(_workflow_selector_errors(root, config), [])

    def test_rejects_missing_workflow_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / ".github" / "workflows").mkdir(parents=True)
            config = {"cloud": {"workflow": "missing.yml"}}
            self.assertEqual(
                _workflow_selector_errors(root, config),
                ["[cloud].workflow references missing .github/workflows/missing.yml"],
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
