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
import tempfile
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
            "tools/agent-capabilities/history/" + "0" * 64 + ".json",
            "tools/agent-capabilities/fingerprints/pulp/midi/strum.hpp.json",
            "docs/status/agent-capability-surface/headers/pulp/midi/strum.hpp.json",
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
            check=True, encoding="utf-8"
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
            ["test/test_widgets.cpp", "apple/Sources/PulpSwift/PulpBridge.swift"],
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
            cwd=repo, capture_output=True, text=True, check=True, encoding="utf-8"
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
        # pulp-native is macOS-only: integrations/CMakeLists.txt refuses it on
        # iOS, and the root adds its example plug-in only when the pulp-native
        # target exists. Its CMake, and that of packages built on it, never
        # runs in the iOS gate either.
        integrations_text = (repo / "integrations/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(
            integrations_text,
            r"if\(NOT APPLE OR IOS OR PULP_IOS\)\s*\n\s*message\(FATAL_ERROR\s*\n"
            r"\s*\"PULP_ENABLE_PULP_NATIVE=ON",
            "pulp-native lost its iOS refusal",
        )
        self.assertRegex(
            root_text,
            r"if\(TARGET pulp-native[^\n]*\)\n\s*add_subdirectory\("
            r"integrations/pulp-native/examples/metal-probe\)",
            "the metal-probe example is no longer gated on the pulp-native target",
        )
        # rmlui-native sits on pulp-native and refuses to build without it.
        self.assertRegex(
            integrations_text,
            r"if\(NOT PULP_ENABLE_PULP_NATIVE\)\s*\n\s*message\(FATAL_ERROR "
            r"\"PULP_ENABLE_RMLUI_ADAPTER=ON needs PULP_ENABLE_PULP_NATIVE=ON\"\)",
            "rmlui-native no longer requires pulp-native",
        )
        desktop_only = (
            "tools/cli/CMakeLists.txt", "tools/cli/gpu_probe/", "integrations/pulp-native/",
            "integrations/rmlui-native/",
        )
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
            ["CMakeLists.txt"],
            [".github/workflows/build.yml"],
            ["docs/guides/local-ci.md", "apple/ios/HostApp.swift"],
            [],
        ):
            with self.subTest(paths=paths):
                self.assertTrue(classify.ios_compile_required(paths))

    def test_the_ios_only_surface_list_is_pinned(self) -> None:
        """Every surface the macOS build never compiles, and nothing shared.

        Widening this list moves shared code back onto the per-PR iOS gate;
        narrowing it lets an iOS-only break land and wait for the nightly.
        Either is a reviewed decision, so the list is pinned exactly.
        """
        self.assertEqual(classify.IOS_SURFACE_PATTERNS, (
            "apple/**", "examples/ios-*", "examples/ios-*/**", "templates/ios-*/**",
            "tools/templates/auv3/**", "*/platform/ios/**", "*/ios/*", "*_ios.*",
            "*_ios_*", "*/ios_*", "CMakeLists.txt", "cmake/**", "tools/cmake/**",
            "setup.sh", "tools/deps/**", ".gitmodules",
            "tools/scripts/fetch_skia_for_release.py", ".github/workflows/build.yml",
            ".github/workflows/ios-*.yml", "tools/ci/ios_gate_digest.py",
            "tools/scripts/classify_changes.py", "tools/scripts/test_ios_compile_gate_legs.py",
            ".shipyard/config.toml",
        ))
        self.assertEqual(classify.IOS_SOURCE_MARKERS, (
            "TARGET_OS_IPHONE", "TARGET_OS_IOS", "TARGET_OS_SIMULATOR", "PULP_IOS", "UIKit"))

    def test_shared_core_no_longer_runs_the_per_pr_gate(self) -> None:
        """The macOS gate compiles these for macOS, so a break fails it there."""
        # Control: each file exists and carries no iOS branch, so the verdict
        # comes from the rule and not from a missing-file fail-closed.
        repo = THIS_DIR.parent.parent
        for paths in (
            # The AUv3 adapter: the one real break the per-PR gate reported in
            # 14 days, which the macOS build compiles into two test targets.
            ["core/format/src/au_adapter.mm"],
            ["core/midi/src/midi_file.cpp"],
            ["core/view/include/pulp/view/view.hpp"],
            ["core/platform/platform/mac/environment_mac.mm"],
            ["core/signal/include/pulp/signal/biquad.hpp", "README.md"],
            ["tools/scripts/test_release_build_matrix.py"],
        ):
            with self.subTest(paths=paths):
                for path in paths:
                    self.assertTrue((repo / path).is_file(), path)
                self.assertFalse(classify.ios_compile_required(paths))

    def test_ios_only_surfaces_run_the_per_pr_gate(self) -> None:
        for paths in (
            ["apple/Sources/PulpSwift/PulpBridge.swift"],
            ["core/view/platform/ios/window_host_ios.mm"],
            ["examples/ios-auv3-synth/src/sine_synth.cpp"],
            ["templates/ios-auv3/HostApp/ContentView.swift"],
            ["tools/cmake/PulpAuv3.cmake"],
            ["core/midi/CMakeLists.txt"],
            ["tools/deps/manifest.json"],
            ["tools/scripts/fetch_skia_for_release.py"],
            [".github/workflows/build.yml"],
            ["core/signal/src/fft_backend.cpp", "apple/Package.swift"],
        ):
            with self.subTest(paths=paths):
                # Real files, so a pass is the rule's, not the fail-closed
                # verdict for a path that cannot be read.
                for path in paths:
                    self.assertTrue((THIS_DIR.parent.parent / path).is_file(), path)
                self.assertTrue(classify.ios_compile_required(paths))

    def test_a_shared_source_with_an_ios_branch_runs_the_gate(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "core/x/src").mkdir(parents=True)
            (root / "core/x/src/branchy.mm").write_text(
                "#if TARGET_OS_IPHONE\nint x;\n#endif\n", encoding="utf-8")
            (root / "core/x/src/plain.mm").write_text("int y;\n", encoding="utf-8")
            self.assertTrue(classify.ios_compile_required(
                ["core/x/src/branchy.mm"], root=root))
            self.assertFalse(classify.ios_compile_required(
                ["core/x/src/plain.mm"], root=root))
            # A deleted source: the macOS build cannot vouch for it.
            self.assertTrue(classify.ios_compile_required(
                ["core/x/src/deleted.cpp"], root=root))
            # A markdown file is never read for markers.
            self.assertFalse(classify.ios_compile_required(
                ["core/x/README.md"], root=root))

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
            capture_output=True, text=True, check=False, env=env, encoding="utf-8"
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
        self.assertFalse(payload["ios_compile_required"])
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
            content = Path(out_path).read_text(encoding="utf-8")
            self.assertIn("native_build_required=false", content)
            self.assertIn("ios_compile_required=false", content)
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
            content = Path(out_path).read_text(encoding="utf-8")
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


