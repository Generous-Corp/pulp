#!/usr/bin/env python3
"""Every `NOT TARGET <name>` guard around an example's add_subdirectory names a
target that example really creates.

The test tree adds some examples (examples/agent-toolkit-synth,
examples/sample-region-allpass) before examples/CMakeLists.txt does, and each
site guards its own add_subdirectory with `if(... AND NOT TARGET <name>)` so the
second one is skipped. `pulp_add_plugin(Foo ...)` creates `Foo_Core`,
`Foo_Standalone` and friends, never a bare `Foo` for a standalone-only plug-in.
A guard on the bare product name is therefore always true: the directory is added
twice and configure fails with "add_library cannot create target ... because
another target with the same name already exists".

For each guard wrapping an `add_subdirectory(<dir>)` that resolves to a directory
under the source root, the guarded name must be either a target that directory's
CMakeLists creates by name (add_library / add_executable / add_custom_target), or
`<Plugin>_<suffix>` for a plug-in it declares with pulp_add_plugin.

Run:
    python3 tools/scripts/test_cmake_example_guards.py
"""
from __future__ import annotations

import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUARD_FILES = ("examples/CMakeLists.txt", "test/cmake/*.cmake", "test/CMakeLists.txt")
IF_RE = re.compile(r"\bif\s*\(", re.I)
ENDIF_RE = re.compile(r"\bendif\s*\(", re.I)
NOT_TARGET_RE = re.compile(r"NOT\s+TARGET\s+([A-Za-z0-9_.+:\-]+)", re.I)
ADD_SUBDIR_RE = re.compile(r"add_subdirectory\s*\(\s*\"?([^\s\")]+)", re.I)
PLUGIN_RE = re.compile(r"pulp_add_plugin\s*\(\s*([A-Za-z0-9_\-]+)", re.I)
TARGET_DECL_RE = re.compile(
    r"\b(?:add_library|add_executable|add_custom_target)\s*\(\s*([A-Za-z0-9_.+\-]+)", re.I)


