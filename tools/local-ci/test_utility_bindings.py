#!/usr/bin/env python3
"""Binding tests for local_ci utility helper facades."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import footprint_bindings  # noqa: E402
import git_helpers_bindings  # noqa: E402
import io_utils_bindings  # noqa: E402


class FakeFootprint:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def format_size_bytes(self, value):
        self.calls.append(("format_size_bytes", value))
        return "formatted"

    def path_size_bytes(self, path):
        self.calls.append(("path_size_bytes", path))
        return 42

    def local_ci_state_footprint(self):
        self.calls.append(("local_ci_state_footprint",))
        return {"total_bytes": 42}

    def state_footprint_lines(self, footprint, *, indent=""):
        self.calls.append(("state_footprint_lines", footprint, indent))
        return [f"{indent}line"]

    def describe_path_for_cleanup(self, path):
        self.calls.append(("describe_path_for_cleanup", path))
        return "relative/path"


class FakeIoUtils:
    def __init__(self) -> None:
        self.calls: list[tuple] = []
        self.lock_token = object()

    def tail_lines(self, path, limit=80):
        self.calls.append(("tail_lines", path, limit))
        return ["tail"]

    def trim_line(self, value, max_len=160):
        self.calls.append(("trim_line", value, max_len))
        return "trimmed"

    def atomic_write_text(self, path, text):
        self.calls.append(("atomic_write_text", path, text))

    def image_change_summary(self, before_path, after_path, *, diff_output_path=None):
        self.calls.append(("image_change_summary", before_path, after_path, diff_output_path))
        return {"changed": True}

    def file_lock(self, path, *, blocking):
        self.calls.append(("file_lock", path, blocking))
        return self.lock_token


class FakeGitHelpers:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def now_iso(self):
        self.calls.append(("now_iso",))
        return "now"

    def current_branch(self):
        self.calls.append(("current_branch",))
        return "branch"

    def current_sha(self):
        self.calls.append(("current_sha",))
        return "sha"

    def git_root_for(self, path):
        self.calls.append(("git_root_for", path))
        return path

    def resolve_git_ref_sha(self, ref):
        self.calls.append(("resolve_git_ref_sha", ref))
        return "resolved"

    def short_sha(self, sha):
        self.calls.append(("short_sha", sha))
        return "short"


class UtilityBindingTests(unittest.TestCase):
    def test_footprint_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeFootprint()
        bindings = {"_footprint": fake}
        path = Path("state")
        footprint = {"total_bytes": 42}

        self.assertEqual(footprint_bindings.format_size_bytes(bindings, 42), "formatted")
        self.assertEqual(footprint_bindings.path_size_bytes(bindings, path), 42)
        self.assertEqual(footprint_bindings.local_ci_state_footprint(bindings), footprint)
        self.assertEqual(footprint_bindings.state_footprint_lines(bindings, footprint, indent="  "), ["  line"])
        self.assertEqual(footprint_bindings.describe_path_for_cleanup(bindings, path), "relative/path")
        self.assertEqual(
            fake.calls,
            [
                ("format_size_bytes", 42),
                ("path_size_bytes", path),
                ("local_ci_state_footprint",),
                ("state_footprint_lines", footprint, "  "),
                ("describe_path_for_cleanup", path),
            ],
        )

    def test_io_utils_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeIoUtils()
        bindings = {"_io_utils": fake}
        before = Path("before.png")
        after = Path("after.png")
        diff = Path("diff.png")

        self.assertEqual(io_utils_bindings.tail_lines(bindings, before, 12), ["tail"])
        self.assertEqual(io_utils_bindings.trim_line(bindings, " value ", 10), "trimmed")
        self.assertIsNone(io_utils_bindings.atomic_write_text(bindings, before, "text"))
        self.assertEqual(
            io_utils_bindings.image_change_summary(bindings, before, after, diff_output_path=diff),
            {"changed": True},
        )
        self.assertIs(io_utils_bindings.file_lock(bindings, before, blocking=False), fake.lock_token)
        self.assertEqual(
            fake.calls,
            [
                ("tail_lines", before, 12),
                ("trim_line", " value ", 10),
                ("atomic_write_text", before, "text"),
                ("image_change_summary", before, after, diff),
                ("file_lock", before, False),
            ],
        )

    def test_git_helpers_bindings_delegate_to_bound_module(self) -> None:
        fake = FakeGitHelpers()
        bindings = {"_git_helpers": fake}
        path = Path("repo")

        self.assertEqual(git_helpers_bindings.now_iso(bindings), "now")
        self.assertEqual(git_helpers_bindings.current_branch(bindings), "branch")
        self.assertEqual(git_helpers_bindings.current_sha(bindings), "sha")
        self.assertEqual(git_helpers_bindings.git_root_for(bindings, path), path)
        self.assertEqual(git_helpers_bindings.resolve_git_ref_sha(bindings, "HEAD"), "resolved")
        self.assertEqual(git_helpers_bindings.short_sha(bindings, "abcdef"), "short")
        self.assertEqual(
            fake.calls,
            [
                ("now_iso",),
                ("current_branch",),
                ("current_sha",),
                ("git_root_for", path),
                ("resolve_git_ref_sha", "HEAD"),
                ("short_sha", "abcdef"),
            ],
        )


if __name__ == "__main__":
    unittest.main()
