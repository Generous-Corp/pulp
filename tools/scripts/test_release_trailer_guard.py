#!/usr/bin/env python3
"""Cover the bypass-trailer classification that decides whether a merge tags.

Three properties, each of which has a way of failing silently:

* A trailer that is only being QUOTED must not withhold a tag. Nothing
  observes a release that does not happen, so this failure reports itself
  nowhere.
* A real declaration must still withhold one, or the bypass is dead.
* The post-merge tagger and the PR-time gate must accept the same values.
  Divergence lets an author write a bypass that passes one layer and fails
  the other.

The shell half runs the step body EXTRACTED from `.github/workflows/`
`auto-release.yml` against real commits, never a replica pasted into this
file — a replica is how the shell scan came to disagree with the parse it was
supposed to mirror in the first place. The extraction is stdlib-only so it
cannot degrade to a skip on a lane without PyYAML, and it raises rather than
returning an empty block, so a behaviour assertion can never pass against
text that does not describe the shipped step.

    python3 tools/scripts/test_release_trailer_guard.py
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS.parent.parent
AUTO_RELEASE = REPO_ROOT / ".github" / "workflows" / "auto-release.yml"
GUARD = SCRIPTS / "release_trailer_guard.py"

sys.path.insert(0, str(SCRIPTS))

import gate_common  # noqa: E402
import release_trailer_guard  # noqa: E402
import version_bump_check  # noqa: E402


FENCED = 'docs: friction report\n\nThe grammar is:\n\n```\n%s\n```\n'
INDENTED = 'docs: guide\n\nThe grammar is:\n\n    %s\n'
BLOCK_QUOTED = 'docs: paste\n\n> %s\n'
DECLARED = 'fix(core): a real change\n\n%s\n'


def _git(repo: Path, *args: str, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], cwd=repo, capture_output=True, text=True, check=True, **kwargs
    )


def _fixture_repo(stack: tempfile.TemporaryDirectory, bodies: list[str]) -> Path:
    """A repo whose commits carry ``bodies``, with the guard's code vendored in."""
    repo = Path(stack) / "repo"
    (repo / "tools" / "scripts").mkdir(parents=True)
    for name in ("gate_common.py", "release_trailer_guard.py"):
        shutil.copy(SCRIPTS / name, repo / "tools" / "scripts" / name)
    _git(repo.parent, "init", "-q", "-b", "main", str(repo))
    _git(repo, "config", "user.email", "t@example.com")
    _git(repo, "config", "user.name", "t")
    _git(repo, "add", "tools")
    _git(repo, "commit", "-q", "-m", "chore: base")
    for index, body in enumerate(bodies):
        (repo / f"f{index}.txt").write_text(str(index), encoding="utf-8")
        _git(repo, "add", f"f{index}.txt")
        _git(repo, "commit", "-q", "-F", "-", input=body)
    return repo