def strip_comments(text: str) -> str:
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def condition_end(text: str, start: int) -> int:
    """Index just past the `)` that closes the `if(` opened at `start`."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def block_end(text: str, after_condition: int) -> int:
    """Index of the `endif(` matching an `if(` whose condition ended at `after_condition`."""
    depth = 1
    pos = after_condition
    while depth:
        nxt_if = IF_RE.search(text, pos)
        nxt_end = ENDIF_RE.search(text, pos)
        if not nxt_end:
            return len(text)
        if nxt_if and nxt_if.start() < nxt_end.start():
            depth += 1
            pos = nxt_if.end()
        else:
            depth -= 1
            pos = nxt_end.end()
    return pos


def resolve_dir(raw: str, root: Path, here: Path) -> Path | None:
    value = (raw.replace("${CMAKE_SOURCE_DIR}", str(root))
                .replace("${PULP_ROOT_DIR}", str(root))
                .replace("${CMAKE_CURRENT_SOURCE_DIR}", str(here.parent)))
    if "${" in value:
        return None
    path = Path(value)
    if not path.is_absolute():
        path = here.parent / path
    return path if (path / "CMakeLists.txt").is_file() else None


def declared(directory: Path) -> tuple[set[str], set[str]]:
    text = strip_comments((directory / "CMakeLists.txt").read_text(encoding="utf-8"))
    return set(PLUGIN_RE.findall(text)), set(TARGET_DECL_RE.findall(text))


def violations(root: Path, files: list[Path]) -> list[str]:
    """One message per guarded add_subdirectory whose guard names a target the
    directory does not create. Also the number of guards examined is reported via
    `checked_guards` so a scan that found nothing can be told from one that passed."""
    out: list[str] = []
    for path in files:
        text = strip_comments(path.read_text(encoding="utf-8"))
        for match in IF_RE.finditer(text):
            cond_end = condition_end(text, match.end() - 1)
            names = NOT_TARGET_RE.findall(text[match.start():cond_end])
            if not names:
                continue
            body = text[cond_end:block_end(text, cond_end)]
            for sub in ADD_SUBDIR_RE.findall(body):
                directory = resolve_dir(sub, root, path)
                if directory is None:
                    continue
                plugins, targets = declared(directory)
                for name in names:
                    if name in targets or any(name.startswith(p + "_") for p in plugins):
                        continue
                    out.append(
                        f"{path.relative_to(root)}: `NOT TARGET {name}` guards add_subdirectory("
                        f"{directory.relative_to(root)}), but that directory creates no target "
                        f"named {name}; pulp_add_plugin creates <Plugin>_Core / <Plugin>_Standalone "
                        f"(plug-ins: {sorted(plugins) or 'none'}). Guard on one of those.")
    return out


def checked_guards(root: Path, files: list[Path]) -> int:
    count = 0
    for path in files:
        text = strip_comments(path.read_text(encoding="utf-8"))
        for match in IF_RE.finditer(text):
            cond_end = condition_end(text, match.end() - 1)
            if not NOT_TARGET_RE.search(text[match.start():cond_end]):
                continue
            body = text[cond_end:block_end(text, cond_end)]
            if any(resolve_dir(s, root, path) for s in ADD_SUBDIR_RE.findall(body)):
                count += 1
    return count


def repo_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for pattern in GUARD_FILES:
        files.extend(sorted(root.glob(pattern)))
    return files


class ExampleGuardTests(unittest.TestCase):
    def test_the_real_tree_has_no_guard_on_a_name_the_example_does_not_create(self) -> None:
        self.assertEqual(violations(ROOT, repo_files(ROOT)), [])

    def test_the_scan_sees_the_guards_it_is_meant_to_check(self) -> None:
        # Control: a scan that matched nothing would report "no violations" over
        # zero coverage. The tree has at least the agent-toolkit-synth guard at
        # both add sites and the sample-region-allpass guard at both.
        self.assertGreaterEqual(checked_guards(ROOT, repo_files(ROOT)), 4)

    def _fixture(self, guard_name: str, plugin: str = "Widget") -> tuple[Path, list[Path]]:
        tmp = Path(tempfile.mkdtemp(prefix="example-guards-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp, ignore_errors=True))
        (tmp / "examples" / "widget").mkdir(parents=True)
        (tmp / "examples" / "widget" / "CMakeLists.txt").write_text(
            f"pulp_add_plugin({plugin}\n    FORMATS Standalone\n)\n", encoding="utf-8")
        (tmp / "examples" / "CMakeLists.txt").write_text(
            f"if(APPLE AND NOT TARGET {guard_name})\n    add_subdirectory(widget)\nendif()\n",
            encoding="utf-8")
        return tmp, [tmp / "examples" / "CMakeLists.txt"]

    def test_a_planted_bare_product_name_guard_fails(self) -> None:
        root, files = self._fixture("Widget")
        found = violations(root, files)
        self.assertEqual(len(found), 1, found)
        self.assertIn("NOT TARGET Widget", found[0])
        self.assertIn("plug-ins: ['Widget']", found[0])

    def test_a_suffixed_target_the_plugin_creates_passes(self) -> None:
        for name in ("Widget_Core", "Widget_Standalone"):
            root, files = self._fixture(name)
            self.assertEqual(violations(root, files), [], name)

    def test_a_target_the_directory_declares_by_name_passes(self) -> None:
        root, files = self._fixture("widget-core")
        (root / "examples" / "widget" / "CMakeLists.txt").write_text(
            "add_library(widget-core STATIC a.cpp)\n", encoding="utf-8")
        self.assertEqual(violations(root, files), [])

    def test_a_guard_that_does_not_wrap_an_example_is_ignored(self) -> None:
        root, files = self._fixture("Widget_Core")
        files[0].write_text("if(NOT TARGET Unrelated)\n    message(STATUS x)\nendif()\n",
                            encoding="utf-8")
        self.assertEqual(violations(root, files), [])

    def test_a_guard_in_a_nested_if_is_attributed_to_the_right_block(self) -> None:
        root, files = self._fixture("Widget")
        files[0].write_text(
            "if(APPLE)\n    if(GPU AND NOT TARGET Widget)\n        add_subdirectory(widget)\n"
            "    endif()\nendif()\nadd_subdirectory(widget)\n", encoding="utf-8")
        self.assertEqual(len(violations(root, files)), 1)


if __name__ == "__main__":
    sys.exit(unittest.main(verbosity=2))
