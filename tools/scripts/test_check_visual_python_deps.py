#!/usr/bin/env python3
"""Unit tests for the visual-analysis dependency presence check.

Pure stdlib on purpose: this is the selftest of the one check that must not
skip, so it must not acquire a dependency that could make it skip.
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_visual_python_deps as CHECK  # noqa: E402

# Two names no distribution can supply, so their absence is a property of the
# test rather than of the machine it runs on.
ABSENT_A = "pulp-absent-dependency-a"
ABSENT_B = "pulp-absent-dependency-b"


def write_requirements(body: str) -> Path:
    handle = tempfile.NamedTemporaryFile(
        "w", suffix=".txt", delete=False, encoding="utf-8"
    )
    handle.write(body)
    handle.close()
    return Path(handle.name)


class DeclaredDistributionsTests(unittest.TestCase):
    def test_strips_specifiers_comments_and_blank_lines(self) -> None:
        requirements = write_requirements(
            "# a comment\n"
            "\n"
            "numpy>=1.24\n"
            "Pillow>=10.0  # trailing comment\n"
            "scikit-image>=0.22\n"
        )
        self.assertEqual(
            CHECK.declared_distributions(requirements),
            ["numpy", "Pillow", "scikit-image"],
        )

    def test_skips_pip_option_lines(self) -> None:
        requirements = write_requirements("--index-url https://example.invalid\nnumpy\n")
        self.assertEqual(CHECK.declared_distributions(requirements), ["numpy"])

    def test_empty_file_declares_nothing(self) -> None:
        self.assertEqual(CHECK.declared_distributions(write_requirements("# only\n")), [])


class ImportNameTests(unittest.TestCase):
    def test_maps_distributions_whose_import_name_differs(self) -> None:
        self.assertEqual(CHECK.import_name("Pillow"), "PIL")
        self.assertEqual(CHECK.import_name("scikit-image"), "skimage")
        self.assertEqual(CHECK.import_name("opencv-python"), "cv2")

    def test_falls_back_to_the_normalized_distribution_name(self) -> None:
        self.assertEqual(CHECK.import_name("numpy"), "numpy")
        self.assertEqual(CHECK.import_name("Some-Other-Dist"), "some_other_dist")


class MissingDistributionsTests(unittest.TestCase):
    def test_reports_every_missing_distribution_not_just_the_first(self) -> None:
        missing = CHECK.missing_distributions([ABSENT_A, "sys", ABSENT_B])
        self.assertEqual(missing, [ABSENT_A, ABSENT_B])

    def test_reports_nothing_when_every_distribution_imports(self) -> None:
        # The control for the assertion above: the same call on importable
        # names must come back empty, so an empty result proves presence
        # rather than proving the check never looked.
        self.assertEqual(CHECK.missing_distributions(["sys", "json"]), [])


def run_main(*args: str) -> tuple[int, str, str]:
    """Run the check with its streams captured.

    The check reports on every path, and this selftest is a registered test of
    its own, so letting that through would put one test's verdict in another
    test's log. Capturing it also makes the message itself assertable.
    """
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = CHECK.main(list(args))
    return code, out.getvalue(), err.getvalue()


class MainTests(unittest.TestCase):
    def test_fails_when_a_declared_distribution_is_missing(self) -> None:
        requirements = write_requirements(f"{ABSENT_A}>=1.0\n{ABSENT_B}\n")
        code, _, err = run_main("--requirements", str(requirements))
        self.assertEqual(code, 1)
        # Both names, not just the first: one run has to name the whole gap,
        # or closing it turns into one install per round trip.
        self.assertIn(ABSENT_A, err)
        self.assertIn(ABSENT_B, err)

    def test_fails_on_an_empty_declaration_rather_than_passing_vacuously(self) -> None:
        requirements = write_requirements("# nothing declared\n")
        code, _, err = run_main("--requirements", str(requirements))
        self.assertEqual(code, 1)
        self.assertIn("declares no dependencies", err)

    def test_fails_when_the_requirements_file_is_absent(self) -> None:
        missing = Path(tempfile.gettempdir()) / "pulp-no-such-requirements-file.txt"
        code, _, err = run_main("--requirements", str(missing))
        self.assertEqual(code, 1)
        self.assertIn(str(missing), err)

    def test_succeeds_when_every_declared_distribution_is_present(self) -> None:
        requirements = write_requirements("sys\njson\n")
        code, out, _ = run_main("--requirements", str(requirements))
        self.assertEqual(code, 0)
        self.assertIn("OK", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
