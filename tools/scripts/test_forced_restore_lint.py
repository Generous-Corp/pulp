#!/usr/bin/env python3
"""Tests for forced_restore_lint.py.

The load-bearing pairs are the twins: the SAME forced restore, once bare and
once with a declared reason, and the SAME primitive, once intact and once with
its index drop removed. A gate that never fires and a gate that always fires
both look green on a clean tree, so each positive case has a negative twin that
differs only in the thing the gate claims to read.

The idiom corpus is checked for sensitivity too. A lint whose regex silently
stopped matching one of the three restore forms would keep passing on the real
repository forever, because the repository contains one annotated call site and
nothing else to notice the loss.
"""

from __future__ import annotations

import importlib.util
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _repo_root() -> Path:
    return Path(
        subprocess.run(
            ["git", "-C", str(HERE), "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )


_spec = importlib.util.spec_from_file_location(
    "forced_restore_lint", HERE / "forced_restore_lint.py")
lint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lint)


# Every idiom that re-materialises the tree in place, so a clean stat cache can
# swallow the whole command.
BARE_RESTORES = (
    'git -C "$dir" checkout --force "$ref" -- .',
    'git -C "$dir" checkout -f "$ref" -- .',
    "git -C \"$dir\" checkout --force -- src/",
    'git -C "$dir" checkout-index -f -a',
    'git -C "$dir" read-tree --reset -u HEAD',
)

# Commands that look similar and are not the defect.
NON_RESTORES = (
    # Moves the ref, so git sees a real difference no stat cache can hide.
    "git -C $Repo checkout --force -B main FETCH_HEAD",
    'git -C "$dir" checkout --detach "$ref"',
    'git -C "$dir" checkout "$ref"',
    'git -C "$dir" read-tree HEAD',
    "git -C \"$dir\" reset --hard",
)

ANNOTATION = '# forced-restore: content-absent reason="the clone could not read the blobs"'

INTACT_PRIMITIVE = '''restore_source_cache_verbatim_eol() {
    local dir="$1"
    local index_path
    index_path="$(git -C "$dir" rev-parse --git-path index)" || return 1
    case "$index_path" in /*) ;; *) index_path="$dir/$index_path" ;; esac
    rm -f "$index_path"
    git -C "$dir" reset --hard --quiet
}
'''


class IdiomSensitivity(unittest.TestCase):
    """Each restore form is caught, and each look-alike is left alone."""

    def test_every_bare_restore_is_flagged(self) -> None:
        for command in BARE_RESTORES:
            with self.subTest(command=command):
                self.assertEqual(len(lint.scan_text(command, "setup.sh")), 1)

    def test_no_look_alike_is_flagged(self) -> None:
        for command in NON_RESTORES:
            with self.subTest(command=command):
                self.assertEqual(lint.scan_text(command, "setup.sh"), [])


class ReasonAnnotation(unittest.TestCase):
    """The twins: the same restore, bare and declared."""

    def test_bare_restore_is_flagged(self) -> None:
        found = lint.scan_text(BARE_RESTORES[0], "setup.sh")
        self.assertEqual(len(found), 1)
        self.assertIn("no declared reason", found[0])

    def test_declared_twin_is_clean(self) -> None:
        body = f"{ANNOTATION}\n{BARE_RESTORES[0]}"
        self.assertEqual(lint.scan_text(body, "setup.sh"), [])

    def test_annotation_may_wrap_across_lines(self) -> None:
        body = (
            "# forced-restore: content-absent reason=\"the completeness check just\n"
            "# reported the blobs missing, so they are absent and not converted\"\n"
            f"{BARE_RESTORES[0]}"
        )
        self.assertEqual(lint.scan_text(body, "setup.sh"), [])

    def test_annotation_further_up_does_not_reach(self) -> None:
        """A reason has to sit with the command it excuses, not somewhere above."""
        body = f"{ANNOTATION}\n" + ("echo padding\n" * 4) + BARE_RESTORES[0]
        self.assertEqual(len(lint.scan_text(body, "setup.sh")), 1)

    def test_any_other_reason_is_rejected(self) -> None:
        body = '# forced-restore: stale-cache reason="it looked wrong"\n' + BARE_RESTORES[0]
        found = lint.scan_text(body, "setup.sh")
        self.assertEqual(len(found), 1)
        self.assertIn("stale-cache", found[0])
        self.assertIn("restore_source_cache_verbatim_eol", found[0])

    def test_prose_about_the_idiom_is_not_a_use_of_it(self) -> None:
        body = "# `git checkout --force -- .` is NOT enough and fails silently"
        self.assertEqual(lint.scan_text(body, "setup.sh"), [])


class PrimitiveStructure(unittest.TestCase):
    """The half an author cannot satisfy by writing a comment."""

    def test_intact_primitive_is_clean(self) -> None:
        self.assertEqual(lint.check_primitive(INTACT_PRIMITIVE), [])

    def test_missing_primitive_is_flagged(self) -> None:
        found = lint.check_primitive("echo nothing here\n")
        self.assertEqual(len(found), 1)
        self.assertIn("is gone", found[0])

    def test_dropping_the_index_delete_is_flagged(self) -> None:
        broken = INTACT_PRIMITIVE.replace('    rm -f "$index_path"\n', "")
        found = lint.check_primitive(broken)
        self.assertTrue(any("no longer deletes the index" in f for f in found))

    def test_dropping_the_index_lookup_is_flagged(self) -> None:
        broken = INTACT_PRIMITIVE.replace(
            'index_path="$(git -C "$dir" rev-parse --git-path index)" || return 1',
            'index_path="$dir/.git/index"',
        )
        found = lint.check_primitive(broken)
        self.assertTrue(any("no longer resolves the index path" in f for f in found))

    def test_dropping_the_rebuild_is_flagged(self) -> None:
        broken = INTACT_PRIMITIVE.replace('    git -C "$dir" reset --hard --quiet\n', "")
        found = lint.check_primitive(broken)
        self.assertTrue(any("no longer re-materialises" in f for f in found))

    def test_primitive_rewritten_as_the_idiom_it_replaced_is_flagged(self) -> None:
        """The regression this gate exists to stop, stated as one case."""
        broken = INTACT_PRIMITIVE.replace(
            '    rm -f "$index_path"\n    git -C "$dir" reset --hard --quiet\n',
            '    git -C "$dir" checkout --force -- .\n',
        )
        self.assertNotEqual(lint.check_primitive(broken), [])


class RealRepository(unittest.TestCase):
    """The tree this lint ships in is clean, and the scan actually read it."""

    def test_repository_is_clean(self) -> None:
        self.assertEqual(lint.run(_repo_root()), 0)

    def test_the_real_call_site_would_fire_without_its_reason(self) -> None:
        """A clean tree is only evidence if the scan can still fail on it.

        Stripping the annotation off the real setup.sh has to produce a finding.
        That proves the scan reaches the one call site the repository has, rather
        than passing because it read nothing.
        """
        setup = (_repo_root() / "setup.sh").read_text(encoding="utf-8")
        self.assertIn("forced-restore: content-absent", setup)
        stripped = "\n".join(
            line for line in setup.splitlines() if "forced-restore:" not in line
        )
        found = lint.scan_text(stripped, "setup.sh")
        self.assertEqual(len(found), 1, found)
        self.assertIn("no declared reason", found[0])




class ProseAndFixtures(unittest.TestCase):
    """The two ways this gate could go quiet against its own source tree.

    Both were found by scanning the lint's own files rather than by reasoning
    about them: the scan is only trustworthy if it stays sensitive to a live
    command in exactly the files it has to hold examples of one.
    """

    def test_docstring_prose_naming_the_idiom_is_not_flagged(self) -> None:
        doc = '"""Why `git checkout --force HEAD -- .` cannot repair a cache."""\n'
        self.assertEqual(lint.scan_text(doc + "x = 1\n", "note.py"), [])

    def test_a_live_command_beside_that_prose_is_still_flagged(self) -> None:
        text = (
            '"""Why `git checkout --force HEAD -- .` cannot repair a cache."""\n'
            'subprocess.run("git checkout --force HEAD -- .", shell=True)\n'
        )
        found = lint.scan_text(text, "note.py")
        self.assertEqual(len(found), 1, found)
        self.assertIn(":2:", found[0])

    def test_a_one_line_docstring_does_not_swallow_the_line_after_it(self) -> None:
        text = '"""One line."""\nrun("git checkout --force HEAD -- .")\n'
        self.assertEqual(len(lint.scan_text(text, "note.py")), 1)

    def test_an_inline_triple_quoted_command_is_not_prose(self) -> None:
        text = 'subprocess.run("""git checkout --force HEAD -- .""", shell=True)\n'
        self.assertEqual(
            len(lint.scan_text(text, "note.py")),
            1,
            "only a line that OPENS with the quote is a docstring",
        )

    def test_shell_gets_no_docstring_exemption(self) -> None:
        text = "# note\ngit checkout --force HEAD -- .\n"
        self.assertEqual(len(lint.scan_text(text, "note.sh")), 1)

    def test_the_lint_is_clean_against_its_own_source(self) -> None:
        own = Path(lint.__file__)
        text = own.read_text(encoding="utf-8")
        self.assertIn("checkout --force", text, "the docstring must still name the idiom")
        self.assertEqual(lint.scan_text(text, "tools/scripts/forced_restore_lint.py"), [])

    def test_the_fixture_exemption_is_scope_not_a_hole_in_the_detector(self) -> None:
        fixtures = Path(_repo_root()) / lint._FIXTURES
        self.assertTrue(fixtures.is_file(), fixtures)
        found = lint.scan_text(fixtures.read_text(encoding="utf-8"), "x.py")
        self.assertGreater(
            len(found),
            0,
            "scan_text must still fire on the fixtures the sweep skips by name",
        )


if __name__ == "__main__":
    unittest.main()
