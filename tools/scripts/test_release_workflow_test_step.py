#!/usr/bin/env python3
"""Regression tests for the release-pipeline failure modes (#720, #724).

Three distinct failures were silently breaking the release pipeline:

1. **sign-and-release.yml** ran the full ctest suite, which includes
   auval-Pulp* validation tests. On hosted GitHub macOS runners, the
   freshly-installed .component bundle is not picked up by the
   AudioComponentRegistrar consistently, so auval returns "Cannot get
   Component's Name strings / Error -50" and the pipeline fails before
   it ever reaches the sign / notarize / publish steps. The dedicated
   `validate.yml` workflow already owns those validation gates on PRs
   (with the documented codesigning caveat).

2. **release-cli.yml** built the Linux pulp binary with
   PULP_BUILD_WEBVIEW=ON. Because pulp-view is a STATIC library and
   webkit2gtk is PRIVATE-linked into it, CMake propagates the dep to
   the final pulp-cli link line, so the CLI binary ends up dynamically
   linked against libjavascriptcoregtk-4.1.so.0. The smoke runner does
   not have webkit2gtk installed, so the artifact fails immediately
   with "error while loading shared libraries: libjavascriptcoregtk".
   Pulp's documented JS engine policy (CLAUDE.md) is QuickJS on Linux,
   so the CLI must not link JSC-GTK at all.

3. **sign-and-release.yml** had no `permissions:` block, so the job
   inherited a read-only GITHUB_TOKEN. The final `Create GitHub
   Release` step (softprops/action-gh-release@v2 with
   release mutation) then failed with "Resource not
   accessible by integration" — the generate-release-notes endpoint
   requires contents:write. Every prior step succeeded, but the
   pipeline still exited non-zero and macOS artifacts never landed on
   the release. Filed as pulp #724 after v0.41.1 exposed the gap (the
   Linux + auval fixes got past the earlier failures, surfacing this
   one).

These tests assert the workflow keeps the load-bearing flags so a
future edit cannot silently re-introduce any of the failure modes.

Run:
    python3 tools/scripts/test_release_workflow_test_step.py
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SIGN_AND_RELEASE = REPO_ROOT / ".github" / "workflows" / "sign-and-release.yml"
RELEASE_CLI = REPO_ROOT / ".github" / "workflows" / "release-cli.yml"
RELEASE_PATH_PR_GATE = REPO_ROOT / ".github" / "workflows" / "release-path-pr-gate.yml"
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"


class SignAndReleaseNoTestGate(unittest.TestCase):
    """sign-and-release.yml must NOT re-run the unit-test suite.

    Supersedes the original #720 design (run ctest minus the `validation`
    label). The release-artifact build is NOT a test gate: a tagged commit has
    already passed the FULL suite on the PR/merge gate (self-hosted lane with
    real GPU/display/iOS-SDK). Re-running ctest on a HEADLESS GitHub-hosted
    runner is redundant AND fails on environment-only tests that pass on real
    hardware — #720 already carved out the auval `validation` label for exactly
    this reason, then more hardware-dependent tests (Skia-raster screenshot,
    cmake-require-gpu, cmake-ios-hostapp-links) kept silently breaking Releases
    (v0.372–v0.391). Excluding labels one-by-one is unbounded whack-a-mole; the
    correct invariant is "don't run the suite here at all." The Build step is the
    release-config compile smoke, validate.yml gates the format validators on PR,
    and a headless-safe built-artifact smoke is the recommended replacement gate.
    """

    def setUp(self) -> None:
        self.assertTrue(
            SIGN_AND_RELEASE.exists(),
            f"missing workflow file: {SIGN_AND_RELEASE}",
        )
        self.text = SIGN_AND_RELEASE.read_text()

    def test_no_ctest_unit_run_step(self) -> None:
        """Guard against re-introducing a `name: Test` step that runs ctest —
        the headless whack-a-mole that blocked GitHub Releases v0.372–v0.391."""
        pattern = re.compile(
            r"-\s*name:\s*Test\s*\n"
            r"(?:\s*#[^\n]*\n)*"
            r"\s*run:\s*(.+?)(?=\n\s*-\s*name:|\Z)",
            re.DOTALL,
        )
        match = pattern.search(self.text)
        if match and "ctest" in match.group(1):
            self.fail(
                "sign-and-release.yml re-introduced a `name: Test` step that "
                "runs ctest. The release build is not a test gate — tests gate "
                "at PR on real hardware; re-running the suite on the headless "
                "release runner manufactures environment-only failures that "
                "silently block Releases. Smoke the built ARTIFACT instead.\n"
                f"Found run block:\n{match.group(1)}"
            )

    def test_appcast_writer_does_not_generate_release_notes(self) -> None:
        self.assertNotIn(
            "generate_release_notes: true",
            self.text,
            "sign-and-release.yml only attaches appcast.xml. release-cli.yml "
            "owns the humanized body plus GitHub generated notes; generating "
            "notes here can duplicate the What's Changed block depending on "
            "which workflow updates the draft last.",
        )


class ReleaseCliLinuxNoWebView(unittest.TestCase):
    """release-cli.yml must NOT pass PULP_BUILD_WEBVIEW=ON on Linux.

    Regression for issue #720 part 2. When pulp-view (a STATIC archive)
    is built with WebView=ON on Linux, webkit2gtk is PRIVATE-linked
    into it; CMake propagates that PRIVATE dep to executables that
    link the static lib, so the CLI binary ends up dynamically linked
    against libjavascriptcoregtk-4.1.so.0 — which is not installed on
    smoke runners or most user machines.

    The fix is to pick PULP_BUILD_WEBVIEW=OFF for the CLI build on
    Linux (the SDK build re-enables WebView in a separate build dir).
    This test asserts the conditional stays in place.
    """

    def setUp(self) -> None:
        self.assertTrue(
            RELEASE_CLI.exists(),
            f"missing workflow file: {RELEASE_CLI}",
        )
        self.text = RELEASE_CLI.read_text()

    def test_linux_cli_build_disables_webview(self) -> None:
        """The Configure step on Linux must select WebView=OFF.

        Accept any expression that resolves to OFF for Linux:
        - `runner.os == 'Linux' && '-DPULP_BUILD_WEBVIEW=OFF'`
        - explicit per-platform if-blocks setting OFF on Linux
        """
        # Accept the matrix conditional shape used today.
        ternary_off = re.search(
            r"runner\.os\s*==\s*'Linux'\s*&&\s*'-DPULP_BUILD_WEBVIEW=OFF'",
            self.text,
        )
        # Accept an alternative explicit step-level conditional.
        per_platform_off = re.search(
            r"if:\s*runner\.os\s*==\s*'Linux'[^\n]*\n[\s\S]{1,400}?-DPULP_BUILD_WEBVIEW=OFF",
            self.text,
        )
        self.assertTrue(
            ternary_off or per_platform_off,
            "release-cli.yml must build the Linux CLI with "
            "-DPULP_BUILD_WEBVIEW=OFF (issue #720). Without this, the "
            "static pulp-view archive transitively pulls libwebkit2gtk -> "
            "libjavascriptcoregtk-4.1 into the CLI binary's link line, "
            "and the artifact fails on every machine without webkit2gtk "
            "installed (i.e. the smoke runner and most user machines).",
        )

    def test_sdk_build_enables_webview(self) -> None:
        """The SDK build must still produce WebView symbols.

        After splitting CLI/SDK builds for #720, the SDK tarball must
        still ship WebViewPanel + make_webview_embedded_resource_fetcher
        symbols in the split view-core archive so plugin authors can use
        WebView through the SDK.
        """
        # Either a separate `build-sdk` configure with WebView=ON, or
        # the original single configure with WebView=ON, is acceptable.
        sdk_build_dir = re.search(
            r"-B\s*build-sdk[\s\S]{1,400}?-DPULP_BUILD_WEBVIEW=ON",
            self.text,
        )
        self.assertTrue(
            sdk_build_dir,
            "release-cli.yml must reconfigure for the SDK build with "
            "PULP_BUILD_WEBVIEW=ON so the SDK tarball still ships "
            "WebViewPanel symbols. See the `Prepare SDK build dir (Linux)` "
            "step in release-cli.yml.",
        )

    def test_sdk_symbol_check_uses_view_core_archive(self) -> None:
        """Phase 9 split keeps WebView symbols in the view-core archive."""
        self.assertIn("sdk-staging/lib/libpulp-view-core.a", self.text)
        self.assertIn("sdk-staging/lib/pulp-view-core.lib", self.text)
        self.assertNotIn(
            'data = Path("sdk-staging/lib/libpulp-view.a").read_bytes()',
            self.text,
        )
        self.assertNotIn(
            "Path(r'sdk-staging/lib/pulp-view.lib').read_bytes(); "
            "assert b'WebViewPanel'",
            self.text,
        )

    def test_cli_and_sdk_build_disable_audio_probes(self) -> None:
        self.assertGreaterEqual(
            self.text.count("-DPULP_ENABLE_AUDIO_PROBES=OFF"),
            2,
            "release-cli.yml must keep audio probes disabled for both the "
            "CLI and SDK release configure steps.",
        )


class BuildWorkflowReleaseGate(unittest.TestCase):
    """build.yml release-path PR gate must match release-cli.yml invariants."""

    def setUp(self) -> None:
        self.assertTrue(
            BUILD_WORKFLOW.exists(),
            f"missing workflow file: {BUILD_WORKFLOW}",
        )
        self.text = BUILD_WORKFLOW.read_text()

    def _find_step_run(self, step_name: str) -> str:
        pattern = re.compile(
            rf"-\s*name:\s*{re.escape(step_name)}\s*\n"
            r"(?:(?!\n\s*-\s*name:).)*?"
            r"\s*run:\s*(.+?)(?=\n\s*-\s*name:|\Z)",
            re.DOTALL,
        )
        match = pattern.search(self.text)
        self.assertIsNotNone(match, f"could not locate `{step_name}` step")
        return match.group(1)

    def test_windows_release_gate_checks_view_core_archive(self) -> None:
        self.assertIn("sdk-staging/lib/pulp-view.lib", self.text)
        self.assertIn("sdk-staging/lib/pulp-view-core.lib", self.text)
        self.assertNotIn(
            "Path(r'sdk-staging/lib/pulp-view.lib').read_bytes(); "
            "assert b'WebViewPanel'",
            self.text,
        )

    def test_windows_release_gate_disables_audio_probes(self) -> None:
        run_block = self._find_step_run("Configure (matches release-cli.yml)")
        self.assertIn("-DPULP_ENABLE_AUDIO_PROBES=OFF", run_block)


class ReleasePathPrGateMacosRouting(unittest.TestCase):
    """release-path-pr-gate.yml must route darwin via the release macOS var."""

    def setUp(self) -> None:
        self.assertTrue(
            RELEASE_PATH_PR_GATE.exists(),
            f"missing workflow file: {RELEASE_PATH_PR_GATE}",
        )
        self.text = RELEASE_PATH_PR_GATE.read_text()

    def test_darwin_leg_uses_release_macos_runner_resolver(self) -> None:
        self.assertIn("resolve-macos-runner:", self.text)
        self.assertIn("PULP_RELEASE_MACOS_RUNS_ON_JSON", self.text)
        self.assertIn("needs: resolve-macos-runner", self.text)
        self.assertIn(
            "matrix.os == 'macos-15' && fromJSON(needs.resolve-macos-runner.outputs.runs_on_json) || matrix.os",
            self.text,
        )

    def test_checkout_does_not_require_git_lfs(self) -> None:
        self.assertIn("lfs: false", self.text)
        self.assertNotIn("lfs: true", self.text)


class ReleaseCliDualBinaryPackaging(unittest.TestCase):
    """release-cli.yml must keep `pulp` and `pulp-cpp` bundled and smoked.

    The Rust CLI delegates several C++-owned subcommands to `pulp-cpp`.
    A release archive that contains only `pulp` can pass a basic CLI smoke
    test but fail later when a user runs a delegated command.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RELEASE_CLI.read_text(encoding="utf-8")

    def _find_step_run(self, step_name: str) -> str:
        pattern = re.compile(
            rf"-\s*name:\s*{re.escape(step_name)}\s*\n"
            r"(?:(?!\n\s*-\s*name:).)*?"
            r"\s*run:\s*(.+?)(?=\n\s*-\s*name:|\Z)",
            re.DOTALL,
        )
        match = pattern.search(self.text)
        self.assertIsNotNone(match, f"could not locate `{step_name}` step")
        return match.group(1)

    def test_unix_package_step_bundles_cpp_delegate(self) -> None:
        run_block = self._find_step_run("Package CLI (Unix)")
        self.assertIn("tools/scripts/package_cli.py", run_block)
        self.assertRegex(run_block, r"--binary\s+build/pulp")
        self.assertRegex(run_block, r"--cpp-binary\s+build/tools/cli/pulp-cpp")
        self.assertRegex(run_block, r"--mcp-binary\s+build/tools/mcp/pulp-mcp")
        self.assertRegex(run_block, r"--out\s+pulp-\$\{\{\s*matrix\.platform\s*\}\}\.tar\.gz")

    def test_windows_package_step_bundles_cpp_delegate(self) -> None:
        run_block = self._find_step_run("Package CLI (Windows)")
        self.assertIn("tools/scripts/package_cli.py", run_block)
        self.assertRegex(run_block, r"--binary\s+build/pulp\.exe")
        self.assertRegex(run_block, r"--cpp-binary\s+build/tools/cli/Release/pulp-cpp\.exe")
        self.assertRegex(run_block, r"--mcp-binary\s+build/tools/mcp/Release/pulp-mcp\.exe")
        self.assertRegex(run_block, r"--out\s+pulp-\$\{\{\s*matrix\.platform\s*\}\}\.zip")

    def test_unix_preswap_backfills_alias_cpp_cli_to_primary_binary(self) -> None:
        run_block = self._find_step_run("Normalize CLI binary layout (Unix)")
        self.assertIn("[ ! -e build/pulp ]", run_block)
        self.assertIn("[ -x build/tools/cli/pulp-cpp ]", run_block)
        self.assertIn("cp -p build/tools/cli/pulp-cpp build/pulp", run_block)
        self.assertIn("test -x build/pulp", run_block)

    def test_windows_preswap_backfills_alias_cpp_cli_to_primary_binary(self) -> None:
        run_block = self._find_step_run("Normalize CLI binary layout (Windows)")
        self.assertIn("$primary = 'build/pulp.exe'", run_block)
        self.assertIn("$cpp = 'build/tools/cli/Release/pulp-cpp.exe'", run_block)
        self.assertIn("Copy-Item -Path $cpp -Destination $primary", run_block)
        self.assertIn("Primary CLI binary missing after normalization", run_block)

    def test_unix_smoke_step_exercises_all_cli_binaries(self) -> None:
        run_block = self._find_step_run(
            "Smoke `pulp help` + `pulp-cpp help` + `pulp-mcp --version` (Unix)"
        )
        self.assertRegex(run_block, r"for\s+ART\s+in\s+pulp\s+pulp-cpp\s+pulp-mcp")
        self.assertIn('pulp-mcp) echo "--version"', run_block)
        self.assertIn('"$BIN" $CMD', run_block)
        self.assertIn("Library not loaded", run_block)
        self.assertIn("cannot open shared object", run_block)

    def test_windows_smoke_step_exercises_all_cli_binaries(self) -> None:
        run_block = self._find_step_run(
            "Smoke `pulp help` + `pulp-cpp help` + `pulp-mcp --version` (Windows)"
        )
        self.assertIn('"pulp.exe"      = "help"', run_block)
        self.assertIn('"pulp-cpp.exe"  = "help"', run_block)
        self.assertIn('"pulp-mcp.exe"  = "--version"', run_block)
        self.assertIn("-ArgumentList $cmd", run_block)
        self.assertIn("DLL was not found", run_block)
        self.assertIn("missing.*\\.dll", run_block)


