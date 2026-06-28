#!/usr/bin/env python3
"""Tests for tools/scripts/test_suite_quality_report.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).parent / "test_suite_quality_report.py"
REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location("test_suite_quality_report", SCRIPT)
assert _SPEC and _SPEC.loader
report = importlib.util.module_from_spec(_SPEC)
sys.modules["test_suite_quality_report"] = report
_SPEC.loader.exec_module(report)


def write(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class CTestSummaryTests(unittest.TestCase):
    def test_summarize_ctest_counts_labels_tiers_and_commands(self) -> None:
        doc = {
            "tests": [
                {
                    "name": "audio",
                    "command": ["/tmp/build/test/pulp-test-signal"],
                    "properties": [{"name": "LABELS", "value": ["unit", "rt-safety"]}],
                },
                {
                    "name": "visual",
                    "command": ["/tmp/build/test/pulp-test-view"],
                    "properties": [{"name": "LABELS", "value": "visual;requires-skia"}],
                },
                {"name": "legacy", "command": ["/tmp/build/test/pulp-test-state"]},
            ]
        }

        summary = report.summarize_ctest(doc)

        self.assertTrue(summary["available"])
        self.assertEqual(summary["registered"], 3)
        self.assertEqual(summary["unlabelled"], 1)
        self.assertEqual(summary["multi_tier_labelled"], 1)
        self.assertEqual(summary["tier_label_counts"]["unit"], 1)
        self.assertEqual(summary["tier_label_counts"]["rt-safety"], 1)
        self.assertEqual(summary["tier_label_counts"]["visual"], 1)
        self.assertEqual(summary["command_counts"]["pulp-test-signal"], 1)


class CliShelloutLabelGuardTests(unittest.TestCase):
    def test_tools_cli_shellout_tests_have_centralized_capability_labels(self) -> None:
        cmake = (REPO_ROOT / "tools" / "cli" / "CMakeLists.txt").read_text(encoding="utf-8")
        host_formats = re.search(r"foreach\(_pulp_host_format IN ITEMS (?P<formats>[^\)]+)\)", cmake)
        self.assertIsNotNone(host_formats)
        formats = host_formats.group("formats").split()

        add_test_names = {
            name
            for name in re.findall(r"add_test\(NAME\s+\"?(cli-[A-Za-z0-9_-]+)", cmake)
            if not name.endswith("-")
        }
        add_test_names.update(f"cli-host-missing-path-{fmt}" for fmt in formats)

        list_start = cmake.index("set(_pulp_cli_shellout_tests")
        list_end = cmake.index("foreach(_pulp_host_format", list_start)
        labelled_names = set(re.findall(r"\b(cli-[A-Za-z0-9_-]+)\b", cmake[list_start:list_end]))
        labelled_names.update(f"cli-host-missing-path-{fmt}" for fmt in formats)

        self.assertEqual(add_test_names, labelled_names)


class CapabilityLabelGuardTests(unittest.TestCase):
    def test_network_capability_suites_keep_explicit_labels(self) -> None:
        cmake = (REPO_ROOT / "test" / "CMakeLists.txt").read_text(encoding="utf-8")
        targets = [
            "pulp-test-network-stream",
            "pulp-test-websocket-channel",
            "pulp-test-osc-channel",
            "pulp-test-ipc",
            "pulp-test-ipc-endpoints",
            "pulp-test-network-service-discovery",
        ]

        for target in targets:
            blocks = re.findall(
                rf"pulp_add_test_suite\(\s*{re.escape(target)}\b(?P<body>.*?)\n\)",
                cmake,
                flags=re.DOTALL,
            )
            self.assertTrue(blocks, f"missing pulp_add_test_suite block for {target}")
            for block in blocks:
                self.assertIn("requires-network", block, f"{target} lacks requires-network")


class CTestExecutionLogTests(unittest.TestCase):
    def test_parse_last_test_log_counts_selected_and_outcomes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            log = pathlib.Path(td) / "LastTest.log"
            write(log, """\