def _github_glob(pattern: str) -> re.Pattern[str]:
    """GitHub's filter glob: ``**`` crosses ``/``, ``*`` and ``?`` do not."""
    out, i = "", 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out, i = out + ".*", i + 2
        elif pattern[i] == "*":
            out, i = out + "[^/]*", i + 1
        elif pattern[i] == "?":
            out, i = out + "[^/]", i + 1
        else:
            out, i = out + re.escape(pattern[i]), i + 1
    return re.compile(out + r"\Z")


def _workflow_event_block(name: str, event: str) -> list[str]:
    """Return the scalar lines in one top-level ``on`` event.

    The gate Python deliberately has no YAML dependency. These tests only
    need the small, indentation-based event fields below, so reading that
    shape directly keeps the ctest lane runnable with its standard library.
    """
    lines = (THIS_DIR.parents[1] / ".github" / "workflows" / name).read_text(
        encoding="utf-8"
    ).splitlines()
    in_on = False
    in_event = False
    event_indent = 0
    out: list[str] = []
    for raw in lines:
        without_comment = raw.split("#", 1)[0].rstrip()
        if not without_comment.strip():
            continue
        indent = len(without_comment) - len(without_comment.lstrip())
        text = without_comment.strip()
        if not in_on:
            if indent == 0 and text == "on:":
                in_on = True
            continue
        if indent == 0:
            break
        if indent == 2 and text == f"{event}:":
            in_event = True
            event_indent = indent
            out = []
            continue
        if in_event and indent <= event_indent:
            break
        if in_event:
            out.append(text)
    return out


