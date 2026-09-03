"""Tests for the Yoga oracle/pin lockstep check (check_yoga_oracle_pin.py)."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.scripts import check_yoga_oracle_pin as cyop  # noqa: E402


# A cmake fixture shaped like the real file: another dependency pinned with its
# own GIT_TAG both before and after the Yoga block, so a parser that grabs the
# first GIT_TAG in the file fails this fixture.
def _cmake(register_ref: str, git_tag: str) -> str:
    return f"""
pulp_register_fetchcontent_source(lv2 REF v1.18.10)
FetchContent_Declare(
    lv2
    GIT_REPOSITORY https://github.com/lv2/lv2.git
    GIT_TAG v1.18.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lv2)

# Facebook Yoga (MIT license) — cross-platform CSS Flexbox/Grid layout engine
pulp_register_fetchcontent_source(yoga REF {register_ref})
FetchContent_Declare(
    yoga
    GIT_REPOSITORY https://github.com/facebook/yoga.git
    GIT_TAG {git_tag}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR yoga
)
FetchContent_MakeAvailable(yoga)

pulp_register_fetchcontent_source(highway REF 1.2.0)
FetchContent_Declare(
    highway
    GIT_REPOSITORY https://github.com/google/highway.git
    GIT_TAG 1.2.0
)
"""


def _oracle(version: str, source_ref: str) -> str:
    return json.dumps({
        "version": version,
        "source": (
            "tools/import-design/catalogs/yoga.tsv + "
            "https://www.yogalayout.dev/docs/styling + "
            f"facebook/yoga@{source_ref} YGEnums.h"
        ),
        "properties": {"flexGrow": {"kind": "number"}},
    })


class YogaOraclePinTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def _write(self, name: str, text: str) -> Path:
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        return path

    # --- repo-state guard ---------------------------------------------------

    def test_live_repo_is_in_lockstep(self) -> None:
        self.assertEqual(cyop.main([]), 0)

    # --- pin parsing --------------------------------------------------------

    def test_read_pin_returns_the_yoga_ref(self) -> None:
        path = self._write("deps.cmake", _cmake("v3.2.1", "v3.2.1"))
        self.assertEqual(cyop.read_pin(path), "v3.2.1")

    def test_read_pin_ignores_other_dependencies_git_tags(self) -> None:
        # lv2 (v1.18.10) is declared before Yoga and highway (1.2.0) after.
        path = self._write("deps.cmake", _cmake("v9.9.9", "v9.9.9"))
        self.assertEqual(cyop.read_pin(path), "v9.9.9")

    def test_read_pin_rejects_disagreeing_ref_and_git_tag(self) -> None:
        path = self._write("deps.cmake", _cmake("v3.2.1", "v3.3.0"))
        with self.assertRaises(cyop.CheckError) as ctx:
            cyop.read_pin(path)
        self.assertIn("v3.2.1", str(ctx.exception))
        self.assertIn("v3.3.0", str(ctx.exception))

    def test_read_pin_rejects_missing_register_call(self) -> None:
        text = _cmake("v3.2.1", "v3.2.1").replace(
            "pulp_register_fetchcontent_source(yoga REF v3.2.1)", "")
        path = self._write("deps.cmake", text)
        with self.assertRaises(cyop.CheckError):
            cyop.read_pin(path)

    def test_read_pin_rejects_missing_git_tag(self) -> None:
        text = _cmake("v3.2.1", "v3.2.1").replace(
            "    GIT_TAG v3.2.1\n    GIT_SHALLOW TRUE\n    SOURCE_SUBDIR yoga\n",
            "    GIT_SHALLOW TRUE\n    SOURCE_SUBDIR yoga\n")
        path = self._write("deps.cmake", text)
        with self.assertRaises(cyop.CheckError):
            cyop.read_pin(path)

    def test_read_pin_errors_name_the_file_they_were_given(self) -> None:
        text = _cmake("v3.2.1", "v3.2.1").replace(
            "FetchContent_Declare(\n    yoga", "FetchContent_Declare(\n    yoghurt")
        path = self._write("deps.cmake", text)
        with self.assertRaises(cyop.CheckError) as ctx:
            cyop.read_pin(path)
        self.assertIn(str(path), str(ctx.exception))

    def test_read_pin_rejects_missing_file(self) -> None:
        with self.assertRaises(cyop.CheckError):
            cyop.read_pin(self.root / "absent.cmake")

    # --- oracle parsing -----------------------------------------------------

    def test_read_oracle_returns_both_stamps(self) -> None:
        path = self._write("oracle.json", _oracle("v3.2.1", "v3.2.1"))
        self.assertEqual(
            cyop.read_oracle(path),
            {"version": "v3.2.1", "source_ref": "v3.2.1"},
        )

    def test_read_oracle_rejects_empty_version(self) -> None:
        path = self._write("oracle.json", _oracle("   ", "v3.2.1"))
        with self.assertRaises(cyop.CheckError):
            cyop.read_oracle(path)

    def test_read_oracle_rejects_source_without_upstream_citation(self) -> None:
        path = self._write("oracle.json", json.dumps(
            {"version": "v3.2.1", "source": "the docs website", "properties": {}}))
        with self.assertRaises(cyop.CheckError):
            cyop.read_oracle(path)

    def test_read_oracle_rejects_invalid_json(self) -> None:
        path = self._write("oracle.json", "{not json")
        with self.assertRaises(cyop.CheckError):
            cyop.read_oracle(path)

    # --- comparison ---------------------------------------------------------

    def test_compare_is_quiet_in_lockstep(self) -> None:
        self.assertEqual(
            cyop.compare("v3.2.1", {"version": "v3.2.1", "source_ref": "v3.2.1"}),
            [],
        )

    def test_compare_reports_version_drift(self) -> None:
        drift = cyop.compare(
            "v3.3.0", {"version": "v3.2.1", "source_ref": "v3.3.0"})
        self.assertEqual(len(drift), 1)
        self.assertIn("'version' stamp", drift[0])
        self.assertIn("v3.2.1", drift[0])
        self.assertIn("v3.3.0", drift[0])

    def test_compare_reports_source_citation_drift(self) -> None:
        drift = cyop.compare(
            "v3.3.0", {"version": "v3.3.0", "source_ref": "v3.2.1"})
        self.assertEqual(len(drift), 1)
        self.assertIn("'source' upstream citation", drift[0])

    # --- exit codes ---------------------------------------------------------

    def test_main_returns_1_on_drift(self) -> None:
        with mock.patch.object(cyop, "read_pin", return_value="v3.3.0"):
            self.assertEqual(cyop.main([]), 1)

    def test_main_returns_2_on_unparseable_input(self) -> None:
        with mock.patch.object(cyop, "read_oracle",
                               side_effect=cyop.CheckError("nope")):
            self.assertEqual(cyop.main([]), 2)


if __name__ == "__main__":
    unittest.main()
