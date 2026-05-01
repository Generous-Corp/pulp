#!/usr/bin/env python3
"""Fixture tests for scripts/run_coverage.sh.

Covers the two bugs fixed in issue #566 Phase 1 PR 1:

- #570: silent failure when CMakeCache.txt was previously configured
  with PULP_ENABLE_COVERAGE:BOOL=OFF. The script must error loudly
  (exit 3) with a helpful remediation message instead of silently
  producing empty profdata.
- #569: Catch2-path / _deps/ / build-coverage/ spam from llvm-cov
  show and gcovr. The canonical `COVERAGE_IGNORE_REGEX` in the
  script must match the noisy paths and NOT match the paths we
  actually want to report on.

We intentionally test the script without running a full build —
that's minutes of CI time and the real dependency (Clang toolchain)
already runs in `.github/workflows/coverage.yml`. Instead we exercise:

  1. The post-configure cache assert by simulating a stale cache via
     a stub `cmake` binary on PATH that leaves a bad cache in place.
  2. The ignore-regex by grepping the canonical constant from the
     script and running it against representative path fixtures.

Run:
    python3 tools/scripts/test_run_coverage.py
"""

from __future__ import annotations

import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "scripts" / "run_coverage.sh"
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))
from lcov_cobertura import LcovCobertura  # noqa: E402


def _read_ignore_regex() -> str:
    """Extract COVERAGE_IGNORE_REGEX value from the script.

    The script is the single source of truth; parse it directly so
    this test can't drift from the shell variable definition.
    """
    text = SCRIPT.read_text()
    match = re.search(
        r"^COVERAGE_IGNORE_REGEX=(['\"])(?P<pattern>[^'\"]+)\1",
        text,
        flags=re.MULTILINE,
    )
    if not match:
        raise AssertionError(
            "Could not find COVERAGE_IGNORE_REGEX in scripts/run_coverage.sh"
        )
    return match.group("pattern")


def _script_contains(fragment: str) -> bool:
    return fragment in SCRIPT.read_text()


