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

import yaml


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SIGN_AND_RELEASE = REPO_ROOT / ".github" / "workflows" / "sign-and-release.yml"
RELEASE_CLI = REPO_ROOT / ".github" / "workflows" / "release-cli.yml"
RELEASE_PUBLISH = REPO_ROOT / ".github" / "workflows" / "release-publish.yml"
RELEASE_PATH_PR_GATE = REPO_ROOT / ".github" / "workflows" / "release-path-pr-gate.yml"
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
AUTO_RELEASE = REPO_ROOT / ".github" / "workflows" / "auto-release.yml"
WATCHDOG_REAPER = REPO_ROOT / ".github" / "workflows" / "watchdog-reaper.yml"
VERSION_SKILL_CHECK = REPO_ROOT / ".github" / "workflows" / "version-skill-check.yml"
INTENT_BUMP_ON_MERGE = REPO_ROOT / ".github" / "workflows" / "intent-bump-on-merge.yml"
POST_TAG_SYNC = REPO_ROOT / ".github" / "workflows" / "post-tag-sync.yml"
RELEASE_SIGNING_HELPER = REPO_ROOT / "tools" / "scripts" / "configure_release_bot_ssh_signing.sh"


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


class SingleOwnerReleasePublication(unittest.TestCase):
    """release-cli.yml's `release` job must own publication END TO END.

    Publication used to be a handshake across three workflows: release-cli made a
    draft, sign-and-release polled 50 minutes for that draft to attach
    appcast.xml, and release-publish.yml flipped the draft only once BOTH legs
    reported success. That coupled the release to the slowest, least reliable leg
    — and 11 of 18 tags in 2026-07 never published, several of them with all six
    platform binaries built green.

    These tests pin the collapse so it cannot be reintroduced.
    """

    REQUIRED_RELEASE_ASSETS = [
        "appcast.xml",
        "pulp-darwin-arm64.tar.gz",
        "pulp-darwin-x64.tar.gz",
        "pulp-linux-arm64.tar.gz",
        "pulp-linux-x64.tar.gz",
        "pulp-windows-arm64.zip",
        "pulp-windows-x64.zip",
        "pulp-sdk-darwin-arm64.tar.gz",
        "pulp-sdk-darwin-x64.tar.gz",
        "pulp-sdk-linux-arm64.tar.gz",
        "pulp-sdk-linux-x64.tar.gz",
        "pulp-sdk-windows-arm64.tar.gz",
        "pulp-sdk-windows-x64.tar.gz",
    ]

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RELEASE_CLI.read_text(encoding="utf-8")
        cls.workflow = yaml.safe_load(cls.text)

    def test_no_publish_coordinator_workflow_exists(self) -> None:
        self.assertFalse(
            RELEASE_PUBLISH.exists(),
            "release-publish.yml is back. Publication must live in release-cli's "
            "`release` job. A cross-workflow coordinator can only publish when "
            "EVERY leg it waits on succeeds, which makes the release only as "
            "reliable as its worst leg.",
        )

    def test_release_job_generates_the_appcast_itself(self) -> None:
        """The Sparkle feed must be written where the release is written.

        appcast.xml is a pure function of the tag name and the date — no
        enclosure, no signature, no dependency on a notarized artifact. Sourcing
        it from sign-and-release was the ONLY thing coupling publication to macOS
        signing, and that coupling is what let a notarization hiccup withhold a
        complete SDK release.
        """
        self.assertIn("name: Generate appcast.xml (Sparkle feed)", self.text)
        self.assertIn("artifacts/appcast.xml", self.text)

    def test_no_advisory_gate_competes_with_the_release_for_hosted_macos(self) -> None:
        """The release path must not run an advisory universal/auval gate.

        `universal-arch-gate` pinned itself to GitHub-hosted `macos-15` and took
        ~2 hours — the same scarce pool the release's REQUIRED `darwin-x64` legs
        need. At ~14 tags/day it queued ~28 macOS-hours/day of hosted work AHEAD of
        the leg that actually gates publication, while the self-hosted Studios sat
        idle. And it was redundant: nightly-intel.yml's `universal-crosscheck` is
        the same check, and intel-portability.yml covers Intel at PR time.

        Two failure modes here, and both cost releases: an advisory job left in
        `needs` still BLOCKS (continue-on-error makes the RESULT advisory, not the
        dependency), and even out of `needs` it still STARVES.
        """
        self.assertNotIn(
            "universal-arch-gate",
            self.workflow["jobs"],
            "The universal/auval gate is back on the release path. It is redundant "
            "with nightly-intel.yml, and it competes with the release's own "
            "required darwin-x64 legs for the hosted macOS pool.",
        )
        needs = self.workflow["jobs"]["release"]["needs"]
        self.assertEqual(sorted(needs), ["build-cli", "smoke-cli"])

    def test_release_job_checksums_and_publishes_in_one_job(self) -> None:
        generate = "release_checksum_manifest.py generate"
        verify = "release_checksum_manifest.py verify"
        upload = 'gh release upload "${TAG}" release-assets/SHA256SUMS'
        publish = 'gh release edit "${TAG}"'
        for needle in (generate, verify, upload, publish, "--exact-required"):
            self.assertIn(needle, self.text)
        self.assertLess(self.text.index(generate), self.text.index(publish))
        self.assertLess(self.text.index(verify), self.text.index(publish))
        self.assertLess(self.text.index(upload), self.text.index(publish))

    def test_every_user_facing_asset_is_required_before_publish(self) -> None:
        for asset in self.REQUIRED_RELEASE_ASSETS:
            self.assertIn(asset, self.text)

    def test_republishing_an_already_published_tag_is_a_no_op(self) -> None:
        """Re-dispatch must be idempotent — release-reconcile.yml depends on it.

        A published GitHub release is IMMUTABLE: uploading an asset onto one fails
        hard. The reconciler re-dispatches this workflow to repair stuck releases,
        and the tag may well have been published in between (a slow first run
        finishing, or a human backfilling). So both the create step and the
        finalizer must be skipped when the tag is already published — otherwise
        recovery blows up on exactly the tags that turned out fine.
        """
        steps = self.workflow["jobs"]["release"]["steps"]
        by_name = {s.get("name"): s for s in steps}

        guard = by_name.get("Is this tag already published?")
        self.assertIsNotNone(
            guard, "release job must check for an existing published release."
        )
        self.assertEqual(guard["id"], "existing")

        for step_name in ("Create release", "Verify assets, generate SHA256SUMS, publish"):
            with self.subTest(step=step_name):
                self.assertEqual(
                    by_name[step_name].get("if"),
                    "steps.existing.outputs.published != 'true'",
                    f"`{step_name}` must be skipped when the tag is already "
                    "published, or a re-dispatch will try to write to an "
                    "immutable release.",
                )

    def test_runs_are_serialized_on_the_TAG_not_the_dispatch_ref(self) -> None:
        """Same-tag runs must share one concurrency group.

        `github.ref` for a workflow_dispatch is the ref it was dispatched FROM
        (`refs/heads/main`), so keying on it put a repair dispatch for v0.655.0 in
        a DIFFERENT group from v0.655.0's own tag-push run — letting both finalize
        the same release concurrently. softprops/action-gh-release replaces
        same-named assets, so one run could delete an asset out from under the
        other's just-verified exact-asset set and publish an incomplete release.
        """
        group = self.workflow["concurrency"]["group"]
        self.assertIn("inputs.version", group)
        self.assertIn("github.ref_name", group)
        self.assertNotIn(
            "github.ref }}",
            group,
            "release-cli concurrency is keyed on github.ref again — for a "
            "workflow_dispatch that is the dispatch ref (main), not the tag being "
            "built, so a repair run can race the tag's own run.",
        )
        self.assertIs(self.workflow["concurrency"]["cancel-in-progress"], False)

    def test_run_name_carries_the_tag_so_repairs_are_attributable(self) -> None:
        """release-reconcile.py identifies repair runs by display_title."""
        run_name = self.workflow["run-name"]
        self.assertIn("inputs.version", run_name)
        self.assertIn("github.ref_name", run_name)
        self.assertTrue(run_name.startswith("Release "))

    def test_publish_transaction_is_globally_serialized(self) -> None:
        """The latest-pointer decision is a read-then-write; serialize it.

        Releases now finish OUT OF ORDER, so two tags could otherwise both read
        while an older tag was latest, both conclude they are greatest, and the
        later write would move /releases/latest BACKWARD.
        """
        job = self.workflow["jobs"]["release"]
        self.assertEqual(job["concurrency"]["group"], "release-publish")
        self.assertIs(job["concurrency"]["cancel-in-progress"], False)

    def test_latest_pointer_cannot_move_backward(self) -> None:
        """Releases now complete OUT OF ORDER, so `--latest` must be conditional.

        A slow v0.645 can finish after a fast v0.646. Unconditionally claiming the
        latest-release pointer (what the old coordinator did) would regress
        /releases/latest to the older tag.
        """
        self.assertIn("latest_flag=\"--latest=false\"", self.text)
        self.assertIn("greatest published SemVer", self.text)


