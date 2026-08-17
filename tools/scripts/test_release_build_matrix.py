#!/usr/bin/env python3
"""Tests for release_build_matrix.py — the active_platforms → matrix-legs map."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from release_artifact_contents import DEFAULT_MATRIX_PATH, ProductMatrix
from release_build_matrix import (
    BUILD_LEGS,
    MatrixLegError,
    build_include,
    main,
    smoke_include,
)

FULL_PLATFORMS = [
    "darwin-arm64",
    "darwin-x64",
    "linux-arm64",
    "linux-x64",
    "windows-arm64",
    "windows-x64",
]


def matrix_with(active: list[str] | None) -> ProductMatrix:
    doc = json.loads(DEFAULT_MATRIX_PATH.read_text(encoding="utf-8"))
    doc["platforms"] = FULL_PLATFORMS
    if active is None:
        doc.pop("active_platforms", None)
    else:
        doc["active_platforms"] = active
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
        handle.write(json.dumps(doc))
        path = Path(handle.name)
    try:
        return ProductMatrix.load(path)
    finally:
        path.unlink()


class ActiveSubsetFiltering(unittest.TestCase):
    def test_arm_only_yields_exactly_one_leg(self) -> None:
        matrix = matrix_with(["darwin-arm64"])
        build = build_include(matrix)
        self.assertEqual(
            build, [{"platform": "darwin-arm64", "os": "macos-15", "artifact": "pulp"}]
        )
        self.assertEqual(
            smoke_include(matrix),
            [{"platform": "darwin-arm64", "os": "macos-15", "artifact": "pulp"}],
        )

    def test_full_set_restores_all_six_legs(self) -> None:
        # The flip back: deleting the field (or listing everything) must
        # restore every leg — a knob that cannot be flipped back is worse
        # than no knob.
        for active in (None, FULL_PLATFORMS):
            with self.subTest(active=active):
                matrix = matrix_with(active)
                build = build_include(matrix)
                self.assertEqual(
                    [leg["platform"] for leg in build], list(BUILD_LEGS)
                )
                self.assertEqual(len(build), 6)
                self.assertEqual(len(smoke_include(matrix)), 6)

    def test_piecemeal_addition_is_one_list_entry(self) -> None:
        matrix = matrix_with(["darwin-arm64", "windows-arm64"])
        self.assertEqual(
            [leg["platform"] for leg in build_include(matrix)],
            ["darwin-arm64", "windows-arm64"],
        )

    def test_linux_x64_container_reaches_build_but_never_smoke(self) -> None:
        matrix = matrix_with(FULL_PLATFORMS)
        build = {leg["platform"]: leg for leg in build_include(matrix)}
        smoke = {leg["platform"]: leg for leg in smoke_include(matrix)}
        self.assertEqual(build["linux-x64"]["container"], "ubuntu:22.04")
        self.assertNotIn("container", smoke["linux-x64"])

    def test_darwin_x64_stays_on_the_xcompile_sentinel(self) -> None:
        matrix = matrix_with(FULL_PLATFORMS)
        build = {leg["platform"]: leg for leg in build_include(matrix)}
        self.assertEqual(build["darwin-x64"]["os"], "macos-15-xcompile")

    def test_windows_legs_carry_the_exe_artifact(self) -> None:
        matrix = matrix_with(FULL_PLATFORMS)
        for leg in build_include(matrix):
            expected = "pulp.exe" if leg["platform"].startswith("windows-") else "pulp"
            self.assertEqual(leg["artifact"], expected)

    def test_inventory_platform_without_leg_config_fails_loudly(self) -> None:
        doc = json.loads(DEFAULT_MATRIX_PATH.read_text(encoding="utf-8"))
        doc["platforms"] = FULL_PLATFORMS + ["freebsd-x64"]
        doc["active_platforms"] = ["darwin-arm64"]
        with tempfile.NamedTemporaryFile(
            "w", suffix=".json", delete=False
        ) as handle:
            handle.write(json.dumps(doc))
            path = Path(handle.name)
        try:
            matrix = ProductMatrix.load(path)
        finally:
            path.unlink()
        with self.assertRaises(MatrixLegError):
            build_include(matrix)

    def test_repo_matrix_resolves(self) -> None:
        # Whatever the checked-in knob currently says, it must expand to at
        # least one leg and to configs the workflow can consume.
        matrix = ProductMatrix.load(DEFAULT_MATRIX_PATH)
        build = build_include(matrix)
        self.assertGreaterEqual(len(build), 1)
        for leg in build:
            self.assertIn("platform", leg)
            self.assertIn("os", leg)
            self.assertIn("artifact", leg)


class GithubOutputFormat(unittest.TestCase):
    def test_github_output_lines_parse_as_json(self) -> None:
        import contextlib
        import io

        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            self.assertEqual(main(["--github-output"]), 0)
        lines = dict(
            line.split("=", 1) for line in buffer.getvalue().splitlines() if line
        )
        self.assertEqual(
            set(lines), {"active_platforms", "build_include", "smoke_include"}
        )
        for value in lines.values():
            json.loads(value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
