#!/usr/bin/env python3
"""Tests for tools/scripts/undefined_names_check.py, and the import-validation scan."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import undefined_names_check as check  # noqa: E402

REPO = HERE.parent.parent


def found(source: str) -> list[tuple[int, str]] | None:
    return check.undefined_names(textwrap.dedent(source), "<fixture>")


class RepositoryScanTests(unittest.TestCase):
    def test_import_validation_has_no_undefined_names(self) -> None:
        count, problems, _ = check.scan([REPO / "tools" / "import-validation"])
        self.assertGreater(count, 0, "the scan found no Python files")
        self.assertEqual(problems, [])


class DetectionTests(unittest.TestCase):
    def test_a_name_an_extraction_left_behind_is_reported_where_it_is_read(self) -> None:
        # The shape of the importer-lab regression: the assignment moved into a
        # call, a helper takes the value as a parameter, and a later read in the
        # caller is left with nothing bound.
        source = """\
            def verify(capture, digest):
                if capture != digest:
                    raise ValueError(digest)

            def compare(source):
                verify(source, hash(source))
                return {"digest": digest}
        """
        # Only the caller's read is undefined; the helper's parameter of the
        # same name is not.
        self.assertEqual(found(source), [(7, "digest")])

    def test_control_the_same_code_with_the_binding_is_clean(self) -> None:
        source = """\
            def verify(capture, digest):
                if capture != digest:
                    raise ValueError(digest)

            def compare(source):
                digest = hash(source)
                verify(source, digest)
                return {"digest": digest}
        """
        self.assertEqual(found(source), [])

    def test_an_undefined_module_level_read_is_reported(self) -> None:
        self.assertEqual(found("x = 1\nprint(y)\n"), [(2, "y")])

    def test_a_class_body_read_is_checked(self) -> None:
        self.assertEqual(found("class A:\n    size = LIMIT\n"), [(2, "LIMIT")])

    def test_a_method_cannot_read_a_class_attribute_by_bare_name(self) -> None:
        source = """\
            class A:
                size = 3
                def grow(self):
                    return size + 1
        """
        self.assertEqual(found(source), [(4, "size")])


class FalsePositiveControls(unittest.TestCase):
    def test_ordinary_bindings_are_clean(self) -> None:
        source = """\
            import os
            from pathlib import Path as P
            try:
                from PIL import Image
            except ImportError:
                Image = None
            COUNT = 0

            def bump():
                global COUNT
                COUNT += 1
                return COUNT

            def late():
                return LATE

            LATE = 1

            class Box:
                size = len("ab")

                def area(self):
                    return self.size * self.size

            def uses_everything(items, *rest, flag=False, **extra):
                squares = [i * i for i in items]
                pairs = {k: v for k, v in extra.items()}
                total = sum(x for x in rest)
                if (n := len(items)) > 1:
                    pass
                with open(__file__, encoding="utf-8") as handle:
                    handle.read()
                for index, item in enumerate(items):
                    del item
                try:
                    pass
                except OSError as error:
                    print(error, __name__, os.sep, P, Image, Box, n, index)
                outer = 1

                def inner():
                    return outer + squares[0] + len(pairs) + total + flag

                return inner, late, bump
        """
        self.assertEqual(found(source), [])

    def test_a_star_import_is_skipped_not_passed(self) -> None:
        self.assertIsNone(found("from os.path import *\nprint(join)\n"))
        with tempfile.TemporaryDirectory() as raw:
            Path(raw, "star.py").write_text("from os import *\n", encoding="utf-8")
            count, problems, skipped = check.scan([Path(raw)])
        self.assertEqual((count, problems), (1, []))
        self.assertEqual(len(skipped), 1)


class CommandLineTests(unittest.TestCase):
    def run_main(self, *roots: Path) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return check.main([str(r) for r in roots])

    def test_a_planted_undefined_name_fails_the_scan(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            Path(raw, "ok.py").write_text("def f():\n    return 1\n", encoding="utf-8")
            Path(raw, "bad.py").write_text("def f():\n    return missing\n", encoding="utf-8")
            self.assertEqual(self.run_main(Path(raw)), 1)

    def test_control_a_clean_tree_passes(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            Path(raw, "ok.py").write_text("def f():\n    return 1\n", encoding="utf-8")
            self.assertEqual(self.run_main(Path(raw)), 0)

    def test_an_empty_scan_is_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            self.assertEqual(self.run_main(Path(raw)), 2)


if __name__ == "__main__":
    unittest.main()
