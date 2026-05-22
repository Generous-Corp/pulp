#!/usr/bin/env python3
"""Self-tests for the source-contract registry checker."""

from __future__ import annotations

import copy
import datetime as dt
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


CHECKER_PATH = Path(__file__).with_name("check-source-contracts.py")
SPEC = importlib.util.spec_from_file_location("check_source_contracts", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
checker = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = checker
SPEC.loader.exec_module(checker)

ROOT = checker.REPO_ROOT
REGISTRY_PATH = checker.DEFAULT_REGISTRY
TODAY = dt.date(2026, 5, 14)


def _load_registry() -> dict:
    return json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))


def _write_registry(tmp: Path, registry: dict) -> Path:
    path = tmp / "source-contracts.json"
    path.write_text(json.dumps(registry, indent=2) + "\n", encoding="utf-8")
    return path


def _contract(registry: dict, source: str) -> dict:
    for entry in registry["contracts"]:
        if entry["source"] == source:
            return entry
    raise AssertionError(f"missing source: {source}")


def _findings(registry: dict, tmp: Path) -> list:
    path = _write_registry(tmp, registry)
    return checker.compute_findings(root=ROOT, registry_path=path, today=TODAY)


def _codes(findings: list) -> set[str]:
    return {finding.code for finding in findings}


def _sources_for(findings: list, code: str) -> set[str]:
    return {finding.source for finding in findings if finding.code == code}


