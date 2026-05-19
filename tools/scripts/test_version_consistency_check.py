#!/usr/bin/env python3
"""Unit tests for version_consistency_check.py.

Catches the bug-classes the script is meant to detect:

  1. CHANGELOG advertises a version higher than CMakeLists VERSION
     (the Codex P2 on #2331 — auto-release wouldn't tag, entry sits
     orphaned).
  2. plugin.json ≠ marketplace.json top-level version (mismatched
     bumps).
  3. plugin.json ≠ marketplace.json plugins[0].version (the Codex P2
     on #2341 — bump script only updated top-level, nested field
     went stale).

Also verifies the script does NOT false-flag the harmless reverse
direction (CMake ahead of CHANGELOG — the brief window between merge
and the auto-release-bot's CHANGELOG regen).
"""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest


HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE / "version_consistency_check.py"


def _make_repo_layout(
    td: pathlib.Path,
    *,
    cmake_version: str,
    plugin_version: str,
    marketplace_top_version: str,
    marketplace_nested_version: str,
    changelog_top_version: str,
) -> None:
    """Stage all four version-bearing files plus a minimal layout that
    matches what `version_consistency_check.py` reads."""
    (td / "CMakeLists.txt").write_text(
        f"project(Pulp VERSION {cmake_version})\n",
        encoding="utf-8",
    )
    (td / ".claude-plugin").mkdir()
    (td / ".claude-plugin" / "plugin.json").write_text(
        json.dumps({"version": plugin_version}),
        encoding="utf-8",
    )
    (td / ".claude-plugin" / "marketplace.json").write_text(
        json.dumps({
            "version": marketplace_top_version,
            "plugins": [{"version": marketplace_nested_version}],
        }),
        encoding="utf-8",
    )
    (td / "CHANGELOG.md").write_text(
        textwrap.dedent(f"""\
        # Changelog

        ## [{changelog_top_version}] - 2026-05-19

        - test entry
        """),
        encoding="utf-8",
    )


def _run_script(repo_root: pathlib.Path) -> subprocess.CompletedProcess:
    """Invoke version_consistency_check.py against a synthetic repo
    layout. The script anchors REPO_ROOT to its own parent.parent.parent,
    so the only sane way to test it against a temp layout is to copy the
    script into the temp tree at the matching depth."""
    target_script = repo_root / "tools" / "scripts" / "version_consistency_check.py"
    target_script.parent.mkdir(parents=True, exist_ok=True)
    target_script.write_bytes(SCRIPT.read_bytes())
    return subprocess.run(
        [sys.executable, str(target_script)],
        capture_output=True,
        text=True,
        check=False,
    )


class VersionConsistencyCheckTests(unittest.TestCase):
    """Each test drops a synthetic four-file layout into a temp dir
    and invokes the script. Exit code 0 = consistent, 1 = drift."""

    def test_all_aligned_passes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            _make_repo_layout(
                td,
                cmake_version="1.2.3",
                plugin_version="0.5.0",
                marketplace_top_version="0.5.0",
                marketplace_nested_version="0.5.0",
                changelog_top_version="1.2.3",
            )
            result = _run_script(td)
            self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
            self.assertIn("version consistency OK", result.stdout)

    def test_changelog_ahead_of_cmake_fails(self) -> None:
        """The exact P2 from #2331: CHANGELOG entry for unreleased version."""
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            _make_repo_layout(
                td,
                cmake_version="1.2.3",
                plugin_version="0.5.0",
                marketplace_top_version="0.5.0",
                marketplace_nested_version="0.5.0",
                changelog_top_version="1.3.0",  # ahead of CMake
            )
            result = _run_script(td)
            self.assertEqual(result.returncode, 1)
            self.assertIn(
                "CHANGELOG top entry [1.3.0] advertises a version",
                result.stdout,
            )

    def test_cmake_ahead_of_changelog_passes(self) -> None:
        """The harmless reverse direction — the brief window between
        a merge bumping CMake and the auto-release-bot regenerating
        CHANGELOG. Must NOT flag, otherwise CI would block every PR
        for ~minutes after a release-tag fires."""
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            _make_repo_layout(
                td,
                cmake_version="1.3.0",  # ahead of CHANGELOG
                plugin_version="0.5.0",
                marketplace_top_version="0.5.0",
                marketplace_nested_version="0.5.0",
                changelog_top_version="1.2.3",
            )
            result = _run_script(td)
            self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)

    def test_plugin_vs_marketplace_top_mismatch_fails(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            _make_repo_layout(
                td,
                cmake_version="1.2.3",
                plugin_version="0.5.0",
                marketplace_top_version="0.4.0",  # mismatched
                marketplace_nested_version="0.5.0",
                changelog_top_version="1.2.3",
            )
            result = _run_script(td)
            self.assertEqual(result.returncode, 1)
            self.assertIn("plugin.json (0.5.0) != marketplace.json top-level", result.stdout)

    def test_marketplace_nested_vs_top_mismatch_fails(self) -> None:
        """The exact P2 from #2341: bump touched top-level but left
        plugins[0].version stale. cmd_version.cpp reads the nested
        field so this drift is real."""
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            _make_repo_layout(
                td,
                cmake_version="1.2.3",
                plugin_version="0.5.0",
                marketplace_top_version="0.5.0",
                marketplace_nested_version="0.4.0",  # nested stale
                changelog_top_version="1.2.3",
            )
            result = _run_script(td)
            self.assertEqual(result.returncode, 1)
            self.assertIn("plugins[0].version", result.stdout)

    def test_marketplace_top_vs_nested_drift_emits_two_diagnostics(self) -> None:
        """When top vs nested disagree AND plugin matches one of them,
        we should emit BOTH the (plugin != nested) and (top != nested)
        diagnostics so the reader sees the full picture."""
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            _make_repo_layout(
                td,
                cmake_version="1.2.3",
                plugin_version="0.5.0",
                marketplace_top_version="0.5.0",
                marketplace_nested_version="0.4.0",
                changelog_top_version="1.2.3",
            )
            result = _run_script(td)
            self.assertEqual(result.returncode, 1)
            self.assertIn(
                "plugin.json (0.5.0) != marketplace.json plugins[0].version (0.4.0)",
                result.stdout,
            )
            self.assertIn(
                "marketplace.json top-level version (0.5.0) != plugins[0].version (0.4.0)",
                result.stdout,
            )

    def test_diagnostic_output_includes_all_four_observed_values(self) -> None:
        """On failure, the script prints all four observed values so a
        reader can diagnose the drift without re-running the script."""
        with tempfile.TemporaryDirectory() as td:
            td = pathlib.Path(td)
            _make_repo_layout(
                td,
                cmake_version="1.2.3",
                plugin_version="0.5.0",
                marketplace_top_version="0.5.0",
                marketplace_nested_version="0.5.0",
                changelog_top_version="1.3.0",
            )
            result = _run_script(td)
            self.assertEqual(result.returncode, 1)
            self.assertIn("CMakeLists VERSION:", result.stdout)
            self.assertIn("plugin.json version:", result.stdout)
            self.assertIn("marketplace top version:", result.stdout)
            self.assertIn("marketplace plugins[0].version:", result.stdout)
            self.assertIn("CHANGELOG top entry:", result.stdout)


if __name__ == "__main__":
    unittest.main()
