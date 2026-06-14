#!/usr/bin/env python3
"""Tests for the local_ci.py facade bootstrap module."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


MODULE_PATH = Path(__file__).with_name("local_ci_bootstrap.py")
LOCAL_CI_DIR = MODULE_PATH.parent


def load_module():
    if str(LOCAL_CI_DIR) not in sys.path:
        sys.path.insert(0, str(LOCAL_CI_DIR))
    spec = importlib.util.spec_from_file_location("local_ci_bootstrap_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class LocalCiBootstrapTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_install_local_ci_facade_exports_constants_and_private_seams(self) -> None:
        bindings = {
            "ROOT": Path("/repo"),
            "WAIT_POLL_SECS": 3,
            "KEEP_COMPLETED_JOBS": 25,
        }

        self.mod.install_local_ci_facade(bindings)

        for name in (
            "HEARTBEAT_INTERVAL_SECS",
            "STUCK_IDLE_SECS",
            "WINDOWS_REQUIRED_REMOTE_TOOLS",
            "LINUX_REQUIRED_REMOTE_TOOLS",
            "PRIORITY_VALUES",
            "GITHUB_ACTIONS_DEFAULTS",
            "BUILTIN_GITHUB_WORKFLOWS",
            "LockBusyError",
            "build_parser",
            "dispatch_main_command",
        ):
            self.assertIn(name, bindings)

        for name in (
            "_cloud",
            "_desktop_setup_commands_cli",
            "_execution",
            "_queue_orchestrator",
            "_source_prep",
            "_windows_target",
            "_build_target_tasks",
            "_run_git",
            "_command_path_rewrite_candidate",
        ):
            self.assertIn(name, bindings)

        self.assertIs(
            bindings["evidence_index_module"],
            self.mod._local_ci_bootstrap_module_aliases.BOOTSTRAP_MODULE_ALIASES["evidence_index_module"],
        )


if __name__ == "__main__":
    unittest.main()