Start testing: Jun 26 09:02 PDT
----------------------------------------------------------
1/3 Testing: passes
1/3 Test: passes
Test Passed.
2/3 Testing: fails
2/3 Test: fails
Test Failed.
3/3 Testing: skips
3/3 Test: skips
Test Skipped.
End testing: Jun 26 09:02 PDT
""")

            parsed = report.parse_last_test_log(log)

        self.assertTrue(parsed["available"])
        self.assertEqual(parsed["selected"], 3)
        self.assertEqual(parsed["executed"], 3)
        self.assertEqual(parsed["passed"], 1)
        self.assertEqual(parsed["failed"], 1)
        self.assertEqual(parsed["skipped"], 1)

    def test_summarize_ctest_execution_counts_disabled_and_failed_logs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            build = pathlib.Path(td) / "build"
            write(build / "Testing" / "Temporary" / "LastTest.log", """\
1/2 Testing: passes
1/2 Test: passes
Test Passed.
""")
            write(build / "Testing" / "Temporary" / "LastTestsDisabled.log", """\
10:disabled-a
11:disabled-b
""")
            write(build / "Testing" / "Temporary" / "LastTestsFailed.log", """\
7:failed-a
""")

            parsed = report.summarize_ctest_execution(build)

        self.assertEqual(parsed["selected"], 2)
        self.assertEqual(parsed["executed"], 1)
        self.assertEqual(parsed["disabled"], 2)
        self.assertEqual(parsed["failed_log_entries"], 1)


class WorkflowExcludeTests(unittest.TestCase):
    def test_extract_build_workflow_excludes_preserves_platform_context(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            workflow = pathlib.Path(td) / "build.yml"
            write(workflow, """\
jobs:
  build:
    steps:
      - name: Test (non-Windows)
        run: |
          exclude='STFT|VisualizationBridge'
          if [ "${RUNNER_OS}" = "macOS" ]; then
            exclude="${exclude}|Mac Only"
          fi
          if [ "${RUNNER_OS}" = "Linux" ]; then
            exclude="${exclude}|Linux Only"
          fi
          ctest --exclude-regex "$exclude"
      - name: Test (Windows)
        run: ctest --exclude-regex "Windows Only|Shared"
""")

            excludes = report.extract_build_workflow_excludes(workflow)

        self.assertEqual(
            excludes,
            [
                {"platform": "all", "pattern": "STFT"},
                {"platform": "all", "pattern": "VisualizationBridge"},
                {"platform": "macos", "pattern": "Mac Only"},
                {"platform": "linux", "pattern": "Linux Only"},
                {"platform": "windows", "pattern": "Windows Only"},
                {"platform": "windows", "pattern": "Shared"},
            ],
        )


class WeakPatternScanTests(unittest.TestCase):
    def test_scan_weak_patterns_counts_and_classifies_hits(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / ".git" / "HEAD", "ref: refs/heads/main\n")
            write(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n")
            write(root / "test" / "test_example.cpp", """\
TEST_CASE("binary missing") {
  SUCCEED("skipped: helper not built");
}
TEST_CASE("placeholder") {
  SUCCEED("scaffold placeholder");
}
TEST_CASE("weak") {
  REQUIRE(true);
}
TEST_CASE("real skip") {
  SKIP("requires display");
}
""")
            write(root / "tools" / "harness" / "fixtures" / "test_fixture.py", """\