class SourceContractsTest(unittest.TestCase):
    def mutate(self, source: str) -> tuple[dict, dict]:
        registry = copy.deepcopy(_load_registry())
        return registry, _contract(registry, source)

    def assert_has_code(self, registry: dict, code: str) -> None:
        with tempfile.TemporaryDirectory() as td:
            findings = _findings(registry, Path(td))
        self.assertIn(code, _codes(findings), [str(f) for f in findings])

    def test_real_registry_has_no_warning_or_error_findings(self) -> None:
        findings = checker.compute_findings(root=ROOT, registry_path=REGISTRY_PATH, today=TODAY)
        warning_pairs = {
            (finding.source, finding.code)
            for finding in findings
            if finding.severity in {"warning", "error"}
        }
        self.assertEqual(warning_pairs, set(), [str(f) for f in findings])

    def test_schema_version_is_required(self) -> None:
        registry = _load_registry()
        registry.pop("registry-schema-version")
        self.assert_has_code(registry, "missing-schema-version")

    def test_kind_and_dispatch_enums_are_checked(self) -> None:
        registry, entry = self.mutate("figma")
        entry["kind"] = "unknown-kind"
        entry["dispatch"]["kind"] = "unknown-dispatch"
        with tempfile.TemporaryDirectory() as td:
            codes = _codes(_findings(registry, Path(td)))
        self.assertIn("invalid-kind", codes)
        self.assertIn("invalid-dispatch-kind", codes)

    def test_required_fixture_paths_must_exist(self) -> None:
        registry, entry = self.mutate("figma")
        entry["validation"]["fixtures"].append("test/fixtures/figma/does-not-exist.tsx")
        self.assert_has_code(registry, "missing-fixture")

    def test_design_source_label_must_resolve(self) -> None:
        registry, entry = self.mutate("figma")
        entry["dispatch"]["label"] = "figma-missing"
        self.assert_has_code(registry, "missing-design-source-label")

    def test_explicit_runtime_parser_must_resolve(self) -> None:
        registry, entry = self.mutate("rn")
        entry["dispatch"]["parser"] = "parse_missing_react_native_export"
        self.assert_has_code(registry, "missing-explicit-runtime-parser")

    def test_parser_runtime_and_static_symbols_must_resolve(self) -> None:
        registry, entry = self.mutate("v0")
        entry["parser"]["runtime"] = "parse_missing_v0_runtime"
        entry["parser"]["static"] = "parse_missing_v0_static"
        with tempfile.TemporaryDirectory() as td:
            findings = _findings(registry, Path(td))
        missing = [f for f in findings if f.code == "missing-parser-symbol"]
        self.assertGreaterEqual(len(missing), 2)

    def test_runtime_file_resolves_runtime_parser_separately(self) -> None:
        # The P6-A3 refactor moved the runtime parsers into claude_bundle.cpp;
        # parser.runtime_file points runtime symbols there while parser.static
        # stays in parser.file. Pointing runtime_file back at the old
        # design_import.cpp must still flag the moved symbol as missing.
        registry, entry = self.mutate("pencil")
        entry["parser"]["runtime_file"] = "core/view/src/design_import.cpp"
        with tempfile.TemporaryDirectory() as td:
            findings = _findings(registry, Path(td))
        missing = [f for f in findings if f.code == "missing-parser-symbol"]
        self.assertTrue(
            any(
                f.path == "core/view/src/design_import.cpp"
                and "parse_pencil_react" in f.message
                for f in missing
            ),
            [str(f) for f in findings],
        )

    def test_present_reference_screenshot_must_exist(self) -> None:
        registry, entry = self.mutate("stitch")
        entry["validation"]["pulp_render_reference"] = {
            "status": "present",
            "path": "test/fixtures/stitch/missing-reference.png",
        }
        self.assert_has_code(registry, "missing-reference-screenshot")

    def test_test_tag_fragments_must_exist(self) -> None:
        registry, entry = self.mutate("v0")
        entry["validation"]["test_tags"] = ["[definitely-not-a-real-tag]"]
        self.assert_has_code(registry, "missing-test-tag")

    def test_stale_source_contract_warns(self) -> None:
        registry, entry = self.mutate("stitch")
        entry["last_verified"] = "2020-01-01"
        entry["recheck_interval_days"] = 1
        self.assert_has_code(registry, "stale-source-contract")

    def test_unverified_source_contract_warns(self) -> None:
        registry, entry = self.mutate("figma")
        entry["last_verified"] = "unverified"
        entry.pop("recheck_interval_days", None)
        self.assert_has_code(registry, "unverified-source")

    def test_compat_source_and_format_foreign_keys_are_checked(self) -> None:
        registry, entry = self.mutate("stitch")
        entry["compat"]["source"] = "missing-source"
        self.assert_has_code(registry, "missing-compat-source")

        registry, entry = self.mutate("stitch")
        entry["format_versions"][0]["version"] = "2099.01"
        self.assert_has_code(registry, "missing-compat-format")

    def test_runtime_surface_refs_have_known_owners(self) -> None:
        registry, entry = self.mutate("figma")
        entry["runtime_requirements"]["surface_refs"].append("not-a-known-owner")
        self.assert_has_code(registry, "invalid-surface-ref")

    def test_roundtrip_literal_fixtures_are_cross_checked(self) -> None:
        registry, entry = self.mutate("v0")
        entry["validation"]["fixtures"] = ["test/fixtures/v0-dev/audio-control-panel.tsx"]
        self.assert_has_code(registry, "roundtrip-fixture-drift")

    def test_roundtrip_literal_references_are_cross_checked(self) -> None:
        registry, entry = self.mutate("rn")
        entry["validation"]["pulp_render_reference"]["path"] = "planning/screenshots/wrong.png"
        self.assert_has_code(registry, "roundtrip-reference-drift")

    def test_rn_not_applicable_compat_is_exempt(self) -> None:
        findings = checker.compute_findings(root=ROOT, registry_path=REGISTRY_PATH, today=TODAY)
        rn_codes = {finding.code for finding in findings if finding.source == "rn"}
        self.assertNotIn("missing-compat-source", rn_codes)
        self.assertNotIn("missing-compat-format", rn_codes)

    def test_spectr_roundtrip_script_is_exempt(self) -> None:
        findings = checker.compute_findings(root=ROOT, registry_path=REGISTRY_PATH, today=TODAY)
        self.assertNotIn("spectr", _sources_for(findings, "unreferenced-roundtrip-script"))

    def test_optional_planning_paths_skip_when_submodule_absent(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            state, reason = checker._path_state(Path(td), "planning/screenshots/reference.png")
        self.assertEqual(state, "optional-missing")
        self.assertIn("planning submodule", reason)

    def test_shell_path_normalization_handles_quotes_defaults_and_unknowns(self) -> None:
        self.assertEqual(checker._strip_shell_value("'quoted'"), "quoted")
        self.assertEqual(checker._strip_shell_value('"quoted"'), "quoted")
        self.assertEqual(checker._strip_shell_value("${VALUE:-fallback}"), "fallback")
        self.assertEqual(
            checker.normalize_shell_repo_path('"${PULP_DIR}/test/fixtures/a.tsx"'),
            "test/fixtures/a.tsx",
        )
        self.assertEqual(
            checker.normalize_shell_repo_path("'$PULP/tools/import-validation/a.sh'"),
            "tools/import-validation/a.sh",
        )
        self.assertEqual(
            checker.normalize_shell_repo_path("${REFERENCE:-/tmp/repo/planning/screenshots/a.png}"),
            "planning/screenshots/a.png",
        )
        self.assertEqual(
            checker.normalize_shell_repo_path("/var/tmp/work/test/fixtures/b.tsx"),
            "test/fixtures/b.tsx",
        )
        self.assertIsNone(checker.normalize_shell_repo_path("/var/tmp/unowned/file.txt"))

    def test_literal_roundtrip_assignments_filters_relevant_variables(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            script = Path(td) / "roundtrip.sh"
            script.write_text(
                "\n".join(
                    [
                        'REFERENCE="${PULP_DIR}/planning/screenshots/ref.png"',
                        "MAIN_FIXTURE=$PULP/test/fixtures/source.tsx",
                        "IGNORED_REFERENCE_SUFFIX=$PULP/test/fixtures/ignored.tsx",
                        "OTHER=$PULP/test/fixtures/other.tsx",
                    ]
                ),
                encoding="utf-8",
            )
            assignments = checker._literal_roundtrip_assignments(script)
        self.assertEqual(len(assignments), 2)
        self.assertNotIn(("OTHER", "test/fixtures/other.tsx"), assignments)
        self.assertEqual(
            assignments,
            [
                ("REFERENCE", "planning/screenshots/ref.png"),
                ("MAIN_FIXTURE", "test/fixtures/source.tsx"),
            ],
        )

    def test_entry_path_collectors_ignore_malformed_shapes(self) -> None:
        entry = {
            "validation": {
                "fixtures": ["test/fixtures/a.tsx", 42],
                "pulp_render_reference": {"path": "planning/screenshots/a.png"},
            },
            "format_versions": [
                {"fixture_paths": ["test/fixtures/b.tsx", None]},
                {"fixture_paths": "not-a-list"},
                "bad-version",
            ],
        }
        self.assertEqual(
            checker._entry_fixture_paths(entry),
            {"test/fixtures/a.tsx", "test/fixtures/b.tsx"},
        )
        self.assertEqual(
            checker._entry_reference_paths(entry),
            {"planning/screenshots/a.png"},
        )
        self.assertEqual(checker._entry_reference_paths({"validation": {}}), set())

    def test_shape_checker_reports_invalid_modes_and_missing_fields(self) -> None:
        findings: list = []
        checker._check_entry_shape(
            findings,
            {
                "source": 123,
                "kind": "bad-kind",
                "trust": "bad-trust",
                "compat": {"mode": "bad-mode"},
                "dispatch": {"kind": "bad-dispatch"},
                "validation": {"pulp_render_reference": {"status": "bad-status"}},
            },
        )
        codes = _codes(findings)
        self.assertEqual(len(findings), 13)
        self.assertIn("missing-required-field", codes)
        self.assertIn("invalid-kind", codes)
        self.assertIn("invalid-trust", codes)
        self.assertIn("invalid-compat-mode", codes)
        self.assertIn("invalid-dispatch-kind", codes)
        self.assertIn("invalid-reference-status", codes)
        self.assertEqual({finding.source for finding in findings}, {"<unknown>"})

    def test_existing_path_checker_reports_missing_empty_and_optional_info(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            present = root / "test/fixtures/present.tsx"
            present.parent.mkdir(parents=True)
            present.write_text("fixture", encoding="utf-8")
            findings: list = []
            checker._check_existing_path(findings, root, "src", "test/fixtures/present.tsx", "missing", "fixture")
            checker._check_existing_path(findings, root, "src", "", "missing", "fixture")
            checker._check_existing_path(findings, root, "src", "test/fixtures/missing.tsx", "missing", "fixture")
            checker._check_existing_path(findings, root, "src", "planning/screenshots/a.png", "missing", "fixture")
        self.assertEqual([finding.code for finding in findings], ["missing", "missing", "optional-planning-missing"])
        self.assertEqual(findings[-1].severity, "info")
        self.assertIn("planning submodule", findings[-1].message)

    def test_test_tag_checker_reports_empty_missing_files_and_missing_fragments(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            test_file = root / "tests/source.test"
            test_file.parent.mkdir(parents=True)
            test_file.write_text("[known] body", encoding="utf-8")
            findings: list = []
            checker._check_test_tags(findings, root, {"source": "empty", "validation": {}})
            checker._check_test_tags(
                findings,
                root,
                {
                    "source": "tagged",
                    "validation": {
                        "test_files": ["tests/source.test", "tests/missing.test"],
                        "test_tags": ["[known][missing]", 12],
                    },
                },
            )
        codes = [finding.code for finding in findings]
        self.assertEqual(codes, ["missing-test-files", "missing-test-file", "missing-test-tag"])
        self.assertEqual([finding.source for finding in findings], ["empty", "tagged", "tagged"])
        self.assertEqual(findings[1].path, "tests/missing.test")
        self.assertIn("missing", findings[2].message)

    def test_cadence_checker_covers_manual_future_invalid_and_overdue(self) -> None:
        today = dt.date(2026, 5, 22)
        findings: list = []
        checker._check_cadence(findings, {"source": "manual", "last_verified": "2020-01-01", "recheck_interval": "manual"}, today)
        checker._check_cadence(findings, {"source": "future", "last_verified": "2026-05-20", "recheck_interval_days": 10}, today)
        checker._check_cadence(findings, {"source": "missing-date", "recheck_interval_days": 10}, today)
        checker._check_cadence(findings, {"source": "bad-date", "last_verified": "nope", "recheck_interval_days": 10}, today)
        checker._check_cadence(findings, {"source": "old", "last_verified": "2026-01-01", "recheck_interval_days": 1}, today)
        self.assertEqual(
            [(finding.source, finding.code) for finding in findings],
            [
                ("missing-date", "invalid-last-verified"),
                ("bad-date", "invalid-last-verified"),
                ("old", "stale-source-contract"),
            ],
        )

    def test_report_rendering_escapes_markdown_and_handles_empty(self) -> None:
        finding = checker.Finding(
            "warning",
            "pipe-code",
            "source",
            "message with | pipe",
            "path/file.tsx",
        )
        self.assertEqual(checker.render_report([], fmt="text"), "source-contracts: no findings\n")
        self.assertEqual(
            checker.render_report([], fmt="markdown"),
            "Source-contract check: no findings.\n",
        )
        text = checker.render_report([finding], fmt="text")
        markdown = checker.render_report([finding], fmt="markdown")
        self.assertTrue(markdown.startswith("| Severity | Code | Source | Path | Message |"))
        self.assertNotIn("message with | pipe", markdown)
        self.assertIn("[warning] pipe-code source", text)
        self.assertIn("(path/file.tsx)", text)
        self.assertIn("message with \\| pipe", markdown)
        self.assertIn("`pipe-code`", markdown)

    def test_strict_flips_exit_code_only_on_warnings(self) -> None:
        registry = _load_registry()
        _contract(registry, "figma")["last_verified"] = "unverified"
        with tempfile.TemporaryDirectory() as td:
            registry_path = _write_registry(Path(td), registry)
            base_cmd = [
                sys.executable,
                str(CHECKER_PATH),
                "--root",
                str(ROOT),
                "--registry",
                str(registry_path),
            ]
            non_strict = subprocess.run(base_cmd, check=False, capture_output=True, text=True)
            strict = subprocess.run(base_cmd + ["--strict"], check=False, capture_output=True, text=True)
        self.assertEqual(non_strict.returncode, 0, non_strict.stdout + non_strict.stderr)
        self.assertEqual(strict.returncode, 1, strict.stdout + strict.stderr)

    def test_strict_real_registry_has_clean_exit(self) -> None:
        cmd = [
            sys.executable,
            str(CHECKER_PATH),
            "--root",
            str(ROOT),
            "--registry",
            str(REGISTRY_PATH),
            "--strict",
        ]
        result = subprocess.run(cmd, check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
