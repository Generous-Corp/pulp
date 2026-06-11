#!/usr/bin/env python3
"""Tests for target preflight facade bindings."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("target_preflight_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("target_preflight_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class TargetPreflightBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, preflight):
        bindings = {
            "_target_preflight": preflight,
            "ROOT": Path("/repo"),
            "os": types.SimpleNamespace(environ={"PULP_LOCAL_CI_CONFIG": "/config"}),
            "subprocess": types.SimpleNamespace(run=object()),
            "time": types.SimpleNamespace(time=object(), sleep=object()),
            "print": object(),
        }
        for name in [
            "run_ssh_subprocess",
            "ssh_probe",
            "ssh_reachable",
            "utmctl_vm_status",
            "utmctl_start",
            "shared_config_path",
            "worktree_config_path",
            "config_material_for_targets",
            "git_root_for",
            "config_path",
            "config_source_name",
            "preflight_target_host_state",
            "find_material_config_drift",
            "normalize_provenance",
            "short_sha",
            "provenance_summary",
        ]:
            bindings[name] = object()
        return bindings

    def test_ssh_and_utm_bindings_delegate_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        preflight = types.SimpleNamespace(
            ssh_probe=capture("ssh_probe", {"ok": True}),
            ssh_reachable=capture("ssh_reachable", True),
            ssh_failure_detail=capture("ssh_failure", "detail"),
            ssh_command_result=capture("ssh_command", {"run": True}),
            utmctl_vm_status=capture("utm_status", "started"),
            utmctl_start=capture("utm_start", True),
            ensure_host_reachable=capture("ensure", "host"),
        )
        bindings = self._bindings(preflight)

        self.assertEqual(self.mod.ssh_probe(bindings, "host", 9), {"ok": True})
        self.assertIs(captured["ssh_probe"][1]["run_ssh_subprocess_fn"], bindings["run_ssh_subprocess"])
        self.assertTrue(self.mod.ssh_reachable(bindings, "host", 9))
        self.assertIs(captured["ssh_reachable"][1]["ssh_probe_fn"], bindings["ssh_probe"])
        self.assertEqual(self.mod.ssh_failure_detail(bindings, "host", 9), "detail")
        self.assertEqual(self.mod.ssh_command_result(bindings, "host", "echo ok", timeout=44), {"run": True})
        self.assertIs(captured["ssh_command"][1]["run_ssh_subprocess_fn"], bindings["run_ssh_subprocess"])
        self.assertEqual(self.mod.utmctl_vm_status(bindings, "VM"), "started")
        self.assertIs(captured["utm_status"][1]["run_fn"], bindings["subprocess"].run)
        self.assertTrue(self.mod.utmctl_start(bindings, "VM"))
        self.assertIs(captured["utm_start"][1]["run_fn"], bindings["subprocess"].run)
        self.assertEqual(self.mod.ensure_host_reachable(bindings, "ubuntu", {"host": "h"}, {}), "host")
        self.assertIs(captured["ensure"][1]["ssh_reachable_fn"], bindings["ssh_reachable"])
        self.assertIs(captured["ensure"][1]["time_fn"], bindings["time"].time)
        self.assertIs(captured["ensure"][1]["sleep_fn"], bindings["time"].sleep)
        self.assertIs(captured["ensure"][1]["print_fn"], bindings["print"])

    def test_config_submission_and_print_bindings_delegate_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result=None):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        preflight = types.SimpleNamespace(
            config_source_name=capture("source", "env-override"),
            config_material_for_targets=lambda *args: captured.setdefault("material", args) and {"mac": {}},
            find_material_config_drift=capture("drift", ["drift"]),
            preflight_target_host_state=capture("host_state", {"status": "local"}),
            build_submission_metadata=lambda *args, **kwargs: captured.setdefault("metadata", (args, kwargs)) and {"branch": args[1]},
            print_submission_metadata=capture("print"),
        )
        bindings = self._bindings(preflight)

        self.assertEqual(self.mod.config_source_name(bindings, Path("/config")), "env-override")
        self.assertIs(captured["source"][1]["environ"], bindings["os"].environ)
        self.assertIs(captured["source"][1]["shared_config_path_fn"], bindings["shared_config_path"])
        self.assertEqual(self.mod.config_material_for_targets(bindings, {"targets": {}}, ["mac"]), {"mac": {}})
        self.assertEqual(captured["material"], ({"targets": {}}, ["mac"]))
        self.assertEqual(self.mod.find_material_config_drift(bindings, ["mac"]), ["drift"])
        self.assertIs(captured["drift"][1]["shared_config_path_fn"], bindings["shared_config_path"])
        self.assertIs(captured["drift"][1]["worktree_config_path_fn"], bindings["worktree_config_path"])
        self.assertIs(captured["drift"][1]["config_material_for_targets_fn"], bindings["config_material_for_targets"])
        self.assertEqual(self.mod.preflight_target_host_state(bindings, "mac", {"type": "local"}, {}), {"status": "local"})
        self.assertIs(captured["host_state"][1]["ssh_reachable_fn"], bindings["ssh_reachable"])
        self.assertEqual(
            self.mod.build_submission_metadata(
                bindings,
                {"targets": {}},
                "feature/topic",
                "a" * 40,
                ["mac"],
                "normal",
                "full",
                allow_root_mismatch=False,
                allow_unreachable_targets=True,
            ),
            {"branch": "feature/topic"},
        )
        metadata_kwargs = captured["metadata"][1]
        self.assertIs(metadata_kwargs["root"], bindings["ROOT"])
        self.assertIs(metadata_kwargs["git_root_for_fn"], bindings["git_root_for"])
        self.assertIs(metadata_kwargs["config_path_fn"], bindings["config_path"])
        self.assertIs(metadata_kwargs["normalize_provenance_fn"], bindings["normalize_provenance"])
        self.assertIs(metadata_kwargs["environ"], bindings["os"].environ)

        self.mod.print_submission_metadata(bindings, {"branch": "feature/topic"})
        self.assertIs(captured["print"][1]["short_sha_fn"], bindings["short_sha"])
        self.assertIs(captured["print"][1]["provenance_summary_fn"], bindings["provenance_summary"])
        self.assertIs(captured["print"][1]["print_fn"], bindings["print"])


if __name__ == "__main__":
    unittest.main()
