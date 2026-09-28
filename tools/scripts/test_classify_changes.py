#!/usr/bin/env python3
"""Tests for classify_changes.py.

The classifier is safety-critical: a wrong "skip" lets a real
regression merge without a native build. These tests pin the
fail-closed contract — especially that anything NOT explicitly
skip-safe forces the native build.

Run:
    python3 tools/scripts/test_classify_changes.py
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

THIS_DIR = Path(__file__).parent
SCRIPT = THIS_DIR / "classify_changes.py"

_spec = importlib.util.spec_from_file_location("classify_changes", SCRIPT)
classify = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(classify)


class SkipSafeTests(unittest.TestCase):
    def test_markdown_anywhere_is_skip_safe(self) -> None:
        for path in ("README.md", "docs/guide.md", "core/view/NOTES.md",
                     ".agents/skills/ci/SKILL.md", "CHANGELOG.md"):
            self.assertTrue(classify.is_skip_safe(path), path)

    def test_skip_safe_prefixes(self) -> None:
        for path in ("docs/reference/cli.md", "docs/assets/logo.png",
                     "planning/STATUS.md", ".githooks/pre-push",
                     ".shipyard/config.toml", ".shipyard.local/config.toml"):
            self.assertTrue(classify.is_skip_safe(path), path)

    def test_skip_safe_exact(self) -> None:
        for path in (
            ".gitignore", ".gitattributes", "CODEOWNERS",
            "tools/scripts/test_prepush_gate_supervisor.py",
        ):
            self.assertTrue(classify.is_skip_safe(path), path)

    def test_docs_migrations_md_is_NOT_skip_safe(self) -> None:
        # docs/migrations/*.md is globbed with CONFIGURE_DEPENDS into the
        # generated migration_index.cpp by tools/cli/CMakeLists.txt — the
        # deny-list must override the .md and docs/ skip-safe rules.
        for path in ("docs/migrations/2026-05-01-release.md",
                     "docs/migrations/v2-upgrade.md"):
            self.assertFalse(classify.is_skip_safe(path), path)

    def test_docs_migrations_prefix_boundary(self) -> None:
        # Only the real docs/migrations/ tree is generated into C++.
        self.assertTrue(classify.is_skip_safe("docs/migrations-guide.md"))
        self.assertTrue(classify.is_skip_safe("docs/migrations_old/foo.md"))
        self.assertFalse(classify.is_skip_safe("docs/migrations/foo.md"))

    def test_consumption_census_documents_are_NOT_skip_safe(self) -> None:
        # The published consumption census and its schema live under docs/, so
        # every skip-safe rule would otherwise admit them. Their only gates
        # (consumption-census-drift / -schema / -negative-contract) exist only
        # inside a configured build tree, so a skip-safe census change is a
        # claim about the build graph that nothing checks.
        for path in ("docs/status/consumption-profiles.json",
                     "docs/status/consumption-profiles.schema.json"):
            self.assertFalse(classify.is_skip_safe(path), path)

    def test_consumption_census_force_build_stays_precise(self) -> None:
        # The deny-list entry must not swallow the rest of docs/status/: those
        # manifests have their own build-free checks, and forcing a native
        # build for them would make every docs PR pay for a census it cannot
        # affect.
        for path in ("docs/status/modules.yaml",
                     "docs/status/support-matrix.yaml",
                     "docs/status/cli-commands.yaml",
                     "docs/status/tools.yaml",
                     "docs/status/consumption.md",
                     "docs/guides/test-lanes.md"):
            self.assertTrue(classify.is_skip_safe(path), path)

    def test_build_inputs_are_NOT_skip_safe(self) -> None:
        for path in ("core/signal/src/fft.cpp",
                     "core/view/include/pulp/view/view.hpp",
                     "CMakeLists.txt", "core/audio/CMakeLists.txt",
                     "tools/cmake/PulpUtils.cmake", "apple/Sources/x.swift",
                     "examples/pulp-gain/main.cpp", "test/test_state.cpp",
                     "setup.sh", "Package.swift",
                     "core/view/js/web-compat-element.js",
                     ".github/workflows/build.yml",
                     "tools/scripts/classify_changes.py",
                     "tools/scripts/resolve_runs_on.py",
                     ".shipyard.toml", "compat.json"):
            self.assertFalse(classify.is_skip_safe(path), path)

    def test_empty_path_is_not_skip_safe(self) -> None:
        self.assertFalse(classify.is_skip_safe(""))


class NativeBuildRequiredTests(unittest.TestCase):
    def test_empty_diff_forces_build(self) -> None:
        # Fail-closed: unknown diff -> build.
        self.assertTrue(classify.native_build_required([]))

    def test_all_docs_skips_build(self) -> None:
        self.assertFalse(classify.native_build_required(
            ["README.md", "docs/guide.md", "CHANGELOG.md"]))

    def test_migration_doc_forces_build(self) -> None:
        # A migrations-only PR changes generated C++ — must build.
        self.assertTrue(classify.native_build_required(
            ["docs/migrations/2026-05-19-foo.md"]))

    def test_migration_doc_among_plain_docs_forces_build(self) -> None:
        # One migration doc among ordinary docs still forces the build.
        self.assertTrue(classify.native_build_required(
            ["README.md", "docs/guide.md",
             "docs/migrations/2026-05-19-foo.md"]))

    def test_any_code_file_forces_build(self) -> None:
        # One code file among many docs still forces the build.
        self.assertTrue(classify.native_build_required(
            ["README.md", "docs/guide.md", "core/signal/src/fft.cpp"]))

    def test_pure_code_forces_build(self) -> None:
        self.assertTrue(classify.native_build_required(
            ["core/midi/src/mpe_voice_tracker.cpp"]))

    def test_skill_only_pr_skips_build(self) -> None:
        # A skill-doc-only PR (markdown) is skip-safe.
        self.assertFalse(classify.native_build_required(
            [".agents/skills/ci/SKILL.md"]))

    def test_workflow_change_forces_build(self) -> None:
        # Changing build.yml itself must get a real run to validate.
        self.assertTrue(classify.native_build_required(
            [".github/workflows/build.yml"]))

    def test_classifier_change_forces_build(self) -> None:
        # Changing the classifier must get a real run.
        self.assertTrue(classify.native_build_required(
            ["tools/scripts/classify_changes.py"]))

    def test_tools_scripts_not_skip_safe(self) -> None:
        # Apart from reviewed exact exceptions, tools/scripts/** remains
        # build-forcing because neighboring scripts can be build-coupled.
        self.assertTrue(classify.native_build_required(
            ["tools/scripts/source_tree_pollution_check.py"]))

    def test_gate_supervisor_regression_slice_skips_native_build(self) -> None:
        self.assertFalse(classify.native_build_required([
            ".githooks/lib/gate-supervisor.py",
            "tools/scripts/test_prepush_gate_supervisor.py",
            ".agents/skills/ci/SKILL.md",
        ]))


class ConsumptionCensusNativePathTests(unittest.TestCase):
    """The census-only diff shape that reached main unmeasured.

    `docs/status/consumption-profiles.json` records target-graph facts — link
    closures and exported public-header counts — that only a configured build
    tree can confirm. Its gates (`consumption-census-drift` and siblings in
    test/cmake/consumption_census_tests.cmake) therefore run nowhere else: they
    are not in tools/ci/source_selftests.json, so the build-free required
    context never sees them. A census change that classifies skip-safe gets the
    hosted placeholder instead of the native leg, lands whatever it claims, and
    then fails every later merge group whose batches did not touch it.
    """

    REPO_ROOT = THIS_DIR.parents[1]

    def _census_module_paths(self) -> tuple[str, str]:
        """The census document + schema, re-derived from the census tool.

        Read from consumption_census.py's own constants rather than retyped, so
        renaming either file fails this test instead of silently restoring the
        skip-safe classification.
        """
        spec = importlib.util.spec_from_file_location(
            "consumption_census_for_classify_test",
            THIS_DIR / "consumption_census.py",
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return (
            module.CENSUS_RELPATH.as_posix(),
            module.SCHEMA_RELPATH.as_posix(),
        )

    def test_census_only_diff_requires_the_native_build(self) -> None:
        # The exact one-file diff shape that took the hosted placeholder.
        self.assertTrue(classify.native_build_required(
            ["docs/status/consumption-profiles.json"]))

    def test_census_schema_only_diff_requires_the_native_build(self) -> None:
        self.assertTrue(classify.native_build_required(
            ["docs/status/consumption-profiles.schema.json"]))

    def test_census_change_among_plain_docs_requires_the_native_build(self) -> None:
        self.assertTrue(classify.native_build_required([
            "README.md",
            "docs/guides/test-lanes.md",
            "docs/status/consumption-profiles.json",
        ]))

    def test_census_tool_paths_force_the_native_build(self) -> None:
        census, schema = self._census_module_paths()
        # Control: the re-derivation must have produced the real pair. An
        # empty or renamed constant would otherwise let the assertions below
        # pass over paths that classify native for unrelated reasons.
        self.assertTrue(census.startswith("docs/"), census)
        self.assertTrue(schema.startswith("docs/"), schema)
        self.assertNotEqual(census, schema)
        for path in (census, schema):
            self.assertFalse(classify.is_skip_safe(path), path)
            self.assertTrue(classify.native_build_required([path]), path)

    def test_census_gates_exist_only_inside_a_build_tree(self) -> None:
        """Pin the premise the deny-list entry rests on.

        The census gates are worth forcing a native build for only because
        nothing else runs them. If they ever move to the build-free lane in
        tools/ci/source_selftests.json, that reasoning changes and this test
        says so instead of leaving a stale rationale in the classifier.
        """
        manifest = (
            self.REPO_ROOT / "test/cmake/consumption_census_tests.cmake"
        ).read_text(encoding="utf-8")
        gates = [
            "consumption-census-drift",
            "consumption-census-schema",
            "consumption-census-negative-contract",
        ]
        # Control: the manifest must actually register them. A renamed or
        # emptied manifest would make the absence assertion below vacuous.
        # Match to the end of the name token, or `consumption-census-drift`
        # would be satisfied by a renamed `…-drift-anything` registration.
        registrations = set(
            re.findall(r"add_test\(NAME\s+(\S+)", manifest)
        )
        for gate in gates:
            self.assertIn(gate, registrations, sorted(registrations))
        selftests = json.loads(
            (self.REPO_ROOT / "tools/ci/source_selftests.json").read_text(
                encoding="utf-8"
            )
        )
        registered = {test["name"] for test in selftests["tests"]}
        # Control: the build-free lane must be populated, or "not in it" is
        # true of everything.
        self.assertGreater(len(registered), 20, len(registered))
        for gate in gates:
            self.assertNotIn(gate, registered, gate)

    def _exported_public_header_roots(self) -> set[str]:
        """Exported include roots, re-derived from the committed census."""
        document = json.loads(
            (self.REPO_ROOT / "docs/status/consumption-profiles.json").read_text(
                encoding="utf-8"
            )
        )
        roots: set[str] = set()
        for profile in document["profiles"].values():
            for target in profile["targets"].values():
                roots.update(target["public_headers"].get("roots", []))
        return roots

    def test_every_exported_public_header_root_forces_the_native_build(self) -> None:
        # A new header under a root a target already exports drifts the census
        # with no CMake or symbol change, so the header side of the gate is only
        # honest while every exported root classifies native.
        roots = self._exported_public_header_roots()
        # Control: the re-derivation must find the real roots. A structural
        # change that yields an empty set would make every assertion below
        # vacuous, which is exactly how a gate goes green measuring nothing.
        self.assertGreaterEqual(len(roots), 20, sorted(roots))
        in_repo = sorted(
            root for root in roots if (self.REPO_ROOT / root).is_dir()
        )
        self.assertGreaterEqual(len(in_repo), 20, in_repo)
        # Second control: the roots must actually hold headers.
        with_headers = [
            root
            for root in in_repo
            if any((self.REPO_ROOT / root).rglob("*.hpp"))
            or any((self.REPO_ROOT / root).rglob("*.h"))
        ]
        self.assertGreaterEqual(len(with_headers), 20, with_headers)
        for root in sorted(roots):
            path = f"{root}/pulp/probe_header.hpp"
            self.assertFalse(classify.is_skip_safe(path), path)
            self.assertTrue(classify.native_build_required([path]), path)


class AgentCapabilityInstalledSdkRequiredTests(unittest.TestCase):
    def test_capability_contract_surfaces_select_installed_sdk_proof(self) -> None:
        for path in (
            ".agents/skills/agent-capabilities/SKILL.md",
            "docs/status/agent-capabilities.json",
            "docs/status/agent-capability-surface.schema.json",
            "test/test_agent_capability_compile.cpp",
            "test/cmake/quality_tests.cmake",
            "tools/agent-capabilities/contract-history.json",
            "CMakeLists.txt",
            "core/audio/CMakeLists.txt",
            "tools/cmake/PulpConfig.cmake.in",
            "tools/cmake/PulpInstallRules.cmake",
            "tools/dsp_vocabulary.py",
            "tools/scripts/agent_capability_registry.py",
            "tools/scripts/test_agent_capability_manifest.py",
        ):
            with self.subTest(path=path):
                self.assertTrue(
                    classify.agent_capability_installed_sdk_required([path])
                )

    def test_selected_skip_safe_surface_forces_containing_native_job(self) -> None:
        path = ".agents/skills/agent-capabilities/SKILL.md"
        self.assertTrue(classify.is_skip_safe(path))
        self.assertTrue(classify.agent_capability_installed_sdk_required([path]))

        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--mode=files", "--json", path],
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertTrue(json.loads(result.stdout)["native_build_required"])

    def test_unrelated_runtime_and_test_changes_do_not_select_proof(self) -> None:
        self.assertFalse(
            classify.agent_capability_installed_sdk_required(
                ["core/audio/src/device.cpp", "test/test_child_process.cpp"]
            )
        )


class IosCompileRequiredTests(unittest.TestCase):
    def test_current_reviewed_mandatory_and_bounded_surfaces_skip(self) -> None:
        for paths in (
            ["docs/guides/local-ci.md"],
            ["test/test_child_process.cpp"],
            ["tools/cli/cmd_forge.cpp"],
            ["docs/reference/cli.md", "tools/cli/cmd_dsp.cpp"],
        ):
            with self.subTest(paths=paths):
                self.assertFalse(classify.ios_compile_required(paths))

    def test_test_tree_docs_and_python_tests_skip(self) -> None:
        # The gate configures with PULP_BUILD_TESTS=OFF: unit-test sources,
        # test manifests, Python suites and prose never reach it.
        for paths in (
            ["test/test_widgets.cpp"],
            ["test/cmake/quality_tests.cmake"],
            ["test/test_state.cpp"],
            ["tools/scripts/test_classify_changes.py"],
            ["docs/status/sequencer-exposure.json", "docs/guides/test-lanes.md"],
            [".agents/skills/audio-harness/SKILL.md"],
            [".claude-plugin/plugin.json", "CHANGELOG.md"],
            # Feeds only the desktop CLI's generated migration index.
            ["docs/migrations/v1-to-v2.md"],
        ):
            with self.subTest(paths=paths):
                self.assertFalse(classify.ios_compile_required(paths))

    def test_paths_the_ios_gate_reads_still_run(self) -> None:
        for paths in (
            # The gate scripts build.yml executes.
            ["test/cmake/test_ios_compile_gate.sh"],
            ["test/cmake/test_ios_source_syntax.sh"],
            # Compiled for iOS by core/midi.
            ["test/ios/coremidi_backend_harness.mm"],
            ["test/test_coremidi_shared_client.cpp"],
            # Named by example / tooling CMake the GPU leg configures.
            ["test/harness/rt_allocation_probe.cpp"],
            ["test/fixtures/native_ui_link_floor/CMakeLists.txt"],
            ["docs/status/gpu-recipes.yaml"],
            ["docs/status/forge-catalog.schema.json"],
            # Test CMake manifests are topology, but a CMakeLists is global.
            ["test/CMakeLists.txt"],
            # Neighbouring non-test scripts may be build-coupled.
            ["tools/scripts/classify_changes.py"],
            ["tools/scripts/fetch_skia_for_release.py"],
            # A test-named script matching a sensitive full-required glob.
            ["tools/scripts/test_release_notes.py"],
            ["test/test_widgets.cpp", "core/midi/src/midi_system.cpp"],
        ):
            with self.subTest(paths=paths):
                self.assertTrue(classify.ios_compile_required(paths))

    def test_every_test_or_docs_path_named_by_non_test_cmake_runs_the_gate(
        self,
    ) -> None:
        """Re-derive the deny list from the live tree so it cannot go stale.

        The iOS GPU leg configures with examples ON, so a test/ or docs/ file
        that any non-test CMake file names is a configure input: renaming or
        deleting it fails the gate. Every such reference must stay denied.
        """
        import re

        repo = THIS_DIR.parent.parent
        listed = subprocess.run(
            ["git", "ls-files", "*CMakeLists.txt", "*.cmake", "*.cmake.in"],
            cwd=repo, capture_output=True, text=True, check=True,
        ).stdout.split()
        # The root adds the desktop CLI and its GPU probe only for non-iOS
        # configurations, so their CMake never runs in the iOS gate. Pin that
        # guard here: if it ever moves, their references count again.
        root_text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
        for subdir in ("tools/cli", "tools/cli/gpu_probe"):
            guarded = re.search(
                r"if\(NOT ANDROID AND NOT IOS[^\n]*\)\n\s*add_subdirectory\("
                + re.escape(subdir) + r"\)",
                root_text,
            )
            self.assertIsNotNone(guarded, f"{subdir} lost its NOT IOS guard")
        desktop_only = ("tools/cli/CMakeLists.txt", "tools/cli/gpu_probe/")
        cmake_files = [
            f for f in listed
            if not f.startswith("test/")
            and not f.startswith(desktop_only)
            and not (f.startswith("tools/cli/") and f.count("/") == 2
                     and f.endswith(".cmake"))
        ]
        # Control: the scan must see the tree, not an empty checkout.
        self.assertIn("core/midi/CMakeLists.txt", cmake_files)
        root_vars = (
            "CMAKE_SOURCE_DIR", "PROJECT_SOURCE_DIR", "PULP_ROOT_DIR",
            "CMAKE_CURRENT_SOURCE_DIR", "CMAKE_CURRENT_LIST_DIR",
        )
        prefixed = re.compile(
            r"\$\{(" + "|".join(root_vars) + r")\}/((?:test|docs)/[A-Za-z0-9_./*@-]*)"
        )
        bare = re.compile(r"(?<![A-Za-z0-9_./${}-])((?:test|docs)/[A-Za-z0-9_./*@-]*)")
        references: set[str] = set()
        for rel in cmake_files:
            text = (repo / rel).read_text(encoding="utf-8", errors="replace")
            for line in text.splitlines():
                code = line.split("#", 1)[0]
                references.update(m.group(2) for m in prefixed.finditer(code))
                if rel == "CMakeLists.txt":
                    references.update(m.group(1) for m in bare.finditer(code))
        references = {ref.rstrip("./") for ref in references}
        # A bare tree root names no file: it appears in path comparisons such
        # as `if(_sub MATCHES "^${CMAKE_SOURCE_DIR}/test/")`, not as a configure
        # input, and denying it would force the gate for every test/ change.
        references -= {"test", "docs"}
        # Control: known references must be found, or the scan is blind.
        self.assertIn("test/ios/coremidi_backend_harness.mm", references)
        self.assertIn("test/harness/rt_allocation_probe.cpp", references)
        self.assertIn("docs/status/gpu-recipes.yaml", references)

        undenied = []
        for ref in sorted(references):
            leaf = ref.rsplit("/", 1)[-1]
            probe = ref if "." in leaf else f"{ref}/probe.cpp"
            probe = probe.replace("*", "probe")
            if not classify.ios_compile_required([probe]):
                undenied.append(ref)
        self.assertEqual(
            undenied, [],
            "non-test CMake names these paths; add them to "
            "IOS_COMPILE_REQUIRED_PATTERNS in classify_changes.py",
        )

    def test_mobile_global_unknown_and_mixed_surfaces_run(self) -> None:
        for paths in (
            ["apple/auv3/Sources/PulpAudioUnit.swift"],
            ["core/format/src/auv3_adapter.mm"],
            ["core/platform/platform/macos/environment_macos.mm"],
            ["core/view/include/pulp/view/view.hpp"],
            ["CMakeLists.txt"],
            [".github/workflows/build.yml"],
            ["docs/guides/local-ci.md", "apple/ios/HostApp.swift"],
            [],
        ):
            with self.subTest(paths=paths):
                self.assertTrue(classify.ios_compile_required(paths))

    def test_missing_or_malformed_policy_runs_fail_closed(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "missing.toml"
            malformed = Path(tmp) / "malformed.toml"
            malformed.write_text("not = [valid", encoding="utf-8")
            for config_path in (missing, malformed):
                with self.subTest(config_path=config_path):
                    self.assertTrue(
                        classify.ios_compile_required(
                            ["test/test_child_process.cpp"], config_path=config_path
                        )
                    )

    def test_removing_mobile_full_rule_does_not_authorize_unknown_path(self) -> None:
        import tempfile

        source = classify.CHANGED_SURFACE_CONFIG.read_text(encoding="utf-8")
        mutated = source.replace('  "apple/**",\n', "")
        self.assertNotEqual(source, mutated)
        with tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False) as fh:
            fh.write(mutated)
            config_path = Path(fh.name)
        try:
            self.assertTrue(
                classify.ios_compile_required(
                    ["apple/auv3/Sources/PulpAudioUnit.swift"],
                    config_path=config_path,
                )
            )
        finally:
            config_path.unlink()

    def test_new_bounded_family_does_not_inherit_mobile_skip_authority(self) -> None:
        import tempfile

        source = classify.CHANGED_SURFACE_CONFIG.read_text(encoding="utf-8")
        source += """