def _workflow_paths_ignore(name: str) -> list[str]:
    lines = _workflow_event_block(name, "pull_request")
    try:
        start = lines.index("paths-ignore:") + 1
    except ValueError:
        return []
    values = []
    for line in lines[start:]:
        if not line.startswith("-"):
            break
        value = line[1:].strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values.append(value)
    return values


def _skipped_by(patterns: list[str], files: list[str]) -> bool:
    """GitHub skips a paths-ignore workflow when every file matches a pattern."""
    compiled = [_github_glob(p) for p in patterns]
    return bool(files) and all(any(c.match(f) for c in compiled) for f in files)


# Path sets from real pull requests and the edges of each class.
IMPACT_TABLE = (
    (["docs/reports/a9-edit-replay-evidence-20261112.md"], "docs-only"),
    (["docs/evidence/g12/README.md", "docs/evidence/g12/manifest.json"], "docs-only"),
    (["README.md", "VISION.md"], "docs-only"),
    (["CLAUDE.md", ".agents/skills/code-comments/SKILL.md"], "docs-only"),
    (["planning"], "docs-only"),
    (["docs/migrations/2026-11-01-x.md"], "code:docs"),
    (["docs/status/consumption-profiles.json"], "code:docs"),
    (["docs/status/agent-capabilities.json"], "code:docs"),
    ([".github/workflows/build.yml"], "ci-only"),
    ([".github/workflows/build.yml", "docs/guides/local-ci.md"], "ci-only"),
    (["core/view/src/widgets.cpp"], "code:core"),
    (["core/view/src/widgets.cpp", "docs/guides/x.md"], "code:core"),
    (["tools/scripts/classify_changes.py", ".github/workflows/x.yml"], "code:.github,tools"),
    (["CMakeLists.txt"], "code:root"),
    ([], "code:unknown"),
    (None, "code:unknown"),
)


class ImpactClassTests(unittest.TestCase):
    def test_table(self) -> None:
        for files, expected in IMPACT_TABLE:
            self.assertEqual(classify.impact_class(files), expected, files)

    def test_docs_only_never_skips_a_required_native_build_input(self) -> None:
        # docs-only is a strict subset of "native build not required".
        for files, expected in IMPACT_TABLE:
            if expected == "docs-only":
                self.assertFalse(classify.native_build_required(files), files)
                self.assertFalse(
                    classify.agent_capability_installed_sdk_required(files), files)

    def test_planning_gitlink_is_skip_safe(self) -> None:
        self.assertTrue(classify.is_skip_safe("planning"))
        self.assertFalse(classify.is_skip_safe("planning-notes.txt"))

    def test_impact_class_is_written_to_github_output_and_json(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()), \
                mock.patch.dict(os.environ, {"GITHUB_OUTPUT": ""}):
            classify.main(["--mode=files", "--json", "docs/a.md"])
        self.assertEqual(json.loads(out.getvalue())["impact_class"], "docs-only")


# A merge group's own entry delta (`base_sha..head_sha --no-renames`), as the
# docs entry-delta step in build.yml lists it. Only `docs-only` skips there.
ENTRY_DELTA_TABLE = (
    (["docs/guides/local-ci.md"], "docs-only"),
    ([".agents/skills/x/SKILL.md"], "docs-only"),
    (["planning"], "docs-only"),
    # Mixed: one build input among docs builds.
    (["docs/guides/local-ci.md", "core/view/src/example.cpp"], "code:core"),
    # A rename is two paths; one leaving a build input builds.
    (["core/view/src/old.cpp", "docs/guides/old.md"], "code:core"),
    (["docs/guides/a.md", "docs/guides/b.md"], "docs-only"),
    (["docs/migrations/x.md"], "code:docs"),
    (["docs/status/consumption-profiles.json"], "code:docs"),
    (["docs/status/agent-capabilities.json"], "code:docs"),
    ([], "code:unknown"),
)