sample = "REQUIRE(true);"
""")

            scan = report.scan_weak_patterns(root)

        self.assertEqual(scan["counts"]["succeed"], 2)
        self.assertEqual(scan["counts"]["require_true"], 2)
        self.assertEqual(scan["counts"]["skip"], 1)
        self.assertEqual(scan["classifications"]["skip-as-pass"], 1)
        self.assertEqual(scan["classifications"]["placeholder"], 1)
        self.assertEqual(scan["classifications"]["unconditional-true"], 1)
        self.assertEqual(scan["classifications"]["fixture"], 1)

    def test_weak_pattern_baseline_reports_deltas(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "weak-pattern-baseline.json", json.dumps({
                "version": 1,
                "counts": {
                    "succeed": 1,
                    "require_true": 0,
                },
                "classifications": {
                    "skip-as-pass": 1,
                    "unconditional-true": 0,
                },
            }))

            summary = report.summarize_weak_pattern_baseline(
                root,
                {
                    "counts": {
                        "succeed": 2,
                        "require_true": 1,
                    },
                    "classifications": {
                        "skip-as-pass": 1,
                        "unconditional-true": 1,
                    },
                },
            )

        self.assertTrue(summary["available"])
        self.assertEqual(summary["count_deltas"], {
            "require_true": 1,
            "succeed": 1,
        })
        self.assertEqual(summary["classification_deltas"], {
            "unconditional-true": 1,
        })

    def test_weak_pattern_exemptions_separate_fixture_hits(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "weak-pattern-exemptions.json", json.dumps({
                "version": 1,
                "entries": [
                    {
                        "path": "tools/scripts/test_test_suite_quality_report.py",
                        "kind": "require_true",
                        "classification": "unconditional-true",
                        "text_regex": "REQUIRE\\(true\\);",
                        "owner": "test-quality",
                        "reason": "fixture literal",
                        "max_count_impact": 1,
                    },
                    {
                        "path": "test/missing.cpp",
                        "kind": "succeed",
                        "classification": "skip-as-pass",
                        "text_regex": "SUCCEED",
                        "owner": "test-quality",
                        "reason": "stale fixture",
                        "max_count_impact": 1,
                    },
                ],
            }))
            weak_patterns = {
                "hits": [
                    {
                        "path": "tools/scripts/test_test_suite_quality_report.py",
                        "kind": "require_true",
                        "classification": "unconditional-true",
                        "line": 10,
                        "text": "REQUIRE(true);",
                    },
                    {
                        "path": "test/test_real.cpp",
                        "kind": "require_true",
                        "classification": "unconditional-true",
                        "line": 20,
                        "text": "REQUIRE(true);",
                    },
                ]
            }

            summary = report.summarize_weak_pattern_exemptions(root, weak_patterns)

        self.assertTrue(summary["available"])
        self.assertEqual(summary["entries"], 2)
        self.assertEqual(summary["exempted_hits"], 1)
        self.assertEqual(summary["unexempted_hits"], 1)
        self.assertEqual(summary["exempted_classifications"], {"unconditional-true": 1})
        self.assertEqual(summary["unexempted_classifications"], {"unconditional-true": 1})
        self.assertEqual(len(summary["unmatched_entries"]), 1)
        self.assertEqual(summary["over_budget_entries"], [])

    def test_weak_pattern_exemptions_report_invalid_and_over_budget_entries(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "weak-pattern-exemptions.json", json.dumps({
                "version": 1,
                "entries": [
                    {
                        "path": "test/test_fixture.cpp",
                        "kind": "require_true",
                        "classification": "unconditional-true",
                        "text_regex": "REQUIRE\\(true\\);",
                        "owner": "test-quality",
                        "reason": "fixture literal",
                        "max_count_impact": 1,
                    },
                    {
                        "path": "test/test_fixture.cpp",
                    },
                ],
            }))
            weak_patterns = {
                "hits": [
                    {
                        "path": "test/test_fixture.cpp",
                        "kind": "require_true",
                        "classification": "unconditional-true",
                        "line": 10,
                        "text": "REQUIRE(true);",
                    },
                    {
                        "path": "test/test_fixture.cpp",
                        "kind": "require_true",
                        "classification": "unconditional-true",
                        "line": 11,
                        "text": "REQUIRE(true);",
                    },
                ]
            }

            summary = report.summarize_weak_pattern_exemptions(root, weak_patterns)

        self.assertEqual(len(summary["invalid_entries"]), 1)
        self.assertEqual(len(summary["over_budget_entries"]), 1)
        self.assertEqual(summary["over_budget_entries"][0]["matched"], 2)
        self.assertEqual(summary["exempted_hits"], 1)
        self.assertEqual(summary["unexempted_hits"], 1)


class QuarantineManifestTests(unittest.TestCase):
    def test_load_and_match_quarantine_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "quarantine.json", json.dumps({
                "version": 1,
                "entries": [
                    {
                        "test_pattern": "Mac Only",
                        "platform": "macos",
                        "tier": "platform",
                        "owner": "mac-view",
                        "reason": "fixture reason",
                        "tracking_issue": "planning/example.md",
                        "expires": "2999-01-01",
                        "max_count_impact": 1,
                    }
                ],
            }))

            manifest = report.load_quarantine_manifest(root)
            summary = report.summarize_quarantine(
                [
                    {"platform": "macos", "pattern": "Mac Only"},
                    {"platform": "linux", "pattern": "Linux Only"},
                ],
                manifest,
            )

        self.assertTrue(summary["available"])
        self.assertEqual(summary["entries"], 1)
        self.assertEqual(summary["invalid_entries"], [])
        self.assertEqual(summary["expired_entries"], [])
        self.assertEqual(
            summary["workflow_excludes_without_manifest"],
            [{"platform": "linux", "pattern": "Linux Only"}],
        )

    def test_invalid_quarantine_entry_reports_missing_fields(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "quarantine.json", json.dumps({
                "version": 1,
                "entries": [{"test_pattern": "missing metadata"}],
            }))

            manifest = report.load_quarantine_manifest(root)

        self.assertTrue(manifest["available"])
        self.assertEqual(len(manifest["invalid_entries"]), 1)
        self.assertIn("owner", manifest["invalid_entries"][0]["missing"])


class CiTierManifestTests(unittest.TestCase):
    def test_summarize_ci_tiers_counts_workflows_and_missing_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / ".github" / "workflows" / "build.yml", "name: Build\n")
            write(root / ".github" / "workflows" / "templates-smoke.yml", "name: Templates\n")
            write(root / "tools" / "test-quality" / "ci-tiers.json", json.dumps({
                "version": 1,
                "required_status_contexts": [
                    {
                        "name": "macos",
                        "source": ".github/workflows/build.yml",
                        "scope": "required PR gate",
                        "test_signal": "required product test gate",
                    }
                ],
                "workflows": [
                    {
                        "path": ".github/workflows/build.yml",
                        "name": "Build and Test",
                        "cadence": "pull_request",
                        "tier": "required-pr",
                        "platforms": ["macos", "linux"],
                        "test_signal": "primary CTest lane",
                        "quality_issue": "fixture issue",
                    },
                    {
                        "path": ".github/workflows/missing.yml",
                        "name": "Missing",
                        "cadence": "schedule",
                        "tier": "nightly",
                        "platforms": ["windows"],
                        "test_signal": "missing workflow should be reported",
                        "quality_issue": "fixture issue",
                    },
                ],
            }))

            summary = report.summarize_ci_tiers(root)

        self.assertTrue(summary["available"])
        self.assertEqual(summary["workflow_count"], 2)
        self.assertEqual(summary["total_workflow_files"], 2)
        self.assertEqual(summary["tier_counts"]["required-pr"], 1)
        self.assertEqual(summary["tier_counts"]["nightly"], 1)
        self.assertEqual(summary["platform_counts"]["macos"], 1)
        self.assertEqual(summary["platform_counts"]["linux"], 1)
        self.assertEqual(summary["platform_counts"]["windows"], 1)
        self.assertEqual(summary["missing_workflow_files"], [".github/workflows/missing.yml"])
        self.assertEqual(summary["untracked_workflow_files"], [".github/workflows/templates-smoke.yml"])


class ModuleCoverageTests(unittest.TestCase):
    def test_summarize_module_coverage_matches_commands_and_names(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "module-coverage.json", json.dumps({
                "version": 1,
                "modules": [
                    {
                        "id": "audio",
                        "owner": "audio-owner",
                        "title": "Audio",
                        "guarantees": ["audio output is correct"],
                        "representative_tests": ["pulp-test-audio"],
                        "missing_or_weak": ["needs mutation proof"],
                        "command_patterns": ["pulp-test-audio"],
                    },
                    {
                        "id": "midi",
                        "owner": "midi-owner",
                        "title": "MIDI",
                        "guarantees": ["midi parsing is correct"],
                        "representative_tests": ["pulp-test-midi"],
                        "missing_or_weak": [],
                        "command_patterns": ["pulp-test-midi"],
                        "test_name_patterns": ["Sysex"],
                    },
                    {
                        "id": "cli",
                        "owner": "cli-owner",
                        "title": "CLI",
                        "guarantees": ["cli output is correct"],
                        "representative_tests": ["cli-help"],
                        "missing_or_weak": [],
                        "command_patterns": ["pulp-test-cli"],
                        "test_name_patterns": ["\\bcli[-:]"],
                    },
                ],
            }))
            doc = {
                "tests": [
                    {"name": "audio case", "command": ["/tmp/build/test/pulp-test-audio"]},
                    {"name": "Sysex parser edge", "command": ["/tmp/build/test/custom-test"]},
                    {"name": "cli-help", "command": ["/tmp/build/test/custom-test"]},
                    {"name": "click handling", "command": ["/tmp/build/test/custom-test"]},
                    {"name": "unmapped", "command": ["/tmp/build/test/other"]},
                ]
            }

            summary = report.summarize_module_coverage(root, doc)

        self.assertTrue(summary["available"])
        self.assertEqual(summary["module_count"], 3)
        self.assertEqual(summary["registered_tests"], 5)
        self.assertEqual(summary["mapped_registered_tests"], 3)
        self.assertEqual(summary["unmapped_registered_tests"], 2)
        modules = {item["id"]: item for item in summary["modules"]}
        self.assertEqual(modules["audio"]["registered_tests"], 1)
        self.assertEqual(modules["audio"]["guarantee_count"], 1)
        self.assertEqual(modules["midi"]["registered_tests"], 1)
        self.assertEqual(modules["cli"]["registered_tests"], 1)

    def test_invalid_module_coverage_entry_reports_missing_fields(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "module-coverage.json", json.dumps({
                "version": 1,
                "modules": [
                    {
                        "id": "audio",
                        "owner": "audio-owner",
                    }
                ],
            }))

            manifest = report.load_module_coverage_manifest(root)

        self.assertTrue(manifest["available"])
        self.assertEqual(len(manifest["invalid_modules"]), 1)
        self.assertIn("guarantees", manifest["invalid_modules"][0]["missing"])

    def test_module_unmapped_triage_tracks_expected_command_counts(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "module-coverage.json", json.dumps({
                "version": 1,
                "modules": [
                    {
                        "id": "audio",
                        "owner": "audio-owner",
                        "title": "Audio",
                        "guarantees": ["audio behavior is covered"],
                        "representative_tests": ["pulp-test-audio"],
                        "missing_or_weak": [],
                        "command_patterns": ["pulp-test-audio"],
                    }
                ],
            }))
            write(root / "tools" / "test-quality" / "module-coverage-unmapped-triage.json", json.dumps({
                "version": 1,
                "entries": [
                    {
                        "command": "python3.13",
                        "expected_count": 1,
                        "owner": "tooling-owner",
                        "reason": "interpreter wrapper hides the tested script",
                        "next_action": "map by script path",
                    }
                ],
            }))
            doc = {
                "tests": [
                    {"name": "audio", "command": ["/tmp/build/test/pulp-test-audio"]},
                    {"name": "script wrapper", "command": ["/usr/bin/python3.13"]},
                    {"name": "new unmapped", "command": ["/tmp/build/test/custom"]},
                ]
            }

            summary = report.summarize_module_coverage(root, doc)

        triage = summary["unmapped_triage"]
        self.assertTrue(triage["available"])
        self.assertEqual(triage["entries"], 1)
        self.assertEqual(triage["unmapped_registered_tests"], 2)
        self.assertEqual(triage["triaged_unmapped_tests"], 1)
        self.assertEqual(triage["untriaged_unmapped_tests"], 1)
        self.assertEqual(triage["untriaged_commands"], {"custom": 1})
        self.assertEqual(triage["count_mismatches"], [])
        self.assertEqual(triage["stale_entries"], [])

    def test_module_unmapped_triage_reports_invalid_stale_and_count_mismatches(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "module-coverage-unmapped-triage.json", json.dumps({
                "version": 1,
                "entries": [
                    {
                        "command": "bash",
                        "expected_count": 1,
                        "owner": "tooling-owner",
                        "reason": "shell wrapper hides the tested script",
                        "next_action": "map by script path",
                    },
                    {
                        "command": "stale",
                        "expected_count": 1,
                        "owner": "tooling-owner",
                        "reason": "old wrapper",
                        "next_action": "remove stale entry",
                    },
                    {
                        "command": "bad",
                        "expected_count": "one",
                        "owner": "tooling-owner",
                    },
                    {
                        "command": "bash",
                        "expected_count": True,
                        "owner": "tooling-owner",
                        "reason": "duplicate shell wrapper",
                        "next_action": "remove duplicate entry",
                    },
                ],
            }))
            doc = {
                "tests": [
                    {"name": "shell one", "command": ["/bin/bash"]},
                    {"name": "shell two", "command": ["/bin/bash"]},
                ]
            }

            summary = report.summarize_module_unmapped_triage(root, doc, [])

        self.assertEqual(summary["entries"], 4)
        self.assertEqual(len(summary["invalid_entries"]), 2)
        self.assertEqual(summary["invalid_entries"][0]["index"], 2)
        self.assertEqual(summary["invalid_entries"][1]["index"], 3)
        self.assertIn("duplicate_command", summary["invalid_entries"][1]["missing"])
        self.assertIn("expected_count:int>=0", summary["invalid_entries"][1]["missing"])
        self.assertEqual(summary["unmapped_registered_tests"], 2)
        self.assertEqual(summary["triaged_unmapped_tests"], 1)
        self.assertEqual(summary["untriaged_unmapped_tests"], 1)
        self.assertEqual(summary["over_expected_commands"], {"bash": 1})
        self.assertEqual(
            summary["count_mismatches"],
            [
                {
                    "command": "bash",
                    "expected_count": 1,
                    "actual_count": 2,
                    "owner": "tooling-owner",
                },
                {
                    "command": "stale",
                    "expected_count": 1,
                    "actual_count": 0,
                    "owner": "tooling-owner",
                },
            ],
        )
        self.assertEqual(summary["stale_entries"][0]["command"], "stale")


class CapabilityManifestTests(unittest.TestCase):
    def test_summarize_capabilities_matches_commands_names_and_labels(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "capabilities.json", json.dumps({
                "version": 1,
                "capabilities": [
                    {
                        "id": "requires-built-cli",
                        "title": "Built CLI",
                        "owner": "cli-owner",
                        "description": "CLI shellout tests",
                        "expected_behavior": "missing required CLI should fail",
                        "command_patterns": ["pulp-test-cli"],
                        "test_name_patterns": ["\\bcli[-:]"],
                        "label_patterns": ["tooling"],
                    },
                    {
                        "id": "requires-gpu",
                        "title": "GPU",
                        "owner": "render-owner",
                        "description": "GPU tests",
                        "expected_behavior": "missing required GPU should fail",
                        "command_patterns": ["pulp-test-gpu"],
                        "test_name_patterns": ["\\bwebgpu\\b"],
                        "label_patterns": ["visual"],
                    },
                ],
            }))
            doc = {
                "tests": [
                    {"name": "cli-help", "command": ["/tmp/build/test/custom-test"]},
                    {
                        "name": "labelled tool",
                        "command": ["/tmp/build/test/custom-test"],
                        "properties": [{"name": "LABELS", "value": "tooling"}],
                    },
                    {"name": "webgpu smoke", "command": ["/tmp/build/test/custom-test"]},
                    {"name": "click handling", "command": ["/tmp/build/test/custom-test"]},
                    {"name": "unmapped", "command": ["/tmp/build/test/other"]},
                ]
            }

            summary = report.summarize_capabilities(root, doc)

        self.assertTrue(summary["available"])
        self.assertEqual(summary["capability_count"], 2)
        self.assertEqual(summary["registered_tests"], 5)
        self.assertEqual(summary["tests_with_capability"], 3)
        self.assertEqual(summary["tests_without_capability"], 2)
        capabilities = {item["id"]: item for item in summary["capabilities"]}
        self.assertEqual(capabilities["requires-built-cli"]["registered_tests"], 2)
        self.assertEqual(capabilities["requires-gpu"]["registered_tests"], 1)

    def test_invalid_capability_entry_reports_missing_fields(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            write(root / "tools" / "test-quality" / "capabilities.json", json.dumps({
                "version": 1,
                "capabilities": [
                    {
                        "id": "requires-built-cli",
                        "owner": "cli-owner",
                    }
                ],
            }))

            manifest = report.load_capability_manifest(root)

        self.assertTrue(manifest["available"])
        self.assertEqual(len(manifest["invalid_capabilities"]), 1)
        self.assertIn("expected_behavior", manifest["invalid_capabilities"][0]["missing"])


class MainContractTests(unittest.TestCase):
    def test_main_renders_json_report_to_output_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td) / "repo"
            write(root / ".git" / "HEAD", "ref: refs/heads/main\n")
            write(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n")
            write(root / ".github" / "workflows" / "build.yml", """\