class GuardClassification(unittest.TestCase):
    """The CLI's verdict on a body, independent of any workflow."""

    def _verdict(self, body: str, query: str = "release-skip") -> str:
        return release_trailer_guard.verdict(body, query)

    def test_a_fenced_release_skip_does_not_withhold_a_tag(self) -> None:
        body = FENCED % 'Release: skip reason="an example"'
        self.assertEqual(self._verdict(body), "no-skip")

    def test_a_declared_release_skip_withholds_a_tag(self) -> None:
        body = DECLARED % 'Release: skip reason="coordinated launch"'
        self.assertEqual(self._verdict(body), "skip")

    def test_an_indented_release_skip_does_not_withhold_a_tag(self) -> None:
        body = INDENTED % 'Release: skip reason="an indented example"'
        self.assertEqual(self._verdict(body), "no-skip")

    def test_a_block_quoted_release_skip_does_not_withhold_a_tag(self) -> None:
        body = BLOCK_QUOTED % 'Release: skip reason="quoted from the report"'
        self.assertEqual(self._verdict(body), "no-skip")

    def test_a_declaration_alongside_a_quoted_example_still_withholds(self) -> None:
        body = (
            'fix(core): a real change\n\n```\nRelease: skip reason="the example"\n```\n'
            '\nRelease: skip reason="the real declaration"\n'
        )
        self.assertEqual(self._verdict(body), "skip")

    def test_a_declaration_buried_by_a_squash_is_still_found(self) -> None:
        """A COMMIT_MESSAGES squash appends a footer after the source commits,
        so `git interpret-trailers` no longer sees a mid-body declaration."""
        body = (
            "fix(core): a real change\n\n"
            'Release: skip reason="declared on the branch"\n\n'
            "---------\n\nCo-authored-by: Someone <s@example.com>\n"
        )
        self.assertEqual(self._verdict(body), "skip")

    def test_a_fenced_version_bump_skip_does_not_withhold_a_tag(self) -> None:
        body = FENCED % 'Version-Bump: skip reason="an example"'
        self.assertEqual(self._verdict(body, "version-bump-skip"), "no-skip")

    def test_a_declared_version_bump_skip_withholds_a_tag(self) -> None:
        body = DECLARED % 'Version-Bump: skip reason="npm-versioned only"'
        self.assertEqual(self._verdict(body, "version-bump-skip"), "skip")

    def test_a_per_surface_skip_is_not_a_whole_release_opt_out(self) -> None:
        body = DECLARED % 'Version-Bump: sdk=skip reason="generated artifact"'
        self.assertEqual(self._verdict(body, "version-bump-skip"), "no-skip")

    def test_an_empty_reason_is_not_a_valid_bypass(self) -> None:
        for value in ('Version-Bump: skip reason=""', "Version-Bump: skip"):
            with self.subTest(value=value):
                self.assertEqual(
                    self._verdict(DECLARED % value, "version-bump-skip"), "no-skip"
                )

    def test_an_unreadable_ref_is_undecidable_rather_than_guessed(self) -> None:
        with tempfile.TemporaryDirectory() as stack:
            repo = _fixture_repo(stack, ["fix(core): x\n"])
            result = subprocess.run(
                [sys.executable, str(GUARD), "--query", "release-skip",
                 "--ref", "0000000000000000000000000000000000000000"],
                cwd=repo, capture_output=True, text=True,
            )
        self.assertEqual(result.returncode, release_trailer_guard.UNDECIDABLE)
        self.assertNotIn("skip", result.stdout)


class LockstepWithTheVersionBumpGate(unittest.TestCase):
    """auto-release.yml:`Skip on trailer or revert` and the PR-time gate must
    honour exactly the same `Version-Bump:` values.

    The comment in that step has always named the invariant — if the layers
    disagreed, an author could write a bypass that passes at PR time and fails
    post-merge, or the reverse. It was previously held by two hand-aligned
    patterns; this asserts the agreement itself, over the values that have
    distinguished them.
    """

    VALUES = [
        'Version-Bump: skip reason="npm-versioned only"',
        'Version-Bump: skip reason=""',
        "Version-Bump: skip",
        'Version-Bump: skip reason="   "',
        'Version-Bump: sdk=skip reason="generated artifact identical"',
        'Version-Bump: sdk=minor reason="additive"',
        'Version-Bump: SKIP reason="upper case is still a skip"',
    ]

    def test_both_layers_agree_on_every_value(self) -> None:
        for value in self.VALUES:
            with self.subTest(value=value), tempfile.TemporaryDirectory() as stack:
                repo = _fixture_repo(stack, [DECLARED % value])
                cwd = os.getcwd()
                os.chdir(repo)
                try:
                    pr_time = version_bump_check._range_has_version_bump_skip_trailer(
                        "HEAD~1", "HEAD"
                    )
                finally:
                    os.chdir(cwd)
                post_merge = subprocess.run(
                    [sys.executable, str(GUARD), "--query", "version-bump-skip",
                     "--ref", "HEAD"],
                    cwd=repo, capture_output=True, text=True, check=True,
                ).stdout.strip()
                self.assertEqual(
                    pr_time, post_merge == "skip",
                    f"PR-time gate says {pr_time}, post-merge tagger says "
                    f"{post_merge!r} for {value!r}",
                )

    def test_both_layers_read_the_same_grammar_implementation(self) -> None:
        """A shared implementation, not two that currently agree."""
        self.assertIs(
            version_bump_check.version_bump_skip_reason,
            gate_common.version_bump_skip_reason,
        )
        self.assertIs(
            release_trailer_guard.version_bump_skip_reason,
            gate_common.version_bump_skip_reason,
        )