class EntryDeltaClassTests(unittest.TestCase):
    """The docs entry-delta step runs the protected base's copy of this file
    from a temporary path, `python3 -I <copy> --mode=files --json -- <files>`,
    and reads `impact_class`. It must work from outside the checkout."""

    def run_copy(self, files: list[str]) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "classifier.py"
            copy.write_text(Path(classify.__file__).read_text(encoding="utf-8"), encoding="utf-8")
            env = {k: v for k, v in os.environ.items() if k != "GITHUB_OUTPUT"}
            proc = subprocess.run([sys.executable, "-I", str(copy), "--mode=files", "--json", "--", *files],
                                  capture_output=True, text=True, encoding="utf-8", check=False,
                                  cwd=tmp, env=env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return json.loads(proc.stdout)["impact_class"]

    def test_table(self) -> None:
        for files, expected in ENTRY_DELTA_TABLE:
            self.assertEqual(classify.impact_class(files), expected, files)

    def test_a_copy_outside_the_checkout_classifies_the_same(self) -> None:
        for files, expected in ENTRY_DELTA_TABLE:
            self.assertEqual(self.run_copy(files), expected, files)

    def test_docs_only_entries_never_name_a_native_build_input(self) -> None:
        for files, expected in ENTRY_DELTA_TABLE:
            if expected == "docs-only":
                self.assertFalse(classify.native_build_required(files), files)


class CodeqlDocsSkipTests(unittest.TestCase):
    """CodeQL's paths-ignore is the classifier's, and skips only docs."""

    def test_workflow_uses_the_classifier_list(self) -> None:
        self.assertEqual(_workflow_paths_ignore("codeql-advanced.yml"),
                         list(classify.DOCS_PATHS_IGNORE))

    def test_every_docs_only_diff_is_skipped(self) -> None:
        for files, expected in IMPACT_TABLE:
            if expected == "docs-only":
                self.assertTrue(_skipped_by(list(classify.DOCS_PATHS_IGNORE), files), files)

    def test_any_code_file_runs_it(self) -> None:
        # Positive control: every diff touching a non-docs file still runs CodeQL.
        for files, expected in IMPACT_TABLE:
            if files and any(not classify.is_docs_only_path(f) and not f.endswith(".md")
                             and not f.startswith("docs/") for f in files):
                self.assertFalse(_skipped_by(list(classify.DOCS_PATHS_IGNORE), files), files)

    def test_glob_agrees_with_the_classifier_on_every_tracked_path(self) -> None:
        proc = subprocess.run(["git", "ls-files"], cwd=THIS_DIR.parents[1],
                              capture_output=True, text=True, encoding="utf-8",
                              check=True)
        tracked = proc.stdout.split()
        self.assertGreater(len(tracked), 1000)  # control: the instrument read the tree
        patterns = list(classify.DOCS_PATHS_IGNORE)
        for path in tracked + ["planning"]:
            ignored = _skipped_by(patterns, [path])
            if classify.impact_class([path]) == "docs-only":
                self.assertTrue(ignored, path)
            if ignored:
                # Wider than docs-only only for build-input documents, never code.
                self.assertTrue(path.endswith(".md") or path.startswith("docs/")
                                or path == "planning", path)

    def test_codeql_still_runs_on_main_and_schedule(self) -> None:
        push = _workflow_event_block("codeql-advanced.yml", "push")
        self.assertNotIn("paths-ignore:", push)
        self.assertNotIn("paths:", push)
        self.assertTrue(_workflow_event_block("codeql-advanced.yml", "schedule"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
