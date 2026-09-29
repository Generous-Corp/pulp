#!/usr/bin/env python3
"""Structural contract for the Build and Test workflow's macOS matrix leg.

Two things are pinned here.

The macOS leg runs on every event EXCEPT push, and on push only. main's macOS
health is the merge group's required `macos` job: the queue lands with the
MERGE method, so the merge-group head IS the commit on main. A push leg
carried no event class, so no gate runner served its selector; it queued for
hours holding main's push concurrency group and got every later push run
cancelled with zero jobs. Dropping it on any OTHER event would remove the
required gate itself. The assertion is made over the parsed syntax tree rather
than the file text, so a differently-spelled event guard cannot slip past a
string match.

The workflow also embeds Python in shell heredocs inside YAML block scalars,
where indentation carries meaning three times over. A mis-indented edit there
produces a file that still loads as YAML and still lints, and fails only when
the job runs. Compiling every heredoc catches that locally instead.

The step that surfaces ctest failures into the job summary is a reporter, and a
reporter must never decide the job's result. It runs whenever the Test step's
outcome is failure, which on a pull-request head is non-gating receipt evidence,
so a non-zero exit from the reporter alone turns the required check red. Its
script is executed here under the exact shell GitHub uses for `shell: bash`.
"""

from __future__ import annotations

import ast
import pathlib
import os
import re
import subprocess
import tempfile
import textwrap
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD_YML = REPO_ROOT / ".github" / "workflows" / "build.yml"

HEREDOC_RE = re.compile(r"<<'(\w+)'\s*$")
SURFACE_STEP = "Surface ctest failures (non-Windows)"
# The command line GitHub Actions runs a `shell: bash` step with.
GITHUB_BASH = ["bash", "--noprofile", "--norc", "-e", "-o", "pipefail"]
EXPRESSION_RE = re.compile(r"\$\{\{[^}]*\}\}")


def step_run_script(text: str, step_name: str) -> str:
    """The `run: |` body of the named step, dedented, expressions blanked out."""
    lines = text.splitlines()
    start = next(
        i for i, line in enumerate(lines) if line.strip() == f"- name: {step_name}"
    )
    step_indent = len(lines[start]) - len(lines[start].lstrip())
    i = start + 1
    while not lines[i].strip().startswith("run: |"):
        if lines[i].strip().startswith("- name:"):
            raise AssertionError(f"step {step_name!r} has no `run: |` block")
        i += 1
    body: list[str] = []
    for line in lines[i + 1 :]:
        indent = len(line) - len(line.lstrip())
        if line.strip() and indent <= step_indent + 2:
            break
        body.append(line)
    return EXPRESSION_RE.sub("x", textwrap.dedent("\n".join(body)))


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


class MacosLegSkipsOnlyPushTests(unittest.TestCase):
    """The macOS matrix entry is gated on exactly `EVENT_NAME != "push"`."""

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

    def _guards(self) -> list[ast.AST]:
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
        return [n for n in chain if isinstance(n, (ast.If, ast.Try))]

    def test_macos_entry_is_gated_only_on_not_push(self) -> None:
        guards = self._guards()
        self.assertEqual(
            len(guards), 1,
            "the macOS matrix entry must sit under exactly one guard, "
            "`if EVENT_NAME != \"push\":`",
        )
        self.assertIsInstance(guards[0], ast.If)
        self.assertEqual(
            ast.unparse(guards[0].test),
            "EVENT_NAME != 'push'",
            "the macOS leg is gated on something other than push; any other "
            "event guard drops the required `macos` gate itself",
        )
        body_calls = [n for n in ast.walk(guards[0]) if n in self._macos_append_nodes()]
        self.assertTrue(body_calls)
        self.assertFalse(
            any(n in self._macos_append_nodes() for stmt in guards[0].orelse for n in ast.walk(stmt)),
            "the macOS entry is on the else branch: it would run ONLY on push",
        )

    def test_push_to_main_still_triggers_the_workflow(self) -> None:
        """Control: push still starts a run, which publishes the Linux/Windows
        caches; the guard above is only meaningful if push runs exist."""
        self.assertRegex(
            self.text,
            r"\n  push:\n    branches: \[main\]",
            "build.yml no longer runs on push to main, so the hosted cache-save "
            "steps are unreachable.",
        )


class SurfaceCtestFailuresStepTests(unittest.TestCase):
    """The failure reporter exits 0 in every state a failed test run leaves."""

    @classmethod
    def setUpClass(cls) -> None:
        if not BUILD_YML.is_file():
            raise unittest.SkipTest(f"{BUILD_YML} not present")
        cls.script = step_run_script(
            BUILD_YML.read_text(encoding="utf-8"), SURFACE_STEP
        )

    def _run(self, files: dict[str, str]) -> tuple[subprocess.CompletedProcess, str]:
        with tempfile.TemporaryDirectory() as root:
            build = pathlib.Path(root, "build")
            for rel, content in files.items():
                path = build / "Testing" / "Temporary" / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
            summary = pathlib.Path(root, "summary.md")
            env = dict(
                os.environ,
                PULP_BUILD_DIR=str(build),
                GITHUB_STEP_SUMMARY=str(summary),
            )
            proc = subprocess.run(
                GITHUB_BASH + ["-c", self.script],
                env=env,
                capture_output=True,
                text=True,
                timeout=60,
            )
            written = summary.read_text(encoding="utf-8") if summary.exists() else ""
        return proc, written

    def test_extracted_the_reporter_script(self) -> None:
        """Control: the extractor found the step, not an empty body."""
        self.assertIn("LastTestsFailed.log", self.script)
        self.assertIn("GITHUB_STEP_SUMMARY", self.script)

    def test_failed_tests_without_catch2_assertions_exit_zero(self) -> None:
        # A timed-out or non-Catch2 test leaves no line the assertion grep
        # matches; under pipefail that grep used to end the step with exit 1.
        proc, summary = self._run(
            {
                "LastTestsFailed.log": "20705:a mouse-opened popup\n",
                "LastTest.log": "Test timed out after 120 seconds\n",
            }
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("a mouse-opened popup", summary)
        self.assertIn("a mouse-opened popup", proc.stdout)

    def test_failed_tests_without_a_last_test_log_exit_zero(self) -> None:
        proc, summary = self._run({"LastTestsFailed.log": "7:only-the-name\n"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("only-the-name", summary)

    def test_catch2_assertions_are_surfaced(self) -> None:
        proc, summary = self._run(
            {
                "LastTestsFailed.log": "3:named\n",
                "LastTest.log": "x.cpp:9: FAILED:\n  REQUIRE( a == b )\n",
            }
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("REQUIRE( a == b )", summary)

    def test_no_ctest_result_log_exits_zero(self) -> None:
        proc, summary = self._run({})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("No ctest result log", summary)


if __name__ == "__main__":
    unittest.main()