GUARD_STEP_NAME = "Skip on trailer or revert"

# Stdlib only, deliberately. The lane that runs this as a ctest has no PyYAML,
# and guarding the import would turn the one test that proves the shipped
# workflow step behaves correctly into one that silently does not run — on the
# lane where that proof matters most. One known step's `run:` block out of one
# known file does not need a general YAML parser.
_STEP_RE = re.compile(
    rf"(?m)^(?P<indent>[ ]*)-[ ]+name:[ ]+{re.escape(GUARD_STEP_NAME)}[ ]*$"
)
_RUN_RE = re.compile(r"(?m)^(?P<indent>[ ]+)run:[ ]*\|[-+]?[ ]*$")

# What the extracted block must contain to be worth asserting anything about.
_GUARD_MARKERS = (
    "python3 tools/scripts/release_trailer_guard.py",
    "guard release-skip HEAD",
    "guard version-bump-skip HEAD",
)


def _step_block(text: str) -> str:
    """The lines belonging to the guard step, by indentation."""
    found = _STEP_RE.search(text)
    if found is None:
        raise AssertionError(
            f"auto-release.yml has no step named {GUARD_STEP_NAME!r}. If it was "
            "renamed, update GUARD_STEP_NAME — do not let this test go on "
            "extracting nothing."
        )
    item_indent = len(found.group("indent"))
    kept: list[str] = []
    for line in text[found.end():].split("\n"):
        if line.strip() and len(line) - len(line.lstrip(" ")) <= item_indent:
            break  # the next step, or the end of the step list
        kept.append(line)
    return "\n".join(kept)


def _guard_step_body() -> str:
    """The guard step's `run:` script, read out of the real workflow.

    Every way this can fail to find the real block RAISES. A rename or a
    restructure must break this test — an empty string handed back quietly
    would let every behaviour assertion below pass against nothing, which is
    the same shape as a skip that reads as a pass.
    """
    block = _step_block(AUTO_RELEASE.read_text(encoding="utf-8"))
    run = _RUN_RE.search(block)
    if run is None:
        raise AssertionError(
            f"the {GUARD_STEP_NAME!r} step has no `run: |` block scalar"
        )
    key_indent = len(run.group("indent"))
    kept: list[str] = []
    for line in block[run.end():].split("\n")[1:]:
        if not line.strip():
            kept.append("")
            continue
        if len(line) - len(line.lstrip(" ")) <= key_indent:
            break
        kept.append(line)
    body = textwrap.dedent("\n".join(kept))

    if not body.strip():
        raise AssertionError(
            f"extracted an EMPTY `run:` block from {GUARD_STEP_NAME!r}"
        )
    for marker in _GUARD_MARKERS:
        if marker not in body:
            raise AssertionError(
                f"the extracted guard step does not contain {marker!r}. Either "
                "the step stopped routing through release_trailer_guard.py, or "
                "this extraction is reading the wrong block — and in either "
                "case every behaviour assertion below would pass against text "
                "that does not describe the shipped step."
            )
    return body