jobs:
  build:
    steps:
      - name: Test (non-Windows)
        run: |
          exclude='STFT'
          ctest --exclude-regex "$exclude"
""")
            write(root / "tools" / "test-quality" / "ci-tiers.json", json.dumps({
                "version": 1,
                "required_status_contexts": [
                    {
                        "name": "macos",
                        "source": ".github/workflows/build.yml",
                        "scope": "required PR gate",
                        "test_signal": "primary product test gate",
                    }
                ],
                "workflows": [
                    {
                        "path": ".github/workflows/build.yml",
                        "name": "Build and Test",
                        "cadence": "pull_request",
                        "tier": "required-pr",
                        "platforms": ["macos"],
                        "test_signal": "primary CTest lane",
                        "quality_issue": "fixture issue",
                    }
                ],
            }))
            write(root / "tools" / "test-quality" / "module-coverage.json", json.dumps({
                "version": 1,
                "modules": [
                    {
                        "id": "tooling",
                        "owner": "tool-owner",
                        "title": "Tooling",
                        "guarantees": ["tooling behavior is observable"],
                        "representative_tests": ["pulp-test-a"],
                        "missing_or_weak": [],
                        "command_patterns": ["pulp-test-a"],
                    }
                ],
            }))
            write(root / "tools" / "test-quality" / "module-coverage-unmapped-triage.json", json.dumps({
                "version": 1,
                "entries": [],
            }))
            write(root / "tools" / "test-quality" / "capabilities.json", json.dumps({
                "version": 1,
                "capabilities": [
                    {
                        "id": "requires-built-cli",
                        "title": "Built CLI",
                        "owner": "cli-owner",
                        "description": "CLI shellout tests",
                        "expected_behavior": "missing required CLI should fail",
                        "command_patterns": ["pulp-test-a"],
                    }
                ],
            }))
            write(root / "tools" / "test-quality" / "weak-pattern-exemptions.json", json.dumps({
                "version": 1,
                "entries": [
                    {
                        "path": "test/test_example.cpp",
                        "kind": "require_true",
                        "classification": "unconditional-true",
                        "text_regex": "REQUIRE\\(true\\);",
                        "owner": "test-quality",
                        "reason": "fixture report contract",
                        "max_count_impact": 1,
                    }
                ],
            }))
            write(root / "test" / "test_example.cpp", "REQUIRE(true);\n")
            ctest_json = pathlib.Path(td) / "ctest.json"
            ctest_json.write_text(json.dumps({"tests": [{"name": "a", "command": ["pulp-test-a"]}]}))
            output = pathlib.Path(td) / "report.json"

            rc = report.main([
                "--repo-root", str(root),
                "--ctest-json", str(ctest_json),
                "--format", "json",
                "--output", str(output),
            ])

            payload = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(rc, 0)
        self.assertEqual(payload["ctest"]["registered"], 1)
        self.assertIn("ctest_execution", payload)
        self.assertIn("weak_pattern_baseline", payload)
        self.assertEqual(payload["ci_tiers"]["workflow_count"], 1)
        self.assertEqual(payload["ci_tiers"]["total_workflow_files"], 1)
        self.assertEqual(payload["module_coverage"]["module_count"], 1)
        self.assertEqual(payload["module_coverage"]["mapped_registered_tests"], 1)
        self.assertEqual(payload["module_coverage"]["unmapped_registered_tests"], 0)
        self.assertEqual(payload["module_coverage"]["unmapped_triage"]["entries"], 0)
        self.assertEqual(
            payload["module_coverage"]["unmapped_triage"]["untriaged_unmapped_tests"],
            0,
        )
        self.assertEqual(payload["capabilities"]["capability_count"], 1)
        self.assertEqual(payload["capabilities"]["tests_with_capability"], 1)
        self.assertEqual(payload["capabilities"]["tests_without_capability"], 0)
        self.assertEqual(payload["weak_pattern_exemptions"]["entries"], 1)
        self.assertEqual(payload["weak_pattern_exemptions"]["exempted_hits"], 1)
        self.assertEqual(payload["weak_pattern_exemptions"]["unexempted_hits"], 0)
        self.assertEqual(payload["workflow_excludes"]["count"], 1)
        self.assertEqual(payload["weak_patterns"]["counts"]["require_true"], 1)

    def test_report_only_shell_summary_survives_unmapped_triage_drift(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td) / "repo"
            write(root / ".git" / "HEAD", "ref: refs/heads/main\n")
            write(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n")
            write(root / ".github" / "workflows" / "build.yml", "jobs: {}\n")
            write(root / "tools" / "test-quality" / "module-coverage.json", json.dumps({
                "version": 1,
                "modules": [],
            }))
            write(root / "tools" / "test-quality" / "module-coverage-unmapped-triage.json", json.dumps({
                "version": 1,
                "entries": [],
            }))
            ctest_json = pathlib.Path(td) / "ctest.json"
            ctest_json.write_text(json.dumps({
                "tests": [
                    {"name": "unmapped", "command": ["/tmp/build/test/custom"]},
                ],
            }))
            output = pathlib.Path(td) / "quality.md"
            summary = pathlib.Path(td) / "summary.md"
            shell = """\