class ReleaseCliBackfillOverlay(unittest.TestCase):
    """Backfill overlays must not import current source lists into old tags."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RELEASE_CLI.read_text(encoding="utf-8")

    def _find_step_block(self, step_name: str) -> str:
        pattern = re.compile(
            rf"-\s*name:\s*{re.escape(step_name)}\s*\n"
            r"(.+?)(?=\n\s*-\s*name:|\Z)",
            re.DOTALL,
        )
        match = pattern.search(self.text)
        self.assertIsNotNone(match, f"could not locate `{step_name}` step")
        return match.group(0)

    def _find_step_run(self, step_name: str) -> str:
        pattern = re.compile(
            rf"-\s*name:\s*{re.escape(step_name)}\s*\n"
            r"(?:(?!\n\s*-\s*name:).)*?"
            r"\s*run:\s*(.+?)(?=\n\s*-\s*name:|\Z)",
            re.DOTALL,
        )
        match = pattern.search(self.text)
        self.assertIsNotNone(match, f"could not locate `{step_name}` step")
        return match.group(1)

    def test_backfill_overlay_runs_for_version_tag_checkout(self) -> None:
        step_block = self._find_step_block(
            "Overlay latest release-pipeline files (workflow_dispatch backfill)"
        )
        condition = step_block.split("run:", 1)[0]
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        self.assertIn("inputs.source_ref == ''", condition)
        self.assertNotIn(
            "inputs.source_ref != ''",
            condition,
            "Normal manual backfills leave source_ref blank and check out "
            "inputs.version. The overlay must run in that path because the "
            "checked-out tag can predate the release-pipeline fixes.",
        )

    def test_backfill_overlay_keeps_cli_cmake_source_list_from_tag(self) -> None:
        run_block = self._find_step_run(
            "Overlay latest release-pipeline files (workflow_dispatch backfill)"
        )
        loop_match = re.search(
            r"for path in\s+\\\n(?P<body>.*?)\n\s+; do",
            run_block,
            re.DOTALL,
        )
        self.assertIsNotNone(loop_match, "could not locate overlay file loop")
        overlay_paths = loop_match.group("body")
        self.assertIn("tools/scripts/fetch_skia_for_release.py", overlay_paths)
        self.assertIn("tools/scripts/package_cli.py", overlay_paths)
        self.assertIn("core/canvas/CMakeLists.txt", overlay_paths)
        self.assertIn("tools/deps/manifest.json", overlay_paths)
        self.assertNotIn(
            "tools/cli/CMakeLists.txt",
            overlay_paths,
            "Backfills must not wholesale-overlay tools/cli/CMakeLists.txt: "
            "main's add_executable() source list can reference .cpp files that "
            "do not exist in older tags.",
        )

    def test_backfill_patches_only_cli_fontconfig_tail_link(self) -> None:
        run_block = self._find_step_run(
            "Overlay latest release-pipeline files (workflow_dispatch backfill)"
        )
        self.assertIn('Path("tools/cli/CMakeLists.txt")', run_block)
        self.assertIn("target_link_libraries\\(pulp-cli PRIVATE", run_block)
        self.assertIn("_PULP_CLI_FONTCONFIG", run_block)
        self.assertIn("path.write_text(text", run_block)
        self.assertNotIn("package_analyzer_descriptors.cpp", run_block)


class ReleaseCliLatestPointer(unittest.TestCase):
    """Manual release-cli backfills must not move GitHub's latest pointer."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RELEASE_CLI.read_text(encoding="utf-8")

    def _find_release_job_block(self) -> str:
        match = re.search(
            r"^  release:\n(?P<body>.*?)(?=^  [A-Za-z0-9_-]+:|\Z)",
            self.text,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(match, "could not locate release-cli.yml release job")
        return match.group("body")

    def _find_step_block(self, step_name: str) -> str:
        pattern = re.compile(
            rf"-\s*name:\s*{re.escape(step_name)}\s*\n"
            r"(.+?)(?=\n\s*-\s*name:|\Z)",
            re.DOTALL,
        )
        match = pattern.search(self.text)
        self.assertIsNotNone(match, f"could not locate `{step_name}` step")
        return match.group(0)

    def test_manual_dispatch_make_latest_input_defaults_false(self) -> None:
        input_match = re.search(
            r"make_latest:\s*\n(?P<body>(?:\s{8,}[^\n]*\n)+)",
            self.text,
        )
        self.assertIsNotNone(input_match, "release-cli.yml must define a make_latest input")
        input_block = input_match.group("body")
        self.assertIn("type: boolean", input_block)
        self.assertIn("default: false", input_block)

    def test_create_release_promotes_latest_only_for_tag_push_or_opt_in(self) -> None:
        step_block = self._find_step_block("Create release")
        self.assertIn(
            "make_latest: ${{ github.event_name != 'workflow_dispatch' || inputs.make_latest }}",
            step_block,
        )
        self.assertNotIn(
            "make_latest: true",
            step_block,
            "Unconditional make_latest lets an old-tag workflow_dispatch backfill "
            "move GitHub's /releases/latest pointer backward.",
        )

    def test_release_job_permissions_cover_body_composition(self) -> None:
        job_block = self._find_release_job_block()
        self.assertIn("contents: write", job_block)
        self.assertIn("issues: read", job_block)


class SignAndReleaseContentsWriteTest(unittest.TestCase):
    """#724: sign-and-release.yml must declare `contents: write` on its
    macOS job so the final `Create GitHub Release` step can upload appcast.xml
    to the draft Release. Without this scope, every sign-and-release run fails
    at the last step with `Resource not accessible by integration` and
    macOS-signed artifacts never land on the release — classic silent release
    failure pattern.
    """

    @classmethod
    def setUpClass(cls) -> None:
        root = Path(__file__).resolve().parent.parent.parent
        cls.workflow_path = root / ".github" / "workflows" / "sign-and-release.yml"
        cls.text = cls.workflow_path.read_text(encoding="utf-8")

    def test_macos_job_declares_contents_write(self) -> None:
        """The build-and-sign-macos job must have `contents: write`.

        The regex matches across the job's header to the first `steps:`
        key, so reordering within the header is fine — only the presence
        of the scope matters.
        """
        # Match the build-and-sign-macos job block up to its `steps:`.
        macos_job = re.search(
            r"build-and-sign-macos:\s*\n([\s\S]{1,800}?)^\s{4}steps:",
            self.text,
            re.MULTILINE,
        )
        self.assertTrue(
            macos_job,
            "sign-and-release.yml must define a `build-and-sign-macos` job "
            "with a `steps:` block. If the job was renamed, update this test "
            "to match.",
        )
        job_header = macos_job.group(1)
        self.assertRegex(
            job_header,
            r"permissions:\s*\n\s*contents:\s*write",
            "sign-and-release.yml `build-and-sign-macos` job must declare "
            "`permissions: contents: write` (issue #724). Without this, the "
            "final `Create GitHub Release` step fails with `Resource not "
            "accessible by integration` while uploading appcast.xml. Every "
            "sign-and-release run then silently fails and macOS-signed "
            "artifacts never land on the release.",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