[[targets.mac.changed_surface_selection.families]]
name = "future-timeline-family"
paths = ["core/timeline/src/**"]
tests = ["future-timeline-test"]
build_targets = ["pulp-timeline"]
supported_build_types = ["debug"]
risk_class = "medium"
"""
        with tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False) as fh:
            fh.write(source)
            config_path = Path(fh.name)
        try:
            self.assertTrue(
                classify.ios_compile_required(
                    ["core/timeline/src/transport.cpp"],
                    config_path=config_path,
                )
            )
        finally:
            config_path.unlink()

    def test_missing_mobile_allowlist_runs_fail_closed(self) -> None:
        import tempfile

        source = classify.CHANGED_SURFACE_CONFIG.read_text(encoding="utf-8")
        marker = "ios_compile_skip_safe_paths = ["
        start = source.index(marker)
        end = source.index("]\n", start) + 2
        mutated = source[:start] + source[end:]
        with tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False) as fh:
            fh.write(mutated)
            config_path = Path(fh.name)
        try:
            self.assertTrue(
                classify.ios_compile_required(
                    ["test/test_child_process.cpp"], config_path=config_path
                )
            )
        finally:
            config_path.unlink()


class DiffModeTests(unittest.TestCase):
    def test_changed_files_from_diff_disables_rename_detection(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git"],
            0,
            stdout="\nREADME.md\n core/signal/src/fft.cpp \n\n",
            stderr="",
        )

        with mock.patch.object(
            classify.subprocess, "run", return_value=completed
        ) as run:
            files = classify._changed_files_from_diff("origin/main")

        self.assertEqual(files, ["README.md", "core/signal/src/fft.cpp"])
        run.assert_called_once_with(
            ["git", "diff", "--no-renames", "--name-only", "origin/main...HEAD"],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_changed_files_from_diff_returns_none_on_git_failure(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git"],
            128,
            stdout="",
            stderr="fatal: bad revision 'missing...HEAD'",
        )
        stderr = io.StringIO()

        with mock.patch.object(classify.subprocess, "run", return_value=completed):
            with contextlib.redirect_stderr(stderr):
                files = classify._changed_files_from_diff("missing")

        self.assertIsNone(files)
        self.assertIn("[classify] git diff failed (exit 128)", stderr.getvalue())

    def test_exact_tree_comparison_does_not_require_merge_history(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git"], 0, stdout="README.md\n", stderr=""
        )

        with mock.patch.object(
            classify.subprocess, "run", return_value=completed
        ) as run:
            files = classify._changed_files_from_diff(
                "a" * 40, comparison="trees"
            )

        self.assertEqual(files, ["README.md"])
        run.assert_called_once_with(
            ["git", "diff", "--no-renames", "--name-only", f"{'a' * 40}..HEAD"],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_main_diff_empty_output_is_fail_closed_json(self) -> None:
        completed = subprocess.CompletedProcess(["git"], 0, stdout="\n\n", stderr="")
        stdout = io.StringIO()
        stderr = io.StringIO()

        with mock.patch.object(classify.subprocess, "run", return_value=completed):
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = classify.main(["--mode=diff", "--json"])

        self.assertEqual(rc, 0)
        payload = json.loads(stdout.getvalue())
        self.assertTrue(payload["native_build_required"])
        self.assertEqual(payload["changed_file_count"], 0)
        self.assertIn("no changed files", payload["reason"])
        self.assertIn("native_build_required=true", stderr.getvalue())

    def test_main_diff_failure_is_json_fail_closed(self) -> None:
        completed = subprocess.CompletedProcess(
            ["git"],
            128,
            stdout="",
            stderr="fatal: bad revision 'missing...HEAD'",
        )
        stdout = io.StringIO()
        stderr = io.StringIO()

        with mock.patch.object(classify.subprocess, "run", return_value=completed):
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = classify.main(["--mode=diff", "--base", "missing", "--json"])

        self.assertEqual(rc, 0)
        payload = json.loads(stdout.getvalue())
        self.assertTrue(payload["native_build_required"])
        self.assertEqual(payload["changed_file_count"], 0)
        self.assertIn("fail-closed", payload["reason"])
        self.assertIn("native_build_required=true", stderr.getvalue())


class CliTests(unittest.TestCase):
    def _run(self, *args: str, env_extra: dict | None = None
             ) -> subprocess.CompletedProcess:
        env = os.environ.copy()
        if env_extra:
            env.update(env_extra)
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            capture_output=True, text=True, check=False, env=env,
        )

    def test_files_mode_docs_only(self) -> None:
        r = self._run("--mode=files", "--json", "README.md", "docs/x.md")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"native_build_required": false', r.stdout)

    def test_files_mode_docs_only_reports_reason_and_count(self) -> None:
        r = self._run("--mode=files", "--json", "README.md", "docs/x.md")
        payload = json.loads(r.stdout)
        self.assertEqual(payload["changed_file_count"], 2)
        self.assertFalse(payload["native_build_required"])
        self.assertTrue(payload["ios_compile_required"])
        self.assertIn("skip-safe", payload["reason"])
        self.assertIn("native_build_required=false", r.stderr)

    def test_files_mode_with_code(self) -> None:
        r = self._run("--mode=files", "--json", "core/signal/src/fft.cpp")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"native_build_required": true', r.stdout)

    def test_files_mode_reports_exact_bounded_ios_authorization(self) -> None:
        r = self._run(
            "--mode=files", "--json", "test/test_child_process.cpp"
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        payload = json.loads(r.stdout)
        self.assertTrue(payload["native_build_required"])
        self.assertFalse(payload["ios_compile_required"])

    def test_files_mode_truncates_long_native_input_reason(self) -> None:
        files = [f"core/signal/src/file_{i}.cpp" for i in range(10)]
        r = self._run("--mode=files", "--json", *files)
        self.assertEqual(r.returncode, 0, r.stderr)
        payload = json.loads(r.stdout)
        self.assertTrue(payload["native_build_required"])
        self.assertIn("file_0.cpp", payload["reason"])
        self.assertIn("(+2 more)", payload["reason"])

    def test_files_mode_empty_is_failclosed(self) -> None:
        # --mode=files with no files -> empty -> fail-closed -> true.
        r = self._run("--mode=files", "--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('"native_build_required": true', r.stdout)

    def test_writes_github_output(self) -> None:
        import tempfile
        with tempfile.NamedTemporaryFile("w+", suffix=".txt",
                                         delete=False) as fh:
            out_path = fh.name
        try:
            r = self._run("--mode=files", "README.md",
                           env_extra={"GITHUB_OUTPUT": out_path})
            self.assertEqual(r.returncode, 0, r.stderr)
            content = Path(out_path).read_text()
            self.assertIn("native_build_required=false", content)
            self.assertIn("ios_compile_required=true", content)
            self.assertIn(
                "agent_capability_installed_sdk_required=false", content
            )
        finally:
            os.unlink(out_path)

    def test_writes_github_output_appends_true_for_code(self) -> None:
        import tempfile
        with tempfile.NamedTemporaryFile("w+", suffix=".txt",
                                         delete=False) as fh:
            fh.write("existing=1\n")
            out_path = fh.name
        try:
            r = self._run("--mode=files", "core/view/src/widget.cpp",
                          env_extra={"GITHUB_OUTPUT": out_path})
            self.assertEqual(r.returncode, 0, r.stderr)
            content = Path(out_path).read_text()
            self.assertIn("existing=1\n", content)
            self.assertIn("native_build_required=true\n", content)
            self.assertIn("ios_compile_required=true\n", content)
            self.assertTrue(
                content.endswith(
                    "agent_capability_installed_sdk_required=false\n"
                )
            )
        finally:
            os.unlink(out_path)

    def test_invalid_mode_exits_usage_error(self) -> None:
        r = self._run("--mode=bogus")
        self.assertEqual(r.returncode, 2)
        self.assertIn("invalid choice", r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