set -uo pipefail
report_rc=0
"$PYTHON" "$SCRIPT" \
  --repo-root "$ROOT" \
  --ctest-json "$CTEST_JSON" \
  --format markdown \
  --fail-on-unmapped-triage-drift \
  --output "$OUTPUT" || report_rc=$?
cat "$OUTPUT" || true
{
  echo "## Test suite quality baseline"
  echo
  cat "$OUTPUT" || true
} >> "$GITHUB_STEP_SUMMARY"
exit "$report_rc"
"""

            proc = subprocess.run(
                ["bash", "-c", shell],
                check=False,
                capture_output=True,
                text=True,
                env={
                    "SCRIPT": str(SCRIPT),
                    "PYTHON": sys.executable,
                    "ROOT": str(root),
                    "CTEST_JSON": str(ctest_json),
                    "OUTPUT": str(output),
                    "GITHUB_STEP_SUMMARY": str(summary),
                },
            )

            summary_text = summary.read_text(encoding="utf-8")
            output_text = output.read_text(encoding="utf-8")

        self.assertEqual(proc.returncode, 1)
        self.assertIn("untriaged module-unmapped tests", proc.stderr)
        self.assertIn("# Test Suite Quality Baseline", output_text)
        self.assertIn("## Test suite quality baseline", summary_text)
        self.assertIn("Module Coverage Unmapped Triage", summary_text)
        self.assertIn("Untriaged commands", summary_text)

    def test_enforcement_findings_are_empty_when_report_matches_baselines(self) -> None:
        findings = report.enforcement_findings(
            {
                "quarantine": {
                    "workflow_excludes_without_manifest": [],
                    "invalid_entries": [],
                    "expired_entries": [],
                },
                "weak_pattern_baseline": {
                    "count_deltas": {"succeed": 0},
                    "classification_deltas": {"skip-as-pass": 0},
                },
                "module_coverage": {
                    "unmapped_triage": {
                        "invalid_entries": [],
                        "untriaged_unmapped_tests": 0,
                        "count_mismatches": [],
                        "stale_entries": [],
                    },
                },
            },
            fail_on_quarantine_drift=True,
            fail_on_weak_pattern_growth=True,
            fail_on_expired_quarantine=True,
            fail_on_unmapped_triage_drift=True,
        )

        self.assertEqual(findings, [])

    def test_enforcement_findings_report_requested_failures(self) -> None:
        findings = report.enforcement_findings(
            {
                "quarantine": {
                    "workflow_excludes_without_manifest": [{"pattern": "x"}],
                    "invalid_entries": [{"index": 0}],
                    "expired_entries": [{"test_pattern": "old"}],
                },
                "weak_pattern_baseline": {
                    "count_deltas": {"succeed": 2, "skip": -1},
                    "classification_deltas": {"skip-as-pass": 1},
                },
                "module_coverage": {
                    "unmapped_triage": {
                        "invalid_entries": [{"index": 0}],
                        "untriaged_unmapped_tests": 3,
                        "count_mismatches": [{"command": "bash"}],
                        "stale_entries": [{"command": "old"}],
                    },
                },
            },
            fail_on_quarantine_drift=True,
            fail_on_weak_pattern_growth=True,
            fail_on_expired_quarantine=True,
            fail_on_unmapped_triage_drift=True,
        )

        self.assertEqual(len(findings), 9)
        self.assertTrue(any("workflow exclusions missing" in item for item in findings))
        self.assertTrue(any("invalid quarantine" in item for item in findings))
        self.assertTrue(any("expired quarantine" in item for item in findings))
        self.assertTrue(any("weak-test count growth" in item for item in findings))
        self.assertTrue(any("weak-test classification growth" in item for item in findings))
        self.assertTrue(any("invalid module-unmapped triage" in item for item in findings))
        self.assertTrue(any("untriaged module-unmapped tests" in item for item in findings))
        self.assertTrue(any("module-unmapped triage count mismatches" in item for item in findings))
        self.assertTrue(any("stale module-unmapped triage entries" in item for item in findings))


if __name__ == "__main__":
    unittest.main(verbosity=2)
