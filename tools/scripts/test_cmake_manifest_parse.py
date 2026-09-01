#!/usr/bin/env python3
"""Every CTest manifest must parse, including the ones no CI lane configures.

`test/cmake/*.cmake` is included by `test/CMakeLists.txt`, but most of it sits
behind an `option()`. A manifest guarded by an option that no lane turns on with
`PULP_BUILD_TESTS=ON` is never parsed by anything, so a syntax error in it is
invisible: the configure that would reject it never runs.

That is not hypothetical. Four Scene3D manifests carried an unbalanced paren from
`ba72508e2c` (2026-07-06) and stayed broken on `main` for eight weeks. Every lane
that sets `PULP_ENABLE_SCENE3D=ON` also sets `PULP_BUILD_TESTS=OFF`, so nothing
ever read them. They were found by hand, not by a gate.

A syntax error is exactly the class a cheap check catches, so check it directly
instead of relying on some lane happening to configure the right combination.
Wrapping a manifest in `if(FALSE)` makes CMake parse the whole file while
executing none of it, so `add_test`/`target_link_libraries` calls that would fail
outside a project context do not need one.

Scope: this is a PARSE check. It sees unbalanced parens, unterminated strings, and
unclosed blocks — the things that make a file unreadable. It does not see semantic
errors (a misspelled command, a target that does not exist); those need a real
configure and are already caught the moment a lane enables the option.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
TEST_DIR = REPO_ROOT / "test"

# Enough manifests that a typo in the glob would be obvious, and a floor that
# fails loudly if the tree is ever moved out from under this test.
MINIMUM_EXPECTED_MANIFESTS = 50


def manifests() -> list[Path]:
    return sorted(TEST_DIR.rglob("*.cmake"))


def parse_error(cmake: str, source: Path | str, workdir: Path) -> str | None:
    """`None` if CMake can parse `source`, else its error text.

    `source` is a path to read or the manifest text itself.
    """
    text = source.read_text(encoding="utf-8") if isinstance(source, Path) else source
    probe = workdir / "probe.cmake"
    probe.write_text(f"if(FALSE)\n{text}\nendif()\n", encoding="utf-8")
    done = subprocess.run(
        [cmake, "-P", str(probe)], capture_output=True, text=True, check=False
    )
    if done.returncode == 0:
        return None
    return (done.stderr or done.stdout).strip()


class CMakeManifestsParse(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = shutil.which("cmake")
        if cls.cmake is None:
            # Deliberately not a skip. This test exists because a check nobody ran
            # read as a passing check for eight weeks; a silent skip would restore
            # exactly that. Anything that runs this has cmake.
            raise AssertionError(
                "cmake is not on PATH, so no manifest was parsed. This test cannot "
                "pass without checking something."
            )

    def test_probe_reports_a_real_parse_error(self) -> None:
        """Guard the guard: a probe that cannot fail would report a clean tree forever."""
        with tempfile.TemporaryDirectory() as td:
            work = Path(td)
            self.assertIsNone(
                parse_error(self.cmake, 'add_test(NAME ok COMMAND "${CMAKE_COMMAND}")', work),
                "The probe rejected a well-formed manifest, so every result it "
                "produces is noise.",
            )
            # The shape that actually shipped: an unbalanced opening paren.
            broken = parse_error(
                self.cmake, 'add_test((NAME broken COMMAND "${CMAKE_COMMAND}")', work
            )
            self.assertIsNotNone(
                broken,
                "The probe accepted an unbalanced paren. Wrapping a manifest in "
                "`if(FALSE)` must still parse it, or this check sees nothing.",
            )
            self.assertIn("Parse error", broken)

    def test_every_manifest_parses(self) -> None:
        found = manifests()
        self.assertGreaterEqual(
            len(found),
            MINIMUM_EXPECTED_MANIFESTS,
            f"Only {len(found)} manifests found under {TEST_DIR}. This test's "
            f"discovery has drifted and it is no longer checking the tree.",
        )

        broken: list[str] = []
        with tempfile.TemporaryDirectory() as td:
            work = Path(td)
            for path in found:
                error = parse_error(self.cmake, path, work)
                if error is not None:
                    broken.append(f"{path.relative_to(REPO_ROOT)}\n    {error}")

        self.assertEqual(
            broken,
            [],
            "A CTest manifest does not parse. If it sits behind an option no lane "
            "enables alongside PULP_BUILD_TESTS=ON, nothing else will tell you:\n\n"
            + "\n".join(f"  {entry}" for entry in broken),
        )


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
