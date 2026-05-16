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
   generate_release_notes: true) then failed with "Resource not
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


class SignAndReleaseTestStep(unittest.TestCase):
    """sign-and-release.yml must skip validation-labeled tests."""

    def setUp(self) -> None:
        self.assertTrue(
            SIGN_AND_RELEASE.exists(),
            f"missing workflow file: {SIGN_AND_RELEASE}",
        )
        self.text = SIGN_AND_RELEASE.read_text()

    def _find_test_step_run(self) -> str:
        """Return the shell text under the `name: Test` step."""
        # Match the Test step block: `name: Test` followed by an optional
        # comment block, then `run:`. The run can be a single line
        # (`run: ctest ...`) or a literal block (`run: |`).
        pattern = re.compile(
            r"-\s*name:\s*Test\s*\n"            # step header
            r"(?:\s*#[^\n]*\n)*"               # optional comment lines
            r"\s*run:\s*(.+?)(?=\n\s*-\s*name:|\Z)",
            re.DOTALL,
        )
        match = pattern.search(self.text)
        self.assertIsNotNone(
            match,
            "could not locate the `name: Test` step in sign-and-release.yml",
        )
        return match.group(1)

    def test_test_step_invokes_ctest(self) -> None:
        run_block = self._find_test_step_run()
        self.assertIn(
            "ctest",
            run_block,
            "sign-and-release Test step should still invoke ctest",
        )

    def test_test_step_excludes_validation_label(self) -> None:
        """Regression for #720.

        The Test step must pass `-LE validation` to ctest (or otherwise
        explicitly skip the validation label) so auval / pluginval /
        clap-validator failures on hosted GH runners do not silently
        break the sign-and-release pipeline.
        """
        run_block = self._find_test_step_run()
        # Accept either the short `-LE validation` form or the long
        # `--label-exclude validation` form so future edits have
        # flexibility.
        has_short = re.search(r"-LE\s+validation", run_block)
        has_long = re.search(r"--label-exclude\s+validation", run_block)
        self.assertTrue(
            has_short or has_long,
            "sign-and-release Test step must exclude the `validation` ctest "
            "label (issue #720). Without `-LE validation`, auval failures on "
            "hosted GitHub macOS runners break the entire release pipeline. "
            f"Found run block:\n{run_block}",
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
        symbols in libpulp-view.a so plugin authors can use WebView.
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


class SignAndReleaseContentsWriteTest(unittest.TestCase):
    """#724: sign-and-release.yml must declare `contents: write` on its
    macOS job so the final `Create GitHub Release` step can call the
    generate-release-notes API. Without this scope, every sign-and-release
    run fails at the last step with `Resource not accessible by integration`
    and macOS-signed artifacts never land on the release — classic silent
    release failure pattern (CLAUDE.md § Silent release failures are critical).
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
            "accessible by integration` because softprops/action-gh-release@v2 "
            "calls the generate-release-notes API which requires the scope. "
            "Every sign-and-release run then silently fails, macOS-signed "
            "artifacts never land on the release, and the next release is a "
            "ghost.",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
