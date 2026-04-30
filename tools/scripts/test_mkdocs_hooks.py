#!/usr/bin/env python3
"""Focused unit coverage for tools/mkdocs_hooks.py."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "mkdocs_hooks.py"

spec = importlib.util.spec_from_file_location("mkdocs_hooks", SCRIPT)
assert spec and spec.loader
mh = importlib.util.module_from_spec(spec)
sys.modules["mkdocs_hooks"] = mh
spec.loader.exec_module(mh)


class FakeFile:
    def __init__(self, src_uri: str, *, documentation: bool = True) -> None:
        self.src_uri = src_uri
        self.dest_uri = f"nested/{Path(src_uri).with_suffix('.html').name}"
        self.url = self.dest_uri
        self.abs_dest_path = f"/site/{self.dest_uri}"
        self._documentation = documentation

    def is_documentation_page(self) -> bool:
        return self._documentation


class MkdocsHooksTests(unittest.TestCase):
    def test_load_slug_map_parses_index_entries_without_yaml_dependency(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            index = Path(td) / "docs-index.yaml"
            index.write_text(
                "\n".join(
                    [
                        "docs:",
                        "  # comments and blank lines are ignored",
                        "",
                        "  - slug: modules",
                        "    path: reference/modules.md",
                        "  - slug: platforms",
                        "    path: reference/platforms.md",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            with mock.patch.object(mh, "_DOCS_INDEX", index):
                mapping = mh._load_slug_map()

        self.assertEqual(
            mapping,
            {
                "reference/modules.md": "modules",
                "reference/platforms.md": "platforms",
            },
        )

    def test_load_slug_map_returns_empty_when_index_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            with mock.patch.object(mh, "_DOCS_INDEX", Path(td) / "missing.yaml"):
                self.assertEqual(mh._load_slug_map(), {})

    def test_on_pre_build_dispatches_existing_checks_in_order(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            docs_generate = root / "docs_generate.py"
            consistency = root / "check-docs-consistency.py"
            docs_generate.write_text("# fake\n", encoding="utf-8")
            consistency.write_text("# fake\n", encoding="utf-8")
            calls: list[tuple[str, list[str]]] = []

            with mock.patch.object(mh, "_DOCS_GENERATE", docs_generate), \
                 mock.patch.object(mh, "_CONSISTENCY", consistency), \
                 mock.patch.object(mh, "_run_check", side_effect=lambda label, cmd: calls.append((label, cmd))):
                mh.on_pre_build({})

        self.assertEqual(
            calls,
            [
                (
                    "docs_generate.py check",
                    [sys.executable, str(docs_generate), "check"],
                ),
                (
                    "check-docs-consistency.py",
                    [sys.executable, str(consistency)],
                ),
            ],
        )

    def test_run_check_raises_system_exit_on_failed_process(self) -> None:
        failed = subprocess.CompletedProcess(args=["fake"], returncode=7)

        with mock.patch.object(mh.subprocess, "run", return_value=failed) as run:
            with self.assertRaises(SystemExit) as ctx:
                mh._run_check("fake check", ["python3", "fake.py"])

        run.assert_called_once_with(["python3", "fake.py"], cwd=mh._REPO_ROOT)
        self.assertIn("fake check failed (exit 7)", str(ctx.exception))

    def test_on_files_rewrites_indexed_document_urls_to_site_root(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            site_dir = Path(td) / "site"
            matched = FakeFile("reference/modules.md")
            unmatched = FakeFile("guides/intro.md")
            non_doc = FakeFile("reference/platforms.md", documentation=False)
            files = [matched, unmatched, non_doc]

            with mock.patch.object(
                mh,
                "_load_slug_map",
                return_value={
                    "reference/modules.md": "modules",
                    "reference/platforms.md": "platforms",
                },
            ):
                returned = mh.on_files(files, {"site_dir": str(site_dir)})

        self.assertIs(returned, files)
        self.assertEqual(matched.dest_uri, "modules.html")
        self.assertEqual(matched.url, "modules.html")
        self.assertEqual(matched.abs_dest_path, str(site_dir / "modules.html"))
        self.assertEqual(unmatched.dest_uri, "nested/intro.html")
        self.assertEqual(non_doc.dest_uri, "nested/platforms.html")

    def test_on_files_returns_files_unchanged_when_slug_map_is_empty(self) -> None:
        files = [FakeFile("reference/modules.md")]

        with mock.patch.object(mh, "_load_slug_map", return_value={}):
            returned = mh.on_files(files, {"site_dir": "/site"})

        self.assertIs(returned, files)
        self.assertEqual(files[0].dest_uri, "nested/modules.html")


if __name__ == "__main__":
    unittest.main()
