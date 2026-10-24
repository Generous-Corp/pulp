#!/usr/bin/env python3
"""Fixture tests for scripts/run_coverage.sh.

Covers two coverage regressions tracked by issue #566:

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
CTEST_POLICY = REPO_ROOT / "scripts" / "coverage_ctest_policy.sh"
NATIVE_PATHS = REPO_ROOT / "scripts" / "coverage_native_paths.sh"
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


def _profile_pattern() -> str:
    """The LLVM_PROFILE_FILE basename pattern run_coverage.sh exports."""
    match = re.search(r'^COVERAGE_PROFILE_PATTERN="([^"]+)"$', SCRIPT.read_text(encoding="utf-8"), re.M)
    assert match, "COVERAGE_PROFILE_PATTERN not found in run_coverage.sh"
    return match.group(1)


def _script_contains(fragment: str) -> bool:
    return fragment in SCRIPT.read_text()


class ParallelismContractTests(unittest.TestCase):
    def test_build_and_test_parallelism_can_be_tuned_independently(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn('TEST_JOBS="${PULP_COVERAGE_TEST_JOBS:-8}"', text)
        self.assertIn('--test-jobs) TEST_JOBS="$2"', text)
        self.assertIn('CTEST_JOBS="${TEST_JOBS}"', text)

    def test_default_suite_excludes_slow_proofs_for_every_caller(self) -> None:
        text = SCRIPT.read_text()
        policy = CTEST_POLICY.read_text()
        self.assertIn('source "${SCRIPT_DIR}/coverage_ctest_policy.sh"', text)
        self.assertIn("PULP_COVERAGE_CTEST_DEFAULT_ARGS", text)
        self.assertIn("validation|slow|performance|bench|quality-lab", policy)
        self.assertIn("fdn.*bounded.*parameter.*vector", policy)
        self.assertIn('EXTRA_CTEST_ARGS=("${DEFAULT_CTEST_ARGS[@]}")', text)

        match = re.search(
            r"PULP_COVERAGE_CTEST_NAME_EXCLUDE:=\((?P<pattern>[^}]+)\)\}",
            policy,
        )
        self.assertIsNotNone(match, "default CTest name exclusion must be one array item")
        exclusion = re.compile(match.group("pattern"))
        source = "\n".join(
            path.read_text()
            for path in (
                REPO_ROOT / "test" / "test_fdn_reverb.cpp",
                REPO_ROOT / "test" / "test_character_delay.cpp",
                REPO_ROOT / "test" / "test_osc_vco.cpp",
                REPO_ROOT / "test" / "test_analog_vcf.cpp",
                REPO_ROOT / "test" / "test_osc_wt.cpp",
                REPO_ROOT / "test" / "test_modal_bank.cpp",
                REPO_ROOT / "test" / "test_spectral_matrix.cpp",
                REPO_ROOT / "test" / "test_playback_program.cpp",
                REPO_ROOT
                / "examples"
                / "PulpSampler"
                / "test_pulp_sampler_streaming.cpp",
            )
        )
        expected_slow_cases = (
            "fdn reverb stays bounded and decaying for every parameter vector",
            "saturating characters self-oscillate bounded at maximum feedback",
            "physical tape keeps sub-unity feedback below oscillation",
            "drift wanders the pitch slowly at ~the commanded RMS",
            "Analog VCF stays finite at worst-case drive and oversampling",
            "OSC-WT worst alias swept to the top of every band",
            "OSC-WT worst-case alias is detection-floor-limited, not a fixed spur",
            "modal bank throughput scales to large banks in real time",
            "matrix: realtime factor reported for quality and low-latency modes",
            "deferred compiler handles ten thousand clips and one hundred coalesced edits",
            "PulpSampler sinc follows independent continuous pitch modulation controls",
        )
        for case in expected_slow_cases:
            self.assertIn(case, source, f"expected slow test was renamed: {case}")
            self.assertRegex(case, exclusion, f"coverage no longer excludes: {case}")

    def test_full_suite_has_an_explicit_opt_out(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn('--include-slow-tests) INCLUDE_SLOW_TESTS=true', text)
        self.assertIn('if [[ "${INCLUDE_SLOW_TESTS}" == true ]]', text)
        self.assertLess(
            text.index('if [[ "${INCLUDE_SLOW_TESTS}" == true ]]'),
            text.index('elif [[ -n "${PULP_COVERAGE_CTEST_ARGS:-}" ]]'),
            "the explicit full-suite option must override the compatibility env hook",
        )


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

    def test_llvm_cov_objects_passed_via_response_file(self) -> None:
        # A full build probes 1000+ `-object` entries. Passing them inline
        # overflows the Windows command-line length limit (CreateProcess
        # ~32 KB) — llvm-cov dies with "Argument list too long", filters out
        # every source file, and the os-windows coverage leg produces no
        # Cobertura XML. report/show/export must all read the object list from
        # an LLVM @response file instead.
        text = SCRIPT.read_text()
        self.assertIn('OBJ_RSP=', text,
                      "object list must be written to a response file")
        self.assertIn('printf -- \'-object\\n"%s"\\n\' "${native}"', text,
                      "response file must be built one -object/path pair at a time, "
                      "with the path double-quoted so a workspace path containing a "
                      "space stays a single -object argument")
        self.assertIn('done > "${OBJ_RSP}"', text)
        self.assertGreaterEqual(
            text.count('"@${OBJ_RSP}"'), 3,
            "llvm-cov report, show, and export must each pass objects via the "
            "@response file, not inline ${BINARIES[@]}",
        )

    def test_objects_that_vanished_after_probe_are_dropped(self) -> None:
        # An -object can disappear between the pre-flight probe and the report
        # (a late-cleaned test-fixture .exe on Windows is the observed case).
        # llvm-cov hard-fails the whole run on one missing -object, so the
        # response-file builder must existence-check each path and drop the
        # vanished ones rather than blackhole the os-windows leg.
        text = SCRIPT.read_text()
        self.assertIn('if [[ -f "${tok}" ]]; then', text,
                      "response-file builder must drop objects that no longer exist")
        self.assertIn("existence filter left zero -object entries", text,
                      "an empty post-filter object set must fail loudly")

    def test_mass_object_drop_fails_loudly(self) -> None:
        # The existence filter must not silently upload near-empty coverage when
        # it drops a large fraction of objects (a broken build tree / discovery
        # bug). A healthy run drops ~0-1; a mass drop must fail, not export
        # coverage from the few survivors as if valid.
        text = SCRIPT.read_text()
        self.assertIn("TOTAL_OBJECTS=", text)
        self.assertIn("VANISHED_OBJECTS * 100", text,
                      "mass-drop guard must compare vanished against a percentage "
                      "of the total object set")
        self.assertIn("refusing to upload coverage from the survivors", text)

    def test_response_file_paths_are_quoted_for_spaces(self) -> None:
        # LLVM response files are whitespace-tokenized but honor double quotes;
        # an unquoted path containing a space would split into bogus args.
        text = SCRIPT.read_text()
        self.assertIn('-object\\n"%s"\\n', text,
                      "object paths in the response file must be double-quoted")

    def test_optional_ctest_args_are_threaded_into_ctest_invocation(self) -> None:
        self.assertTrue(
            _script_contains('PULP_COVERAGE_CTEST_ARGS'),
            "Coverage workflow needs a script-level hook for platform-specific "
            "CTest policy such as macOS validation-label exclusion.",
        )
        self.assertTrue(
            _script_contains('"${EXTRA_CTEST_ARGS[@]}" --quiet --output-on-failure'),
            "run_coverage.sh must pass optional CTest args before the shared "
            "output/retry flags.",
        )

    def test_ctest_progress_is_suppressed_and_failures_are_replayed(self) -> None:
        text = SCRIPT.read_text()
        self.assertEqual(
            text.count('--quiet --output-on-failure'),
            2,
            "both filtered and full coverage runs must suppress successful "
            "CTest progress while retaining failure output",
        )
        self.assertIn(
            "run_ctest()",
            text,
            "the full coverage suite must run through the bounded-output helper",
        )
        self.assertIn(
            ">/dev/null",
            text,
            "per-test CTest progress must not fill the hosted runner log pager",
        )
        self.assertIn(
            "ctest --rerun-failed --output-on-failure",
            text,
            "failed tests must still be replayed with diagnostics",
        )

    def test_coverage_build_output_does_not_fill_runner_logs(self) -> None:
        script = SCRIPT.read_text()
        self.assertIn(
            'cmake --build "${BUILD_DIR}" > "${BUILD_STDOUT_LOG}" 2>"${BUILD_LOG}"',
            script,
        )
        self.assertNotIn(
            'cmake --build "${BUILD_DIR}" -j"${JOBS}"',
            script,
            "the governed wrapper owns build parallelism",
        )
        self.assertIn('tail -n 200 "${BUILD_LOG}"', script)

    def test_html_drilldown_can_be_disabled_for_ci(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn('PULP_COVERAGE_SKIP_HTML:-0', text)
        self.assertIn('Skipping llvm-cov HTML drilldown', text)

    def test_profraw_cleanup_uses_find_delete(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn(
            'find "${PROFRAW_DIR}" -name \'*.profraw\' -type f -delete',
            text,
            "run_coverage.sh must delete stale profraw files with find so "
            "large previous coverage runs cannot hit ARG_MAX.",
        )
        self.assertNotIn(
            'rm -f "${PROFRAW_DIR}"/*.profraw',
            text,
            "rm over a profraw glob fails once coverage has thousands of "
            "profile files.",
        )

    def test_merged_profraw_shards_are_reclaimed_before_html_report(self) -> None:
        text = SCRIPT.read_text()
        merge = text.index('llvm-profdata merge -sparse')
        cleanup = text.index('=== Reclaiming merged raw profile shards ===')
        html = text.index('echo "=== llvm-cov show (HTML drilldown) ==="')
        self.assertLess(merge, cleanup)
        self.assertLess(cleanup, html)
        self.assertIn(
            'find "${PROFRAW_DIR}" -name \'*.profraw\' -type f -delete',
            text[cleanup:html],
        )
        self.assertIn(
            'REMAINING_PROFILE_SHARDS=$(find "${PROFRAW_DIR}"',
            text[cleanup:html],
        )

    def test_profraw_shards_are_reclaimed_after_merge(self) -> None:
        text = SCRIPT.read_text()
        marker = 'echo "=== Merged ${PROFILE_SHARDS} raw profile shard(s) from ${PROFILE_MODULES} module pool(s); ignored ${INVALID_PROFILE_SHARDS} invalid shard(s) ==="'
        self.assertIn(marker, text)
        self.assertIn('find "${PROFRAW_DIR}" -name \'*.profraw\' -type f -delete', text[text.index(marker):])

    def test_profraw_pattern_is_a_per_module_pool_not_per_process(self) -> None:
        """Bound the shard count by binaries, not by processes.

        `%p` gives every test process its own file, so the whole suite's raw
        profiles sit on disk at once and filled the hosted Linux runner's root
        volume mid-suite. `%Nm` keeps one online-merge pool per module.
        """
        pattern = _profile_pattern()
        self.assertNotIn("%p", pattern, "a per-process profile grows with every test process")
        self.assertRegex(
            pattern, r"%[1-9]m",
            "the pool needs `%Nm`: plain `%m` is one file per module, which "
            "parallel exits of one binary corrupt",
        )
        self.assertIn(
            'export LLVM_PROFILE_FILE="${PROFRAW_DIR}/${COVERAGE_PROFILE_PATTERN}"',
            SCRIPT.read_text(encoding="utf-8"),
        )

    def test_full_run_fails_closed_when_pools_cover_too_few_modules(self) -> None:
        """A pool shared across binaries drops profiles without failing.

        The guard compares distinct pool signatures with the test-binary count,
        after TEST_BINARIES exists, and only for an unfiltered run.
        """
        text = SCRIPT.read_text(encoding="utf-8")
        guard = text.index("PROFILE_MODULES * 2 < ${#TEST_BINARIES[@]}")
        self.assertLess(text.index("TEST_BINARIES+=("), guard)
        self.assertIn('[[ -z "${TESTS_REGEX}" ]]', text[guard - 200:guard])
        self.assertIn("exit 1", text[guard:guard + 400])

    def test_merge_tolerates_isolated_bad_shards_but_has_a_mass_guard(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn("--failure-mode=all", text)
        self.assertIn('INVALID_PROFILE_SHARDS}" -gt 25', text)
        self.assertIn("INVALID_PROFILE_SHARDS * 100", text)
        self.assertIn("PROFILE_SHARDS * 5", text)

    def test_merge_uses_one_input_list_invocation(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn(
            'find "${PROFRAW_DIR}" -name \'*.profraw\' -type f -print \\\n'
            '    | coverage_native_path_list > "${PROFILE_INPUTS}"',
            text,
        )
        self.assertIn('--input-files="${PROFILE_INPUTS}"', text)
        self.assertNotIn(
            "xargs -0 llvm-profdata merge",
            text,
            "xargs may split a large shard set into multiple merges whose "
            "shared output path causes later invocations to discard earlier data.",
        )


class NativePathListTests(unittest.TestCase):
    """Path lists that native Windows LLVM tools read from a file.

    MSYS translates argv for a native program but never the contents of a
    file, so the llvm-profdata --input-files list and the llvm-cov @response
    file must already hold native paths on the os-windows leg.
    """

    # Mimics `cygpath -m -f -` for drive paths: /d/a/x -> D:/a/x.
    CYGPATH_STUB = textwrap.dedent("""\
        #!/usr/bin/env bash
        echo "$*" >> "$CYGPATH_CALLS"
        while IFS= read -r line; do
            drive=$(printf '%s' "${line:1:1}" | tr '[:lower:]' '[:upper:]')
            printf '%s:%s\\n' "${drive}" "${line:2}"
        done
        """)

    def _run(self, stdin: str, *, msystem: str | None, cygpath: bool) -> tuple[subprocess.CompletedProcess, str]:
        with tempfile.TemporaryDirectory() as tmp:
            stub_bin = Path(tmp) / "bin"
            stub_bin.mkdir()
            calls = Path(tmp) / "calls"
            if cygpath:
                stub = stub_bin / "cygpath"
                stub.write_text(self.CYGPATH_STUB, encoding="utf-8")
                stub.chmod(0o755)
            env = {"PATH": f"{stub_bin}:/usr/bin:/bin", "CYGPATH_CALLS": str(calls)}
            if msystem is not None:
                env["MSYSTEM"] = msystem
            result = subprocess.run(
                ["bash", "-c", f'source "{NATIVE_PATHS}"; coverage_native_path_list'],
                input=stdin, capture_output=True, text=True, env=env, check=False,
            )
            return result, calls.read_text() if calls.exists() else ""

    def test_msys_lists_are_rewritten_to_native_paths(self) -> None:
        result, calls = self._run(
            "/d/a/pulp/build-coverage/profraw/pulp-1_0.profraw\n/d/a/b c/x.exe\n",
            msystem="MINGW64", cygpath=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         ["D:/a/pulp/build-coverage/profraw/pulp-1_0.profraw", "D:/a/b c/x.exe"])
        self.assertEqual(calls.strip(), "-m -f -")

    def test_posix_hosts_pass_lists_through_unchanged(self) -> None:
        lines = "/home/runner/build-coverage/profraw/pulp-1_0.profraw\n/Volumes/CI Builds/x\n"
        result, calls = self._run(lines, msystem=None, cygpath=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, lines)
        self.assertEqual(calls, "", "cygpath must not run outside MSYS")

    def test_msys_without_cygpath_passes_through(self) -> None:
        result, _ = self._run("/d/a/x\n", msystem="MINGW64", cygpath=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "/d/a/x\n")

    def test_both_file_read_lists_go_through_the_helper(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn('source "${SCRIPT_DIR}/coverage_native_paths.sh"', text)
        self.assertEqual(text.count("| coverage_native_path_list"), 2,
                         "the profdata input list and the llvm-cov response file "
                         "must both be rewritten to native paths")


class ProfilePoolBehaviourTests(unittest.TestCase):
    """Run the script's real LLVM_PROFILE_FILE pattern against real binaries.

    The load-bearing property is that the pool is keyed per BINARY: two
    different instrumented programs exiting concurrently must land in separate
    pools and both keep every run's counters. A pool shared across binaries is
    the silent under-reporting this lane once had, so it is tested by
    behaviour, not only by grep.
    """

    @staticmethod
    def _tool(name: str) -> str | None:
        found = shutil.which(name)
        if found:
            return found
        try:
            out = subprocess.run(
                ["xcrun", "-f", name], capture_output=True, text=True, check=False,
            )
        except FileNotFoundError:
            return None
        return out.stdout.strip() or None

    def test_concurrent_runs_of_two_binaries_pool_per_module_and_keep_counters(self) -> None:
        clang = shutil.which("clang")
        profdata = self._tool("llvm-profdata")
        if clang is None or profdata is None:
            self.skipTest("clang / llvm-profdata not available")
        runs_per_binary = 12

        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            profraw = work / "profraw"
            profraw.mkdir()
            binaries = []
            for name, body in (("alpha", "x * 2"), ("beta", "x > 0 ? x : -x")):
                src = work / f"{name}.c"
                src.write_text(
                    f"int {name}(int x){{return {body};}}\n"
                    f"int main(void){{return {name}(1) == 0;}}\n",
                    encoding="utf-8",
                )
                binary = work / name
                subprocess.run(
                    [clang, "-fprofile-instr-generate", "-fcoverage-mapping", "-O0",
                     "-o", str(binary), str(src)],
                    check=True, capture_output=True,
                )
                binaries.append(binary)

            env = dict(os.environ, LLVM_PROFILE_FILE=str(profraw / _profile_pattern()))
            procs = [
                subprocess.Popen([str(b)], env=env)
                for _ in range(runs_per_binary) for b in binaries
            ]
            for proc in procs:
                self.assertEqual(proc.wait(), 0)

            shards = sorted(p.name for p in profraw.glob("*.profraw"))
            self.assertTrue(shards, "the pattern produced no profiles")
            self.assertLessEqual(
                len(shards), 2 * 8,
                f"{len(shards)} shards for 2 binaries: the pool is not bounded per module",
            )
            # The script's own signature extraction must see one pool per binary.
            inputs = work / "inputs.txt"
            inputs.write_text("".join(f"{profraw / s}\n" for s in shards), encoding="utf-8")
            extract = re.search(r"PROFILE_MODULES=\$\((sed -n '[^']+')", SCRIPT.read_text(encoding="utf-8"))
            self.assertIsNotNone(extract, "PROFILE_MODULES extraction not found")
            modules = subprocess.run(
                ["bash", "-c", f'{extract.group(1)} "$1" | sort -u | wc -l', "_", str(inputs)],
                capture_output=True, text=True, encoding="utf-8", check=True,
            ).stdout.strip()
            self.assertEqual(modules, "2", f"expected one pool per binary, got {modules}: {shards}")

            merged = work / "m.profdata"
            subprocess.run(
                [profdata, "merge", "-sparse", *map(str, profraw.glob("*.profraw")),
                 "-o", str(merged)],
                check=True, capture_output=True,
            )
            shown = subprocess.run(
                [profdata, "show", "--all-functions", str(merged)],
                capture_output=True, text=True, encoding="utf-8", check=True,
            ).stdout
            # Every run of each binary must survive the pool merge: a dropped
            # profile shows up as a function count below runs_per_binary.
            for name in ("alpha", "beta"):
                block = re.search(
                    rf"^\s*{name}:\n(?:.*\n)*?\s*Function count: (\d+)", shown, re.M,
                )
                self.assertIsNotNone(block, f"{name} missing from the merged profile:\n{shown}")
                self.assertEqual(int(block.group(1)), runs_per_binary, shown)


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
        (repo / "scripts" / "coverage_ctest_policy.sh").symlink_to(CTEST_POLICY)
        (repo / "scripts" / "coverage_native_paths.sh").symlink_to(NATIVE_PATHS)

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


class BuildFailureDiagnosticsTests(unittest.TestCase):
    """A failed coverage build must name its first error, not only the tail.

    With parallel jobs, sibling targets keep compiling after the first error,
    so the last lines of the build log are usually unrelated warnings. The
    stub build fails with one error line followed by 300 warning lines, more
    than the tail shows.
    """

    def _run_failing_build(self, build_body: str,
                           extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        """Run run_coverage.sh against a stub cmake whose --build runs build_body."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            stub_bin = tmp_path / "bin"
            stub_bin.mkdir()
            stub_cmake = stub_bin / "cmake"
            stub_cmake.write_text(
                "#!/usr/bin/env bash\n"
                'if [[ -n "${STUB_CMAKE_ARGS_LOG:-}" ]]; then printf \'%s\\n\' "$@" >> "$STUB_CMAKE_ARGS_LOG"; fi\n'
                'if [[ "$1" == "--build" ]]; then\n'
                f"{build_body}\n"
                "    exit 2\n"
                "fi\n"
                + textwrap.dedent(
                    """
                    BUILD=""
                    while [[ $# -gt 0 ]]; do
                        case "$1" in
                            -B) BUILD="$2"; shift 2 ;;
                            *) shift ;;
                        esac
                    done
                    mkdir -p "$BUILD"
                    printf 'PULP_ENABLE_COVERAGE:BOOL=ON\n' > "$BUILD/CMakeCache.txt"
                    """
                ),
                encoding="utf-8",
            )
            stub_cmake.chmod(0o755)
            for tool in ("clang", "llvm-profdata", "llvm-cov"):
                stub = stub_bin / tool
                stub.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
                stub.chmod(0o755)

            repo = tmp_path / "repo"
            (repo / "scripts").mkdir(parents=True)
            (repo / "scripts" / "run_coverage.sh").symlink_to(SCRIPT)
            (repo / "scripts" / "coverage_ctest_policy.sh").symlink_to(CTEST_POLICY)
            (repo / "scripts" / "coverage_native_paths.sh").symlink_to(NATIVE_PATHS)
            # The governed wrapper's lease logic is not under test here.
            (repo / "tools" / "ci").mkdir(parents=True)
            wrapper = repo / "tools" / "ci" / "governed-build.sh"
            wrapper.write_text('#!/usr/bin/env bash\nexec "$@"\n', encoding="utf-8")
            wrapper.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = f"{stub_bin}:{env['PATH']}"
            env.pop("GITHUB_ACTIONS", None)
            env.pop("MSYSTEM", None)
            env.pop("OS", None)
            env.update(extra_env or {})
            return subprocess.run(
                ["bash", str(repo / "scripts" / "run_coverage.sh")],
                cwd=repo,
                env=env,
                capture_output=True,
                text=True,
                encoding="utf-8",
            )

    def test_first_error_is_printed_even_when_buried_before_the_tail(self) -> None:
        result = self._run_failing_build(
            '    echo "custom command FAILED: stdout-only-marker"\n'
            '    echo "core/x.cpp:1:1: error: buried-compile-error" >&2\n'
            '    for i in $(seq 1 300); do echo "warning: noise $i" >&2; done'
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Coverage build failed", result.stderr)
        self.assertIn("buried-compile-error", result.stderr)
        self.assertIn("stdout-only-marker", result.stderr)
        self.assertIn("warning: noise 300", result.stderr)

    def test_phases_are_annotated_on_github_actions(self) -> None:
        # A lost hosted runner keeps its annotations but not its log, so each
        # phase is also raised as a notice there, and only there.
        body = '    echo "x.cpp:1:1: error: stop" >&2'
        on_actions = self._run_failing_build(body, {"GITHUB_ACTIONS": "true"})
        self.assertIn("::notice title=coverage phase::", on_actions.stdout)
        self.assertRegex(on_actions.stdout, r"::notice title=coverage phase::\S+ Building")
        locally = self._run_failing_build(body)
        self.assertIn("=== Building ===", locally.stdout)
        self.assertNotIn("::notice", locally.stdout)

    def test_a_stderr_error_flood_does_not_hide_stdout_errors(self) -> None:
        # More stderr matches than one log's cap; the stdout-only failure must
        # still be printed.
        result = self._run_failing_build(
            '    for i in $(seq 1 150); do echo "x.cpp:$i:1: error: flood $i" >&2; done\n'
            '    echo "FAILED: stdout-only-step"'
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("flood 1", result.stderr)
        self.assertIn("stdout-only-step", result.stderr)

    def test_lld_link_errors_print_the_mismatching_objects(self) -> None:
        # lld-link puts the two disagreeing objects on the lines after the
        # error; without them a /failifmismatch does not say which library.
        result = self._run_failing_build(
            '    echo "FAILED: CLAP/PulpGain.clap"\n'
            '    echo "lld-link: error: /failifmismatch: mismatch detected for \'_ITERATOR_DEBUG_LEVEL\':"\n'
            '    echo ">>> gain.obj has value 2"\n'
            '    echo ">>> skia.lib(SkFoo.obj) has value 0"'
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("lld-link errors with context", result.stderr)
        self.assertIn(">>> skia.lib(SkFoo.obj) has value 0", result.stderr)

    def _configured_build_type(self, extra_env: dict[str, str]) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "cmake-args.txt"
            self._run_failing_build('    echo "x.cpp:1:1: error: stop" >&2',
                                    {**extra_env, "STUB_CMAKE_ARGS_LOG": str(log)})
            return [a for a in log.read_text(encoding="utf-8").splitlines()
                    if a.startswith("-DCMAKE_BUILD_TYPE=")]

    def test_windows_configures_relwithdebinfo_not_debug(self) -> None:
        # A Debug Windows build links the /MTd runtime, which the prebuilt
        # release Skia rejects at link time (_ITERATOR_DEBUG_LEVEL 2 vs 0).
        self.assertEqual(self._configured_build_type({"MSYSTEM": "MINGW64"}),
                         ["-DCMAKE_BUILD_TYPE=RelWithDebInfo"])
        self.assertEqual(self._configured_build_type({"OS": "Windows_NT"}),
                         ["-DCMAKE_BUILD_TYPE=RelWithDebInfo"])

    def test_posix_configures_debug(self) -> None:
        self.assertEqual(self._configured_build_type({}), ["-DCMAKE_BUILD_TYPE=Debug"])


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
