#!/usr/bin/env python3
"""Facade-level desktop runtime helper integration tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_desktop_runtime_helpers_integration",
        add_module_dir=True,
    )


class DesktopRuntimeHelpersIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_view_tree_coordinate_and_process_helpers_cover_edge_paths(self) -> None:
        self.assertEqual(self.mod.parse_coordinate_pair(" 10.5, 20 ", flag_name="--click"), (10.5, 20.0))
        with self.assertRaisesRegex(ValueError, "X,Y form"):
            self.mod.parse_coordinate_pair("10", flag_name="--click")
        with self.assertRaisesRegex(ValueError, "numeric"):
            self.mod.parse_coordinate_pair("x,y", flag_name="--click")

        view_tree = {
            "id": "root",
            "bounds": {"x": 10, "y": 20, "width": 200, "height": 100},
            "children": [
                {"id": "hidden", "visible": False, "bounds": {"x": 1, "y": 1, "width": 20, "height": 20}},
                {"id": "zero", "type": "button", "bounds": {"x": 2, "y": 2, "width": 0, "height": 20}},
                {
                    "id": "panel",
                    "bounds": {"x": 5, "y": 6, "width": 100, "height": 80},
                    "children": [
                        {
                            "id": "target",
                            "type": "button",
                            "text": "OK",
                            "label": "Confirm",
                            "bounds": {"x": 7, "y": 8, "width": 30, "height": 10},
                        },
                    ],
                },
            ],
        }
        nodes = list(self.mod.iter_view_tree_nodes(view_tree))
        self.assertEqual(list(self.mod.iter_view_tree_nodes("not-a-node")), [])
        self.assertEqual(nodes[-1][1], {"x": 22.0, "y": 34.0, "width": 30.0, "height": 10.0})
        self.assertEqual(nodes[0][1]["x"], 10.0)
        self.assertEqual(nodes[0][1]["height"], 100.0)
        self.assertEqual(
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id="target",
                view_type="button",
                view_text="OK",
                view_label="Confirm",
            ),
            (37.0, 39.0),
        )
        self.assertEqual(
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id=None,
                view_type="button",
                view_text="OK",
                view_label=None,
            ),
            (37.0, 39.0),
        )
        with self.assertRaisesRegex(RuntimeError, "No visible view matched"):
            self.mod.resolve_view_tree_click_point(
                view_tree,
                view_id="missing",
                view_type=None,
                view_text=None,
                view_label=None,
            )
        self.assertEqual(
            self.mod.screen_point_for_content_point(
                {"bounds": {"x": 100, "y": 50, "width": 400, "height": 300}},
                (200, 180),
                (10, 20),
            ),
            (210.0, 190.0),
        )
        self.assertEqual(
            self.mod.screen_point_for_content_point(
                {"bounds": {"x": 5, "y": 6, "width": 20, "height": 10}},
                (40, 30),
                (3, 4),
            ),
            (8.0, 10.0),
        )

        running = mock.Mock()
        running.poll.return_value = None
        running.wait.side_effect = [subprocess.TimeoutExpired(["proc"], 1), None]
        self.mod.terminate_process(running, timeout_secs=0.01)
        running.terminate.assert_called_once()
        running.kill.assert_called_once()

        complete = mock.Mock()
        complete.poll.return_value = 0
        self.mod.terminate_process(complete)
        complete.terminate.assert_not_called()

    def test_macos_capture_and_local_worktree_helpers_cover_retry_edges(self) -> None:
        output_path = self.root / "captures" / "window.png"

        def successful_capture(*_args, **_kwargs):
            output_path.write_bytes(b"png")
            return subprocess.CompletedProcess([], 0, stdout="", stderr="")

        with mock.patch.object(self.mod.subprocess, "run", side_effect=successful_capture) as run:
            self.mod.capture_macos_window(42, output_path)
        self.assertEqual(output_path.read_bytes(), b"png")
        self.assertTrue(output_path.parent.is_dir())
        self.assertEqual(run.call_args.args[0][0], "screencapture")
        self.assertIn("-l", run.call_args.args[0])
        self.assertIn("42", run.call_args.args[0])

        failed_output = self.root / "captures" / "missing.png"
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 1, stdout="stdout detail", stderr=""),
        ), mock.patch.object(self.mod.time, "sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, "stdout detail"):
                self.mod.capture_macos_window(99, failed_output)
        self.assertEqual(sleep.call_count, 4)
        self.assertFalse(failed_output.exists())
        self.assertEqual(sleep.call_args.args[0], 0.2)

        missing = self.root / "missing-worktree"
        self.assertFalse(self.mod._local_worktree_matches(missing, "abc123"))
        worktree = self.root / "worktree"
        worktree.mkdir()
        (worktree / ".git").write_text("gitdir: elsewhere\n", encoding="utf-8")
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="abc123\n", stderr=""),
        ) as run:
            self.assertTrue(self.mod._local_worktree_matches(worktree, "abc123"))
        self.assertEqual(run.call_args.args[0][2], str(worktree))
        self.assertEqual(run.call_args.kwargs["text"], True)
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="other\n", stderr=""),
        ):
            self.assertFalse(self.mod._local_worktree_matches(worktree, "abc123"))
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 128, stdout="", stderr="bad git"),
        ):
            self.assertFalse(self.mod._local_worktree_matches(worktree, "abc123"))


if __name__ == "__main__":
    unittest.main()