class ExtractedWorkflowStep(unittest.TestCase):
    """Run the workflow's own guard step against real commits.

    Extracted from the YAML rather than transcribed: a transcribed copy is
    exactly what silently stopped matching the step it stood for.
    """

    def _run(self, body: str, *, break_guard: bool = False) -> tuple[int, dict, str]:
        with tempfile.TemporaryDirectory() as stack:
            repo = _fixture_repo(stack, [body])
            if break_guard:
                (repo / "tools" / "scripts" / "release_trailer_guard.py").unlink()
            output = Path(stack) / "step-output"
            output.write_text("", encoding="utf-8")
            env = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
            env["GITHUB_OUTPUT"] = str(output)
            result = subprocess.run(
                ["bash", "-c", _guard_step_body()],
                cwd=repo, env=env, capture_output=True, text=True,
            )
            parsed = dict(
                line.split("=", 1)
                for line in output.read_text(encoding="utf-8").splitlines()
                if "=" in line
            )
        return result.returncode, parsed, result.stdout + result.stderr

    def test_a_fenced_release_skip_does_not_suppress_the_tag(self) -> None:
        code, out, log = self._run(FENCED % 'Release: skip reason="an example"')
        self.assertEqual(code, 0, log)
        self.assertEqual(out.get("skip"), "0", log)

    def test_a_declared_release_skip_suppresses_the_tag(self) -> None:
        code, out, log = self._run(DECLARED % 'Release: skip reason="coordinated"')
        self.assertEqual(code, 0, log)
        self.assertEqual(out.get("skip"), "1", log)
        self.assertEqual(out.get("tracker_opt_out"), "1", log)

    def test_an_indented_release_skip_does_not_suppress_the_tag(self) -> None:
        code, out, log = self._run(INDENTED % 'Release: skip reason="an example"')
        self.assertEqual(code, 0, log)
        self.assertEqual(out.get("skip"), "0", log)

    def test_a_block_quoted_release_skip_does_not_suppress_the_tag(self) -> None:
        code, out, log = self._run(BLOCK_QUOTED % 'Release: skip reason="quoted"')
        self.assertEqual(code, 0, log)
        self.assertEqual(out.get("skip"), "0", log)

    def test_a_fenced_version_bump_skip_does_not_suppress_the_tag(self) -> None:
        code, out, log = self._run(FENCED % 'Version-Bump: skip reason="an example"')
        self.assertEqual(code, 0, log)
        self.assertEqual(out.get("skip"), "0", log)

    def test_a_declared_version_bump_skip_suppresses_the_tag(self) -> None:
        code, out, log = self._run(DECLARED % 'Version-Bump: skip reason="npm only"')
        self.assertEqual(code, 0, log)
        self.assertEqual(out.get("skip"), "1", log)

    def test_an_ordinary_commit_is_tagged(self) -> None:
        code, out, log = self._run("fix(core): an ordinary change\n")
        self.assertEqual(code, 0, log)
        self.assertEqual(out.get("skip"), "0", log)

    def test_a_revert_still_suppresses_the_tag(self) -> None:
        code, out, log = self._run('Revert "fix(core): a thing"\n')
        self.assertEqual(code, 0, log)
        self.assertEqual(out.get("skip"), "1", log)

    def test_an_undecidable_verdict_fails_loudly_instead_of_tagging(self) -> None:
        """A classification the guard cannot make must not resolve to either
        guess: one publishes an unwanted tag, the other withholds a wanted one
        with nothing reporting it."""
        code, out, log = self._run("fix(core): a change\n", break_guard=True)
        self.assertEqual(code, 1, log)
        self.assertNotEqual(out.get("skip"), "1", log)
        # Name the guard that could not decide. The step asks two questions in
        # sequence, so a bare `::error::` assertion passes on the SECOND one's
        # arm while the first silently falls through — which is the shape of
        # the defect, not a pass.
        self.assertIn("release trailer guard could not classify", log)


class NoSecondImplementation(unittest.TestCase):
    """The workflow must not grow its own trailer pattern back."""

    def test_the_extraction_is_the_whole_step_not_a_fragment(self) -> None:
        """A truncated block would still carry the guard markers near its top
        while dropping the tail the ordinary-commit and revert cases exercise,
        so those would pass against a step that never reaches its own end."""
        body = _guard_step_body()
        self.assertGreater(len(body.splitlines()), 50)
        self.assertIn('echo "skip=0" >> "$GITHUB_OUTPUT"', body)
        self.assertIn('if [[ "$subject" =~ ^[Rr]evert ]]; then', body)

    def test_the_guard_step_classifies_via_the_shared_parse(self) -> None:
        body = _guard_step_body()
        self.assertIn("release_trailer_guard.py", body)
        self.assertIn("guard release-skip HEAD", body)
        self.assertIn("guard version-bump-skip HEAD", body)

    def test_no_workflow_step_regexes_a_bypass_trailer_itself(self) -> None:
        text = AUTO_RELEASE.read_text(encoding="utf-8")
        for pattern in ("^release: skip", "release:[[:space:]]+skip",
                        "version-bump:[[:space:]]+skip"):
            with self.subTest(pattern=pattern):
                self.assertNotIn(
                    pattern, text,
                    "auto-release.yml is matching a bypass trailer with its own "
                    "pattern again; route it through release_trailer_guard.py so "
                    "it cannot drift from the pre-merge gates.",
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