class IgnoreRegexTests(unittest.TestCase):
    """The regex excludes noisy paths and keeps our real source tree."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.pattern = _read_ignore_regex()
        cls.regex = re.compile(cls.pattern)

    def assert_ignored(self, path: str) -> None:
        self.assertTrue(
            self.regex.search(path),
            f"expected COVERAGE_IGNORE_REGEX to match noisy path: {path!r}",
        )

    def assert_kept(self, path: str) -> None:
        self.assertFalse(
            self.regex.search(path),
            f"expected COVERAGE_IGNORE_REGEX to NOT match production path: {path!r}",
        )

    def test_ignores_catch2_build_mapping(self) -> None:
        # The exact noisy paths from issue #569.
        self.assert_ignored(
            "build-coverage/_deps/catch2-build/src/src/catch2/catch_test_registry.cpp"
        )
        self.assert_ignored(
            "build-coverage/_deps/catch2-build/src/src/catch2/internal/catch_debug_console.cpp"
        )
        self.assert_ignored(
            "/abs/build-coverage/_deps/catch2-src/src/catch2/catch_all.hpp"
        )

    def test_ignores_fetchcontent_deps(self) -> None:
        self.assert_ignored("build/_deps/dawn-src/src/dawn/native/device.cpp")
        self.assert_ignored("build-coverage/_deps/skia-src/include/core/SkCanvas.h")

    def test_ignores_external_dir(self) -> None:
        self.assert_ignored("/repo/external/choc/choc_midi.h")
        self.assert_ignored("external/vst3sdk/base/source/fbuffer.cpp")

    def test_ignores_test_dir(self) -> None:
        self.assert_ignored("test/test_audio.cpp")
        self.assert_ignored("/abs/repo/test/helpers/test_helpers.hpp")

    def test_ignores_build_dirs(self) -> None:
        self.assert_ignored("/repo/build-coverage/CMakeFiles/Foo.cpp")
        self.assert_ignored("/repo/build/CMakeFiles/bar.cpp")

    def test_keeps_core_source(self) -> None:
        # The stuff we ACTUALLY want coverage on.
        self.assert_kept("core/audio/src/buffer_view.cpp")
        self.assert_kept("core/host/src/host.cpp")
        self.assert_kept("core/midi/include/pulp/midi/sysex_accumulator.hpp")
        self.assert_kept("core/format/src/vst3_adapter.cpp")
        self.assert_kept("tools/cli/src/cmd_build.cpp")

    def test_keeps_paths_that_merely_contain_test_substring(self) -> None:
        # Regression guard: a file called `attested.cpp` or a path like
        # `core/contest/` should not be accidentally excluded by a naive
        # 'test' substring match. We require the '/test/' component.
        self.assert_kept("core/runtime/src/attested_payload.cpp")
        self.assert_kept("core/protest/src/demo.cpp")


class ObjectDiscoveryTests(unittest.TestCase):
    """Regression guards for the llvm-cov object discovery passes."""

    def test_windows_executables_are_included_without_unix_execute_bit(self) -> None:
        self.assertTrue(
            _script_contains("\\( -perm -u+x -o -name '*.exe' \\)"),
            "Windows/MSYS test executables can lack a visible Unix "
            "execute bit; run_coverage.sh must include *.exe explicitly.",
        )

    def test_test_static_libraries_are_not_added_as_primary_objects(self) -> None:
        self.assertTrue(
            _script_contains('! -path "${BUILD_DIR}/test/*"'),
            "Test-local .lib archives can add zero-hit duplicate coverage "
            "maps; production archives are enough for full-surface rows.",
        )


class StaleCacheTests(unittest.TestCase):
    """#570: the post-configure assert errors when the cache is stale.

    Rather than running a real cmake configure (slow, toolchain-dependent),
    we stub `cmake` with a shell script that writes a BAD CMakeCache.txt
    (PULP_ENABLE_COVERAGE:BOOL=OFF) into the specified build directory,
    then assert run_coverage.sh detects the mismatch and exits with the
    expected error message + exit code.
    """

    def test_stale_cache_off_triggers_helpful_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            self._run_with_stub(tmp_path, cached_value="OFF")

    def test_missing_cache_triggers_helpful_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            # Write nothing; the script should still bail with the same
            # error (missing file == stale-or-bogus cache).
            self._run_with_stub(tmp_path, cached_value=None)

    def _run_with_stub(self, tmp: Path, cached_value: str | None) -> None:
        # Stub cmake binary: writes a deliberately-wrong cache to the
        # build dir so our post-configure assert trips.
        stub_bin = tmp / "bin"
        stub_bin.mkdir()
        stub_cmake = stub_bin / "cmake"
        cache_line = (
            f"PULP_ENABLE_COVERAGE:BOOL={cached_value}\n"
            if cached_value is not None
            else ""
        )
        stub_cmake.write_text(
            textwrap.dedent(
                f"""
                #!/usr/bin/env bash
                set -e
                # Parse -B to find the build dir.
                BUILD=""
                while [[ $# -gt 0 ]]; do
                    case "$1" in
                        -B) BUILD="$2"; shift 2 ;;
                        *) shift ;;
                    esac
                done
                if [[ -n "$BUILD" ]]; then
                    mkdir -p "$BUILD"
                    printf '{cache_line}' > "$BUILD/CMakeCache.txt"
                fi
                exit 0
                """
            ).lstrip()
        )
        stub_cmake.chmod(stub_cmake.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)

        # Fake build dir inside the stubbed repo root so the script's
        # REPO_ROOT logic still works. We point REPO_ROOT at a tmp tree
        # that has scripts/run_coverage.sh as a symlink to the real one.
        repo = tmp / "repo"
        repo.mkdir()
        (repo / "scripts").mkdir()
        (repo / "scripts" / "run_coverage.sh").symlink_to(SCRIPT)

        # Provide stub clang/llvm-profdata/llvm-cov so the preflight
        # checks pass even on hosts without a real toolchain.
        for tool in ("clang", "llvm-profdata", "llvm-cov"):
            stub = stub_bin / tool
            stub.write_text("#!/usr/bin/env bash\nexit 0\n")
            stub.chmod(stub.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)

        env = os.environ.copy()
        env["PATH"] = f"{stub_bin}:{env['PATH']}"

        result = subprocess.run(
            ["bash", str(repo / "scripts" / "run_coverage.sh")],
            cwd=repo,
            env=env,
            capture_output=True,
            text=True,
        )

        # The script must exit 3 (our sentinel code) with a helpful
        # message referencing the fix and the build dir.
        self.assertEqual(
            result.returncode,
            3,
            f"expected exit 3 for stale-cache guard, got {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        combined = result.stdout + result.stderr
        self.assertIn("PULP_ENABLE_COVERAGE", combined)
        self.assertIn("rm -rf", combined)
        self.assertIn("#570", combined)


class LcovCoberturaTests(unittest.TestCase):
    """Regression tests for Pulp's vendored LCOV -> Cobertura converter."""

    def _convert(self, lcov: str) -> ET.Element:
        xml = LcovCobertura(lcov, base_dir=str(REPO_ROOT)).convert()
        return ET.fromstring(xml)

    def _line(self, root: ET.Element, filename: str, number: int) -> ET.Element:
        for class_el in root.findall(".//class"):
            if class_el.attrib.get("filename") == filename:
                line = class_el.find(f"./lines/line[@number='{number}']")
                if line is not None:
                    return line
        self.fail(f"missing line {number} for {filename}")

    def test_duplicate_source_records_preserve_covered_header_lines(self) -> None:
        header = REPO_ROOT / "core/render/include/pulp/render/texture_atlas.hpp"
        lcov = textwrap.dedent(
            f"""
            SF:{header}
            DA:23,7
            BRDA:23,0,0,7
            BRDA:23,0,1,0
            end_of_record
            SF:{header}
            DA:23,0
            BRDA:23,0,0,0
            BRDA:23,0,1,0
            end_of_record
            """
        )

        root = self._convert(lcov)
        line = self._line(
            root, "core/render/include/pulp/render/texture_atlas.hpp", 23)

        self.assertEqual(line.attrib["hits"], "7")
        self.assertEqual(line.attrib["branch"], "true")
        self.assertEqual(line.attrib["condition-coverage"], "50% (1/2)")
        self.assertEqual(root.attrib["lines-valid"], "1")
        self.assertEqual(root.attrib["lines-covered"], "1")

    def test_excluded_packages_are_removed_before_summary_rates(self) -> None:
        kept = REPO_ROOT / "core/audio/src/kept.cpp"
        ignored = REPO_ROOT / "external/noise/ignored.cpp"
        lcov = textwrap.dedent(
            f"""
            SF:{kept}
            DA:10,1
            DA:11,0
            end_of_record
            SF:{ignored}
            DA:20,0
            DA:21,0
            end_of_record
            """
        )

        xml = LcovCobertura(
            lcov,
            base_dir=str(REPO_ROOT),
            excludes=[r"external\.noise"],
        ).convert()
        root = ET.fromstring(xml)
        class_names = {
            class_el.attrib["filename"]
            for class_el in root.findall(".//class")
        }

        self.assertEqual(class_names, {"core/audio/src/kept.cpp"})
        self.assertEqual(root.attrib["lines-valid"], "2")
        self.assertEqual(root.attrib["lines-covered"], "1")
        self.assertEqual(root.attrib["line-rate"], "0.5")

    def test_duplicate_source_records_merge_function_hits(self) -> None:
        source = REPO_ROOT / "core/runtime/src/widget.cpp"
        lcov = textwrap.dedent(
            f"""
            SF:{source}
            FN:7,_ZN4pulp6Widget3runEv
            FNDA:2,_ZN4pulp6Widget3runEv
            DA:7,2
            end_of_record
            SF:{source}
            FN:7,_ZN4pulp6Widget3runEv
            FNDA:3,_ZN4pulp6Widget3runEv
            DA:7,3
            end_of_record
            """
        )

        root = self._convert(lcov)
        method = root.find(".//method[@name='_ZN4pulp6Widget3runEv']")
        self.assertIsNotNone(method)
        assert method is not None
        method_line = method.find("./lines/line")
        self.assertIsNotNone(method_line)
        assert method_line is not None

        self.assertEqual(method.attrib["line-rate"], "1.0")
        self.assertEqual(method_line.attrib["number"], "7")
        self.assertEqual(method_line.attrib["hits"], "5")
        self.assertEqual(root.attrib["lines-covered"], "1")

    def test_relpath_value_error_falls_back_to_original_filename(self) -> None:
        lcov = textwrap.dedent(
            """
            SF:D:\\workspace\\pulp\\core\\runtime\\foreign_drive.cpp
            DA:3,1
            end_of_record
            """
        )

        with mock.patch(
            "lcov_cobertura.os.path.relpath",
            side_effect=ValueError("path is on mount 'D:', start on mount 'C:'"),
        ):
            root = self._convert(lcov)

        class_el = root.find(".//class")
        self.assertIsNotNone(class_el)
        assert class_el is not None
        self.assertEqual(
            class_el.attrib["filename"],
            "D:\\workspace\\pulp\\core\\runtime\\foreign_drive.cpp",
        )
        self.assertEqual(root.attrib["lines-valid"], "1")
        self.assertEqual(root.attrib["lines-covered"], "1")


if __name__ == "__main__":
    unittest.main()