class SignAndReleaseCannotTouchTheRelease(unittest.TestCase):
    """sign-and-release.yml must be structurally incapable of gating a release.

    It formerly polled `gh release view` 150 times at 20s (50 minutes) for a draft
    that release-cli does not create until its full build matrix finishes — which
    really takes 70-165+ minutes. So on every slow release, this workflow failed by
    timeout, and the coordinator then withheld an otherwise-complete release.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SIGN_AND_RELEASE.read_text(encoding="utf-8")
        cls.workflow = yaml.safe_load(cls.text)

    def test_no_polling_for_the_release(self) -> None:
        for needle in ("gh release view", "gh release upload", "gh release edit"):
            self.assertNotIn(
                needle,
                self.text,
                f"sign-and-release.yml must not run `{needle}`. release-cli.yml's "
                "`release` job is the sole writer of the GitHub Release; a second "
                "writer here can only ever delay or clobber it.",
            )

    def test_contents_scope_is_read_only(self) -> None:
        """Withholding the write scope is what makes single-ownership structural."""
        perms = self.workflow["jobs"]["build-and-sign-macos"]["permissions"]
        self.assertEqual(
            perms.get("contents"),
            "read",
            "sign-and-release.yml regained `contents: write`. It has no business "
            "writing to a release — release-cli.yml owns publication.",
        )


class NoSupersedeReaper(unittest.TestCase):
    """auto-release.yml must never cancel a release run or delete a release.

    The "supersede reaper" cancelled any in-flight release-cli / sign-and-release
    run, and DELETED any draft release, whose tag was older than the latest
    published release. Because the pipeline (70-165+ min) outlasts the gap between
    tags (~100 min), releases routinely complete out of order — so "older SemVer"
    did not mean "obsolete", and the reaper destroyed healthy, in-flight releases.
    """

    @classmethod
    def setUpClass(cls) -> None:
        path = REPO_ROOT / ".github" / "workflows" / "auto-release.yml"
        cls.text = path.read_text(encoding="utf-8")
        cls.workflow = yaml.safe_load(cls.text)

    def test_no_actions_write_scope(self) -> None:
        self.assertNotIn(
            "actions",
            self.workflow.get("permissions", {}),
            "auto-release.yml regained an `actions` scope. Without it the workflow "
            "CANNOT cancel a release run, which is the point: the reaper is not "
            "meant to be re-addable by accident.",
        )

    def test_does_not_cancel_runs_or_delete_releases(self) -> None:
        # Match the destructive CALLS, not any string containing a run URL — the
        # stranded-fix/feat tracker legitimately embeds an .../actions/runs/<id>
        # link in an issue body.
        for forbidden in (
            "/cancel",          # POST /repos/…/actions/runs/{id}/cancel
            '"DELETE"',         # DELETE /repos/…/releases/{id}
            "-X DELETE",
            "gh run cancel",
            "gh release delete",
        ):
            self.assertNotIn(
                forbidden,
                self.text,
                f"auto-release.yml contains {forbidden!r} — it is cancelling runs "
                "or deleting releases again. Nothing in the release path may "
                "destroy work that is still in flight.",
            )


class ReleaseReconcilerIsTheSingleWatchdog(unittest.TestCase):
    """One reconciler that FIXES, not four watchdogs that only report.

    release-guard, release-health, release-cli-watchdog and release-draft-stuck-check
    filed 413 issues in two weeks and fixed nothing; recovery was always a human
    running `gh workflow run` by hand. Their grace windows (15/30/45/60 min) were
    all below the real pipeline duration, so they also alarmed on healthy releases.
    """

    RETIRED = [
        "release-guard.yml",
        "release-health.yml",
        "release-cli-watchdog.yml",
        "release-draft-stuck-check.yml",
    ]

    def test_retired_watchdogs_stay_retired(self) -> None:
        for name in self.RETIRED:
            self.assertFalse(
                (REPO_ROOT / ".github" / "workflows" / name).exists(),
                f"{name} is back. Release health belongs to release-reconcile.yml, "
                "which re-dispatches stuck releases and keeps ONE incident issue. "
                "Adding another reporter re-creates the issue firehose.",
            )

    def test_reconciler_exists_and_can_dispatch_but_not_cancel(self) -> None:
        path = REPO_ROOT / ".github" / "workflows" / "release-reconcile.yml"
        self.assertTrue(path.exists(), "release-reconcile.yml is missing.")
        workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
        perms = workflow["permissions"]
        self.assertEqual(perms["actions"], "write")   # to re-dispatch release-cli
        self.assertEqual(perms["contents"], "read")   # never writes release state


class ReleaseArtifactAttestations(unittest.TestCase):
    """Release workflows should emit build provenance attestations."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.release_cli = RELEASE_CLI.read_text(encoding="utf-8")
        cls.sign_and_release = SIGN_AND_RELEASE.read_text(encoding="utf-8")

    def test_release_cli_attests_cli_and_sdk_archives(self) -> None:
        self.assertIn("id-token: write", self.release_cli)
        self.assertIn("attestations: write", self.release_cli)
        self.assertIn("name: Attest CLI and SDK artifacts", self.release_cli)
        self.assertIn("uses: actions/attest@v4", self.release_cli)
        self.assertIn("pulp-${{ matrix.platform }}.*", self.release_cli)
        self.assertIn("pulp-sdk-${{ matrix.platform }}.*", self.release_cli)

    def test_release_cli_attests_the_appcast_it_now_generates(self) -> None:
        """The appcast attestation follows the appcast into release-cli.

        It used to live in sign-and-release, which no longer produces the feed and
        no longer holds the write scopes to attest anything onto a release.
        """
        self.assertIn("name: Attest appcast", self.release_cli)
        self.assertIn("subject-path: artifacts/appcast.xml", self.release_cli)
        self.assertNotIn("name: Attest appcast", self.sign_and_release)


