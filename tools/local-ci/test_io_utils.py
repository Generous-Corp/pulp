#!/usr/bin/env python3
"""Tests for the io_utils text/file/image helpers.

Split out of test_local_ci.py (roadmap P11-3) so the test surface mirrors
the extracted io_utils module. The harness still loads the local_ci.py
orchestrator, which re-exports the io_utils symbols.
"""

import io
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.parse import urlparse
from unittest import mock
from contextlib import redirect_stdout
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("local_ci.py")
VALIDATE_BUILD_PATH = MODULE_PATH.parent.parent.parent / "validate-build.sh"


def load_module():
    spec = importlib.util.spec_from_file_location("pulp_local_ci", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module



class IoUtilsTests(unittest.TestCase):
    def _set_target_enabled(self, name: str, enabled: bool):
        payload = json.loads(self.config_path.read_text())
        payload.setdefault("targets", {}).setdefault(name, {})["enabled"] = enabled
        self.config_path.write_text(json.dumps(payload) + "\n")

    def _write_desktop_manifest(self, config, target, action, manifest):
        bundle = self.mod.create_desktop_run_bundle(config, target, action)
        payload = dict(manifest)
        artifacts = dict(payload.get("artifacts", {}))
        artifacts.setdefault("bundle_dir", str(bundle))
        payload["artifacts"] = artifacts
        (bundle / "manifest.json").write_text(json.dumps(payload) + "\n")
        return bundle, payload

    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"
        self.config_path = root / "config.json"
        self.config_path.write_text(
            json.dumps(
                {
                    "desktop_automation": {
                        "artifact_root": str(root / "desktop-artifacts"),
                    },
                    "targets": {
                        "mac": {"type": "local", "enabled": True},
                        "ubuntu": {"type": "ssh", "enabled": True, "host": "ubuntu", "repo_path": "/tmp/pulp"},
                        "windows": {"type": "ssh", "enabled": False, "host": "win2", "repo_path": "C:\\Pulp"},
                    },
                    "github_actions": {
                        "repository": "danielraffel/pulp",
                        "defaults": {
                            "workflow": "build",
                            "provider": "github-hosted",
                            "wait_poll_secs": 5,
                            "match_timeout_secs": 30,
                        },
                        "workflows": {
                            "build": {
                                "providers": {
                                    "namespace": {
                                        "linux_runner_selector_json": "\"namespace-profile-default\"",
                                        "windows_runner_selector_json": "\"namespace-profile-default\"",
                                    }
                                }
                            },
                            "docs-check": {
                                "providers": {
                                    "namespace": {
                                        "runner_selector_json": "\"namespace-profile-default\""
                                    }
                                }
                            }
                        },
                    },
                    "defaults": {
                        "priority": "normal",
                        "targets": ["mac"],
                    },
                }
            )
            + "\n"
        )

        self.prev_home = os.environ.get("PULP_LOCAL_CI_HOME")
        self.prev_config = os.environ.get("PULP_LOCAL_CI_CONFIG")
        os.environ["PULP_LOCAL_CI_HOME"] = str(self.state_dir)
        os.environ["PULP_LOCAL_CI_CONFIG"] = str(self.config_path)
        self.mod = load_module()

    def tearDown(self):
        if self.prev_home is None:
            os.environ.pop("PULP_LOCAL_CI_HOME", None)
        else:
            os.environ["PULP_LOCAL_CI_HOME"] = self.prev_home

        if self.prev_config is None:
            os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        else:
            os.environ["PULP_LOCAL_CI_CONFIG"] = self.prev_config

        self.tmpdir.cleanup()


    def test_text_file_helpers_tail_trim_and_atomic_write(self):
        log_path = self.state_dir / "logs" / "job" / "mac.log"
        log_path.parent.mkdir(parents=True)
        log_path.write_text("one\ntwo\nthree\n")

        self.assertEqual(self.mod.tail_lines(self.state_dir / "missing.log"), [])
        self.assertEqual(self.mod.tail_lines(log_path, limit=2), ["two\n", "three\n"])

        target_path = self.state_dir / "results" / "result.txt"
        self.mod.atomic_write_text(target_path, "first\n")
        self.mod.atomic_write_text(target_path, "second\n")
        self.assertEqual(target_path.read_text(), "second\n")

        trimmed = self.mod.trim_line("  " + ("x" * 170) + "tail  ", max_len=12)
        self.assertEqual(len(trimmed), 12)
        self.assertTrue(trimmed.endswith("tail"))


    def test_image_change_summary_falls_back_to_hash_for_non_images(self):
        before_path = Path(self.tmpdir.name) / "before.bin"
        after_path = Path(self.tmpdir.name) / "after.bin"
        diff_path = Path(self.tmpdir.name) / "diff" / "image.png"
        before_path.write_bytes(b"same")
        after_path.write_bytes(b"same")

        unchanged = self.mod.image_change_summary(before_path, after_path, diff_output_path=diff_path)
        self.assertFalse(unchanged["changed"])
        self.assertEqual(unchanged["method"], "file-hash")
        self.assertFalse(diff_path.exists())

        after_path.write_bytes(b"different")
        changed = self.mod.image_change_summary(before_path, after_path, diff_output_path=diff_path)
        self.assertTrue(changed["changed"])
        self.assertEqual(changed["method"], "file-hash")
        self.assertFalse(diff_path.exists())

    def test_image_change_summary_uses_pixel_bbox_when_pillow_is_available(self):
        before_path = Path(self.tmpdir.name) / "before.png"
        after_path = Path(self.tmpdir.name) / "after.png"
        diff_path = Path(self.tmpdir.name) / "diff" / "image.png"
        before_path.write_bytes(b"before")
        after_path.write_bytes(b"after")

        class FakeImage:
            def convert(self, mode):
                self.mode = mode
                return self

        class FakeDiff:
            def __init__(self, bbox):
                self.bbox = bbox

            def save(self, path):
                path.write_text("diff\n")

            def getbbox(self):
                return self.bbox

        pil_pkg = type(sys)("PIL")
        image_mod = type(sys)("PIL.Image")
        chops_mod = type(sys)("PIL.ImageChops")
        image_mod.open = mock.Mock(return_value=FakeImage())
        chops_mod.difference = mock.Mock(return_value=FakeDiff((1, 2, 3, 4)))
        pil_pkg.Image = image_mod
        pil_pkg.ImageChops = chops_mod

        with mock.patch.dict(
            sys.modules,
            {
                "PIL": pil_pkg,
                "PIL.Image": image_mod,
                "PIL.ImageChops": chops_mod,
            },
        ):
            changed = self.mod.image_change_summary(
                before_path, after_path, diff_output_path=diff_path
            )

        self.assertTrue(changed["changed"])
        self.assertEqual(changed["method"], "pixel-bbox")
        self.assertEqual(
            changed["bbox"],
            {"left": 1, "top": 2, "right": 3, "bottom": 4},
        )
        self.assertEqual(diff_path.read_text(), "diff\n")

        chops_mod.difference = mock.Mock(return_value=FakeDiff(None))
        with mock.patch.dict(
            sys.modules,
            {
                "PIL": pil_pkg,
                "PIL.Image": image_mod,
                "PIL.ImageChops": chops_mod,
            },
        ):
            unchanged = self.mod.image_change_summary(before_path, after_path)

        self.assertFalse(unchanged["changed"])
        self.assertEqual(unchanged["method"], "pixel-bbox")
        self.assertNotIn("bbox", unchanged)

    def test_tail_lines_replaces_invalid_bytes_and_trim_exact_boundary(self):
        log_path = self.state_dir / "logs" / "invalid.log"
        log_path.parent.mkdir(parents=True)
        log_path.write_bytes(b"good\nbad:\xff\n")

        self.assertEqual(self.mod.tail_lines(log_path), ["good\n", "bad:\ufffd\n"])
        self.assertEqual(self.mod.trim_line(" abc ", max_len=3), "abc")
        self.assertEqual(self.mod.trim_line("abcdef", max_len=2), "…f")

    def test_file_lock_raises_busy_when_nonblocking_flock_fails(self):
        lock_path = self.state_dir / "queue.lock"
        io_utils_mod = sys.modules["io_utils"]

        with mock.patch.object(io_utils_mod.fcntl, "flock", side_effect=BlockingIOError("busy")):
            with self.assertRaises(self.mod.LockBusyError):
                with self.mod.file_lock(lock_path, blocking=False):
                    pass

    def test_file_lock_yields_handle_and_releases(self):
        lock_path = self.state_dir / "queue.lock"

        with self.mod.file_lock(lock_path, blocking=True) as handle:
            self.assertFalse(handle.closed)
            handle.write("held")

        self.assertTrue(handle.closed)



if __name__ == "__main__":
    unittest.main()
