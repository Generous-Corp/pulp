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

    def test_parser_role_specific_files_must_resolve(self) -> None:
        registry, entry = self.mutate("rn")
        entry["parser"]["files"] = {"runtime": "core/view/src/does-not-exist.cpp"}
        with tempfile.TemporaryDirectory() as td:
            findings = _findings(registry, Path(td))
        self.assertIn("missing-explicit-runtime-parser", _codes(findings))
        missing = [f for f in findings if f.code == "missing-parser-symbol"]
        self.assertTrue(
            any("does-not-exist.cpp" in (finding.path or "") for finding in missing),
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