class ReleaseBotSshSigning(unittest.TestCase):
    """Automation-created release refs must be signed by the release bot."""

    BOT_EMAIL = "25807+danielraffel@users.noreply.github.com"

    @classmethod
    def setUpClass(cls) -> None:
        cls.auto_release = AUTO_RELEASE.read_text(encoding="utf-8")
        cls.intent_bump = INTENT_BUMP_ON_MERGE.read_text(encoding="utf-8")
        cls.post_tag_sync = POST_TAG_SYNC.read_text(encoding="utf-8")
        cls.helper = RELEASE_SIGNING_HELPER.read_text(encoding="utf-8")

    def test_signing_helper_requires_release_bot_private_key_secret(self) -> None:
        self.assertIn("RELEASE_BOT_SSH_SIGNING_KEY:?RELEASE_BOT_SSH_SIGNING_KEY", self.helper)
        self.assertIn("git config --global gpg.format ssh", self.helper)
        self.assertIn("git config --global user.signingkey", self.helper)
        self.assertIn("git config --global commit.gpgsign true", self.helper)
        self.assertIn("git config --global tag.gpgSign true", self.helper)
        self.assertIn(self.BOT_EMAIL, self.helper)

    def test_auto_release_signs_version_tags(self) -> None:
        self.assertIn("name: Configure release bot SSH signing", self.auto_release)
        self.assertIn("RELEASE_BOT_SSH_SIGNING_KEY: ${{ secrets.RELEASE_BOT_SSH_SIGNING_KEY }}", self.auto_release)
        self.assertIn("bash tools/scripts/configure_release_bot_ssh_signing.sh", self.auto_release)
        self.assertIn(f'git config user.email "{self.BOT_EMAIL}"', self.auto_release)
        self.assertIn('git tag -s "$tag"', self.auto_release)
        self.assertNotIn('git tag -a "$tag"', self.auto_release)
        self.assertLess(
            self.auto_release.index("name: Configure release bot SSH signing"),
            self.auto_release.index("name: Create tags for moved surfaces"),
        )

    def test_intent_bump_commits_are_signed(self) -> None:
        self.assertIn("name: Configure release bot SSH signing", self.intent_bump)
        self.assertIn("RELEASE_BOT_SSH_SIGNING_KEY: ${{ secrets.RELEASE_BOT_SSH_SIGNING_KEY }}", self.intent_bump)
        self.assertIn("bash tools/scripts/configure_release_bot_ssh_signing.sh", self.intent_bump)
        self.assertIn(f'git config user.email "{self.BOT_EMAIL}"', self.intent_bump)
        self.assertIn('git commit -S -m "chore: bump versions"', self.intent_bump)

    def test_post_tag_sync_commits_use_signed_bot_identity(self) -> None:
        self.assertIn("name: Configure release bot SSH signing", self.post_tag_sync)
        self.assertIn("RELEASE_BOT_SSH_SIGNING_KEY: ${{ secrets.RELEASE_BOT_SSH_SIGNING_KEY }}", self.post_tag_sync)
        self.assertIn("bash tools/scripts/configure_release_bot_ssh_signing.sh", self.post_tag_sync)


