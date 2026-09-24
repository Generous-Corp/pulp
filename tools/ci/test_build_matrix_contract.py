#!/usr/bin/env python3
"""Structural contract for the Build and Test workflow's macOS matrix leg.

Two things are pinned here.

The macOS leg must run on EVERY event, push included. A push to main is the
only lane that runs the full macOS suite against a commit that is actually on
main: pull-request heads skip the test step, and a merge group validates a
synthetic merge commit. Re-gating the leg on the event name would restore the
blind spot in which a break on main is invisible until a queued batch inherits
it, reports ~40 minutes later, and names a batch member that is not the
culprit. The assertion is made over the parsed syntax tree rather than the file
text, so a differently-spelled event guard cannot slip past a string match.

The workflow also embeds Python in shell heredocs inside YAML block scalars,
where indentation carries meaning three times over. A mis-indented edit there
produces a file that still loads as YAML and still lints, and fails only when
the job runs. Compiling every heredoc catches that locally instead.
"""

from __future__ import annotations

import ast
import pathlib
import re
import textwrap
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD_YML = REPO_ROOT / ".github" / "workflows" / "build.yml"

HEREDOC_RE = re.compile(r"<<'(\w+)'\s*$")


def extract_heredocs(text: str) -> list[tuple[int, str, str]]:
    """Every quoted heredoc body in the workflow, dedented and ready to compile."""
    lines = text.splitlines()
    blocks: list[tuple[int, str, str]] = []
    i = 0
    while i < len(lines):
        match = HEREDOC_RE.search(lines[i])
        if match:
            tag = match.group(1)
            body: list[str] = []
            j = i + 1
            while j < len(lines) and lines[j].strip() != tag:
                body.append(lines[j])
                j += 1
            blocks.append((i + 1, tag, textwrap.dedent("\n".join(body))))
            i = j
        i += 1
    return blocks


class HeredocSyntaxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not BUILD_YML.is_file():
            raise unittest.SkipTest(f"{BUILD_YML} not present")
        cls.blocks = extract_heredocs(BUILD_YML.read_text(encoding="utf-8"))

    def test_extractor_found_heredocs(self) -> None:
        """Control: a zero here would make every other check in this class vacuous."""
        self.assertGreater(
            len(self.blocks),
            0,
            "no heredocs extracted from build.yml — the extractor is broken, "
            "so the syntax check below would pass without checking anything.",
        )

    def test_every_embedded_python_heredoc_compiles(self) -> None:
        checked = 0
        for start, tag, src in self.blocks:
            if "import " not in src and "print(" not in src:
                continue  # not a Python body
            checked += 1
            with self.subTest(line=start, tag=tag):
                try:
                    compile(src, f"build.yml:{start}", "exec")
                except SyntaxError as exc:  # pragma: no cover - failure path
                    self.fail(
                        f"embedded Python at build.yml:{start} does not compile: "
                        f"{exc}. A block scalar's indentation was probably "
                        f"disturbed by an edit."
                    )
        self.assertGreater(checked, 0, "no embedded Python heredocs were checked")


class MacosLegAlwaysRunsTests(unittest.TestCase):
    """The macOS matrix entry must not be conditional on the event."""

    @classmethod
    def setUpClass(cls) -> None:
        if not BUILD_YML.is_file():
            raise unittest.SkipTest(f"{BUILD_YML} not present")
        text = BUILD_YML.read_text(encoding="utf-8")
        cls.text = text
        # The resolver heredoc is the largest Python body in the workflow.
        candidates = [
            (start, src)
            for start, _tag, src in extract_heredocs(text)
            if "include" in src and "macos" in src
        ]
        if not candidates:
            raise AssertionError("no heredoc assembles a matrix with a macos key")
        cls.start, cls.src = max(candidates, key=lambda pair: len(pair[1]))
        cls.tree = ast.parse(cls.src)

    def _macos_append_nodes(self) -> list[ast.Call]:
        """Every `include.append({... "key": "macos" ...})` call in the resolver."""
        found: list[ast.Call] = []
        for node in ast.walk(self.tree):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            if not (isinstance(func, ast.Attribute) and func.attr == "append"):
                continue
            if not (isinstance(func.value, ast.Name) and func.value.id == "include"):
                continue
            for arg in node.args:
                if not isinstance(arg, ast.Dict):
                    continue
                for key, value in zip(arg.keys, arg.values):
                    if (
                        isinstance(key, ast.Constant)
                        and key.value == "key"
                        and isinstance(value, ast.Constant)
                        and value.value == "macos"
                    ):
                        found.append(node)
        return found

    def test_exactly_one_macos_matrix_entry(self) -> None:
        self.assertEqual(
            len(self._macos_append_nodes()),
            1,
            "expected exactly one include.append for the macos matrix key",
        )

    def test_macos_entry_is_not_inside_a_conditional(self) -> None:
        target = self._macos_append_nodes()[0]

        # Map each node to its parent so the append's ancestry can be walked.
        parents: dict[ast.AST, ast.AST] = {}
        for parent in ast.walk(self.tree):
            for child in ast.iter_child_nodes(parent):
                parents[child] = parent

        node: ast.AST = target
        chain: list[ast.AST] = []
        while node in parents:
            node = parents[node]
            chain.append(node)

        guards = [n for n in chain if isinstance(n, (ast.If, ast.Try))]
        self.assertEqual(
            guards,
            [],
            "the macOS matrix entry is nested inside a conditional. It must be "
            "appended unconditionally: a push to main is the only lane that "
            "runs the full macOS suite against main, so gating it re-opens the "
            "window in which a break on main is invisible for ~40 minutes and "
            "is then blamed on an innocent queue entry.",
        )

    def test_no_event_gated_macos_omission_remains(self) -> None:
        self.assertNotIn(
            "PUSH_ONLY_CACHE_EVENTS",
            self.text,
            "the push-only cache-event gate is back; it omitted the macOS leg "
            "on push, which is the blind spot this contract closes.",
        )

    def test_push_to_main_still_triggers_the_workflow(self) -> None:
        """Control: the leg is only useful if push actually starts a run."""
        self.assertRegex(
            self.text,
            r"\n  push:\n    branches: \[main\]",
            "build.yml no longer runs on push to main, so the macOS leg above "
            "would never execute against main.",
        )


if __name__ == "__main__":
    unittest.main()