class StrandedReleaseTrackerWorkflow(unittest.TestCase):
    """The stranded-release detector must produce actionable trackers.

    SHA-keyed trackers have no semantic version in their title, so the daily
    version reaper must skip them without aborting its entire sweep.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.auto_release = AUTO_RELEASE.read_text(encoding="utf-8")
        cls.watchdog_reaper = WATCHDOG_REAPER.read_text(encoding="utf-8")
        cls.version_skill_check = VERSION_SKILL_CHECK.read_text(encoding="utf-8")

    def test_version_gate_reruns_when_pr_title_is_edited(self) -> None:
        self.assertRegex(
            self.version_skill_check,
            r"(?m)^\s{4}types:\s*\[opened, synchronize, reopened, edited\]\s*$",
        )

    def test_version_gate_reports_expected_tags_to_pr_queue(self) -> None:
        self.assertIn("name: Report expected release tags", self.version_skill_check)
        self.assertIn(
            "python3 tools/scripts/pr_release_tag_report.py",
            self.version_skill_check,
        )
        self.assertIn("github.event_name == 'pull_request'", self.version_skill_check)
        self.assertIn("github.base_ref == 'main'", self.version_skill_check)
        self.assertIn('--pr-title "$GITHUB_PR_TITLE"', self.version_skill_check)
        self.assertIn('tee -a "$GITHUB_STEP_SUMMARY"', self.version_skill_check)

    def test_catch_up_command_analyzes_the_stranded_merge(self) -> None:
        self.assertIn(
            "--base %s --head %s",
            self.auto_release,
        )
        self.assertIn('"$range_base" "$HEAD_SHA"', self.auto_release)
        self.assertIn("--apply-version-base HEAD", self.auto_release)
        self.assertIn("--recover-stranded-release", self.auto_release)
        self.assertIn("--recover-surfaces %s", self.auto_release)
        self.assertIn("--recover-levels %s", self.auto_release)
        self.assertIn("git fetch origin main", self.auto_release)
        self.assertIn("origin/main", self.auto_release)
        self.assertIn('--require-bump-for-fix-feat --pr-title ""', self.auto_release)
        self.assertNotIn(
            'echo "   python3 tools/scripts/version_bump_check.py --mode=apply"',
            self.auto_release,
            "A fresh catch-up branch equals origin/main, so the default range "
            "is empty and --mode=apply would edit nothing.",
        )

    def test_detector_provisions_its_release_stuck_label(self) -> None:
        self.assertIn(
            'gh api -X POST "/repos/${GITHUB_REPOSITORY}/labels"',
            self.auto_release,
        )
        self.assertIn('-f name="release-stuck"', self.auto_release)
        self.assertIn('>/dev/null 2>&1 || true', self.auto_release)

    def test_unresolved_revert_of_metadata_fails_closed(self) -> None:
        self.assertIn(
            'git rev-parse --verify --quiet --end-of-options "${revert_sha}^{commit}"',
            self.auto_release,
        )

    def test_squash_guard_reads_embedded_source_skip_trailers(self) -> None:
        self.assertIn("COMMIT_MESSAGES squash bodies", self.auto_release)
        self.assertIn(
            "^[[:space:]]*release:[[:space:]]+skip",
            self.auto_release,
        )
        self.assertIn(
            "^[[:space:]]*version-bump:[[:space:]]+skip",
            self.auto_release,
        )
        self.assertIn("bump_body=$(git log -1 --format=%B", self.auto_release)
        self.assertIn(
            'printf \'%s\\n\' "$bump_body" | grep -iqE',
            self.auto_release,
        )
        self.assertIn(
            "Unresolved Revert-Of trailer; treating this as an ordinary change",
            self.auto_release,
        )

    def test_stranded_detector_classifies_the_whole_push_range(self) -> None:
        self.assertIn("tracker_opt_out=0", self.auto_release)
        self.assertIn("BEFORE_SHA: ${{ github.event.before }}", self.auto_release)
        self.assertIn("FIRST_PUSH_SHA", self.auto_release)
        self.assertIn("git fetch --no-tags origin", self.auto_release)
        self.assertIn("classify-unreleased-range", self.auto_release)
        self.assertIn('sdk_covered="$SDK_SHOULD_TAG"', self.auto_release)
        self.assertIn("sdk_skip_bump=$sdk_skip_bump", self.auto_release)
        self.assertIn("SDK_SKIP_BUMP: ${{ steps.versions.outputs.sdk_skip_bump }}", self.auto_release)
        self.assertIn('"${SDK_SKIP_BUMP:--}" "${PLUGIN_SKIP_BUMP:--}"', self.auto_release)
        self.assertIn("refs/pull/${pr_number}/head:${source_ref}", self.auto_release)
        self.assertIn('"$source_base" "$source_head"', self.auto_release)
        self.assertIn("uncovered_surfaces", self.auto_release)
        self.assertIn("Live release signal", self.auto_release)

    def test_reaper_neutralizes_sha_tracker_version_miss(self) -> None:
        self.assertIn("| head -1 || true)", self.watchdog_reaper)
        self.assertIn('if [ -z "$ver" ]; then', self.watchdog_reaper)
        self.assertIn("Skipping SHA-keyed tracker", self.watchdog_reaper)


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

    def test_create_step_defers_the_latest_pointer_to_the_publish_step(self) -> None:
        """The create step must not claim `latest`; the publish step decides.

        `softprops/action-gh-release` can only be told latest-or-not up front,
        which cannot express "only if this is the greatest published SemVer". With
        releases now completing OUT OF ORDER (a slow v0.645 finishing after a fast
        v0.646), that guard is the difference between a correct pointer and
        regressing /releases/latest to an older tag.
        """
        step_block = self._find_step_block("Create release")
        self.assertIn("make_latest: false", step_block)
        self.assertNotIn(
            "make_latest: true",
            step_block,
            "Unconditional make_latest lets an out-of-order or backfilled release "
            "move GitHub's /releases/latest pointer backward.",
        )

    def test_release_job_permissions_cover_body_composition(self) -> None:
        job_block = self._find_release_job_block()
        self.assertIn("contents: write", job_block)
        self.assertIn("issues: read", job_block)


class SignAndReleaseMacosRoutingTest(unittest.TestCase):
    """Signing must share release-cli's dedicated macOS runner priority."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SIGN_AND_RELEASE.read_text(encoding="utf-8")

    def test_dedicated_release_runner_precedes_hosted_fallback(self) -> None:
        release_pos = self.text.index("PULP_RELEASE_MACOS_RUNS_ON_JSON")
        namespace_pos = self.text.index("PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON")
        fallback_pos = self.text.index('runs_on_json=["macos-15"]')
        self.assertNotIn("PULP_LOCAL_MACOS_RUNS_ON_JSON", self.text)
        self.assertLess(release_pos, namespace_pos)
        self.assertLess(namespace_pos, fallback_pos)

    def test_signing_keychain_is_unique_and_always_cleaned_up(self) -> None:
        self.assertIn("pulp-signing-${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}", self.text)
        self.assertIn("PREVIOUS_DEFAULT_KEYCHAIN", self.text)
        self.assertIn("s/^[[:space:]]*//", self.text)
        self.assertIn("s/[[:space:]]*$//", self.text)
        self.assertIn("name: Clean up signing keychain", self.text)
        self.assertIn("if: always() && env.SIGNING_KEYCHAIN != ''", self.text)
        self.assertIn('security delete-keychain "$SIGNING_KEYCHAIN"', self.text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
