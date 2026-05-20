#!/usr/bin/env python3
"""Warn-only checks for Pulp import source contracts.

The registry is intentionally additive in PR1. This checker reports drift, but
does not fail unless --strict is passed by a focused self-test or a human.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as _dt
import json
import re
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REGISTRY = REPO_ROOT / "tools/import-validation/source-contracts.json"
ROUNDTRIP_EXEMPT = {"spectr-roundtrip.sh"}
KINDS = {"screen-export", "system-tokens", "static-only"}
TRUST_LEVELS = {"official", "observed", "absent"}
DISPATCH_KINDS = {"design-source-label", "explicit-runtime-parser", "static-only"}
COMPAT_MODES = {"foreign-key", "not-applicable"}
REFERENCE_STATUSES = {"present", "pending", "not-applicable"}
KNOWN_SURFACE_REFS = (
    "compat.json:",
    "packages/",
    "core/view",
)
DEFAULT_PARSER_FILE = "core/view/src/design_import.cpp"


@dataclasses.dataclass(frozen=True)
class Finding:
    severity: str
    code: str
    source: str
    message: str
    path: str | None = None


def load_registry(path: Path = DEFAULT_REGISTRY) -> dict[str, Any]:
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


def _load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def _source(entry: dict[str, Any]) -> str:
    value = entry.get("source")
    return value if isinstance(value, str) else "<unknown>"


def _add(
    findings: list[Finding],
    code: str,
    source: str,
    message: str,
    *,
    path: str | None = None,
    severity: str = "warning",
) -> None:
    findings.append(Finding(severity, code, source, message, path))


def _design_import_text(root: Path) -> str:
    return _read_text(root / "core/view/src/design_import.cpp")


def _parse_design_source_labels(design_import_text: str) -> set[str]:
    match = re.search(
        r"parse_design_source\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}",
        design_import_text,
        flags=re.S,
    )
    if not match:
        return set()
    return set(re.findall(r'name\s*==\s*"([^"]+)"', match.group("body")))


def _symbol_exists(root: Path, rel_file: str, symbol: str) -> bool:
    path = root / rel_file
    if not path.exists():
        return False
    return re.search(rf"\b{re.escape(symbol)}\b", _read_text(path)) is not None


def _parser_file_for(
    findings: list[Finding],
    source: str,
    parser_info: dict[str, Any],
    role: str,
) -> str:
    parser_file = parser_info.get("file", DEFAULT_PARSER_FILE)
    if not isinstance(parser_file, str):
        _add(findings, "invalid-parser-file", source, "parser.file must be a string")
        parser_file = DEFAULT_PARSER_FILE

    parser_files = parser_info.get("files") or {}
    if not isinstance(parser_files, dict):
        _add(findings, "invalid-parser-files", source, "parser.files must be an object")
        parser_files = {}

    role_file = parser_files.get(role, parser_file)
    if not isinstance(role_file, str):
        _add(
            findings,
            "invalid-parser-file",
            source,
            f"parser.files.{role} must be a string",
        )
        return parser_file
    return role_file


def _path_state(root: Path, rel_path: str) -> tuple[str, str | None]:
    full_path = root / rel_path
    if full_path.exists():
        return "present", None
    if rel_path.startswith("planning/"):
        planning_root = root / "planning"
        try:
            has_planning_contents = planning_root.exists() and any(planning_root.iterdir())
        except OSError:
            has_planning_contents = False
        if not has_planning_contents:
            return "optional-missing", "planning submodule is not initialized"
    return "missing", None


def _check_existing_path(
    findings: list[Finding],
    root: Path,
    source: str,
    rel_path: Any,
    code: str,
    label: str,
) -> None:
    if not isinstance(rel_path, str) or not rel_path:
        _add(findings, code, source, f"{label} path is missing from the registry")
        return
    state, reason = _path_state(root, rel_path)
    if state == "present":
        return
    if state == "optional-missing":
        _add(
            findings,
            "optional-planning-missing",
            source,
            f"skipped optional planning path {rel_path}: {reason}",
            path=rel_path,
            severity="info",
        )
        return
    _add(findings, code, source, f"{label} does not exist: {rel_path}", path=rel_path)


def _entry_fixture_paths(entry: dict[str, Any]) -> set[str]:
    paths: set[str] = set()
    validation = entry.get("validation") or {}
    for path in _as_list(validation.get("fixtures")):
        if isinstance(path, str):
            paths.add(path)
    for version in _as_list(entry.get("format_versions")):
        if not isinstance(version, dict):
            continue
        for path in _as_list(version.get("fixture_paths")):
            if isinstance(path, str):
                paths.add(path)
    return paths


def _entry_reference_paths(entry: dict[str, Any]) -> set[str]:
    validation = entry.get("validation") or {}
    ref = validation.get("pulp_render_reference") or {}
    path = ref.get("path")
    return {path} if isinstance(path, str) and path else set()


def _strip_shell_value(value: str) -> str:
    value = value.strip()
    if (value.startswith('"') and value.endswith('"')) or (
        value.startswith("'") and value.endswith("'")
    ):
        value = value[1:-1]
    default_match = re.search(r"\$\{[^}:]+:-([^}]+)\}", value)
    if default_match:
        value = default_match.group(1)
    return value


def normalize_shell_repo_path(value: str) -> str | None:
    value = _strip_shell_value(value)
    prefixes = (
        "$PULP_DIR/",
        "${PULP_DIR}/",
        "$PULP/",
        "${PULP}/",
    )
    for prefix in prefixes:
        if value.startswith(prefix):
            return value[len(prefix) :]
    for marker in ("/planning/", "/test/", "/tools/"):
        if marker in value:
            return marker.strip("/") + "/" + value.split(marker, 1)[1]
    return None


def _literal_roundtrip_assignments(script: Path) -> list[tuple[str, str]]:
    assignments: list[tuple[str, str]] = []
    for line in _read_text(script).splitlines():
        match = re.match(r"\s*([A-Z0-9_]*(?:REFERENCE|FIXTURE)[A-Z0-9_]*)=(.+?)\s*$", line)
        if not match:
            continue
        name, value = match.groups()
        if name != "REFERENCE" and not name.endswith("FIXTURE"):
            continue
        normalized = normalize_shell_repo_path(value)
        if normalized is not None:
            assignments.append((name, normalized))
    return assignments


def _check_schema(findings: list[Finding], registry: dict[str, Any]) -> None:
    if registry.get("registry-schema-version") != "0.1":
        _add(
            findings,
            "missing-schema-version",
            "<registry>",
            "registry-schema-version must be present and equal to 0.1",
        )
    if not isinstance(registry.get("contracts"), list):
        _add(findings, "missing-contracts", "<registry>", "contracts must be a list")


def _check_entry_shape(findings: list[Finding], entry: dict[str, Any]) -> None:
    source = _source(entry)
    for field in (
        "source",
        "display_name",
        "kind",
        "trust",
        "compat",
        "dispatch",
        "mcp",
        "upstream",
        "detected_artifact_shape",
        "runtime_export_shape",
        "runtime_requirements",
        "parser",
        "format_versions",
        "validation",
    ):
        if field not in entry:
            _add(findings, "missing-required-field", source, f"missing required field: {field}")

    if entry.get("kind") not in KINDS:
        _add(findings, "invalid-kind", source, f"invalid kind: {entry.get('kind')!r}")
    if entry.get("trust") not in TRUST_LEVELS:
        _add(findings, "invalid-trust", source, f"invalid trust: {entry.get('trust')!r}")

    compat = entry.get("compat") or {}
    if compat.get("mode") not in COMPAT_MODES:
        _add(findings, "invalid-compat-mode", source, f"invalid compat.mode: {compat.get('mode')!r}")

    dispatch = entry.get("dispatch") or {}
    if dispatch.get("kind") not in DISPATCH_KINDS:
        _add(
            findings,
            "invalid-dispatch-kind",
            source,
            f"invalid dispatch.kind: {dispatch.get('kind')!r}",
        )

    ref = (entry.get("validation") or {}).get("pulp_render_reference") or {}
    if ref.get("status") not in REFERENCE_STATUSES:
        _add(
            findings,
            "invalid-reference-status",
            source,
            f"invalid pulp_render_reference.status: {ref.get('status')!r}",
        )


def _check_dispatch_and_symbols(
    findings: list[Finding],
    root: Path,
    entry: dict[str, Any],
    design_source_labels: set[str],
) -> None:
    source = _source(entry)
    dispatch = entry.get("dispatch") or {}
    parser_info = entry.get("parser") or {}
    kind = dispatch.get("kind")
    if kind == "design-source-label":
        label = dispatch.get("label")
        if not isinstance(label, str) or label not in design_source_labels:
            _add(
                findings,
                "missing-design-source-label",
                source,
                f"dispatch label is not present in parse_design_source(): {label!r}",
            )
    elif kind == "explicit-runtime-parser":
        parser = dispatch.get("parser")
        runtime_file = _parser_file_for(findings, source, parser_info, "runtime")
        if not isinstance(parser, str) or not _symbol_exists(
            root, runtime_file, parser
        ):
            _add(
                findings,
                "missing-explicit-runtime-parser",
                source,
                f"explicit runtime parser symbol is missing from {runtime_file}: {parser!r}",
            )

    for role in ("runtime", "static"):
        symbol = parser_info.get(role)
        if symbol is None:
            continue
        parser_file = _parser_file_for(findings, source, parser_info, role)
        if not isinstance(symbol, str) or not _symbol_exists(root, parser_file, symbol):
            _add(
                findings,
                "missing-parser-symbol",
                source,
                f"parser.{role} symbol is missing from {parser_file}: {symbol!r}",
                path=parser_file,
            )


def _check_referential_integrity(
    findings: list[Finding], root: Path, entry: dict[str, Any]
) -> None:
    source = _source(entry)
    for path in sorted(_entry_fixture_paths(entry)):
        _check_existing_path(findings, root, source, path, "missing-fixture", "fixture")

    for version in _as_list(entry.get("format_versions")):
        if not isinstance(version, dict):
            continue
        for path in _as_list(version.get("expected_paths")):
            if isinstance(path, str):
                _check_existing_path(
                    findings, root, source, path, "missing-expected-file", "expected file"
                )

    validation = entry.get("validation") or {}
    script = validation.get("roundtrip_script")
    if script:
        _check_existing_path(
            findings, root, source, script, "missing-roundtrip-script", "roundtrip script"
        )

    ref = validation.get("pulp_render_reference") or {}
    if ref.get("status") == "present":
        _check_existing_path(
            findings,
            root,
            source,
            ref.get("path"),
            "missing-reference-screenshot",
            "reference screenshot",
        )


def _check_test_tags(findings: list[Finding], root: Path, entry: dict[str, Any]) -> None:
    source = _source(entry)
    validation = entry.get("validation") or {}
    test_files = [p for p in _as_list(validation.get("test_files")) if isinstance(p, str)]
    if not test_files:
        _add(findings, "missing-test-files", source, "validation.test_files is empty")
        return

    joined = ""
    for rel_path in test_files:
        path = root / rel_path
        if not path.exists():
            _add(findings, "missing-test-file", source, f"test file does not exist: {rel_path}", path=rel_path)
            continue
        joined += _read_text(path)

    for tag in _as_list(validation.get("test_tags")):
        if not isinstance(tag, str):
            continue
        fragments = re.findall(r"\[([^\]]+)\]", tag)
        missing = [fragment for fragment in fragments if fragment not in joined]
        if missing:
            _add(
                findings,
                "missing-test-tag",
                source,
                f"test tag fragments not found in configured files: {tag} missing {missing}",
            )


def _compat_imports(root: Path) -> dict[str, dict[str, Any]]:
    compat = _load_json(root / "compat.json")
    imports = compat.get("imports") or {}
    return {key: value for key, value in imports.items() if isinstance(value, dict)}


def _compat_formats(entry: dict[str, Any]) -> list[str]:
    return [
        version.get("version")
        for version in _as_list(entry.get("format_versions"))
        if isinstance(version, dict) and isinstance(version.get("version"), str)
    ]


def _check_compat(findings: list[Finding], imports: dict[str, dict[str, Any]], entry: dict[str, Any]) -> None:
    source = _source(entry)
    compat = entry.get("compat") or {}
    if compat.get("mode") != "foreign-key":
        return
    compat_source = compat.get("source")
    if not isinstance(compat_source, str) or compat_source not in imports:
        _add(
            findings,
            "missing-compat-source",
            source,
            f"compat source does not exist in compat.json imports: {compat_source!r}",
        )
        return
    detected = {
        item.get("format-version")
        for item in _as_list(imports[compat_source].get("detected-formats"))
        if isinstance(item, dict)
    }
    for version in _compat_formats(entry):
        if version not in detected:
            _add(
                findings,
                "missing-compat-format",
                source,
                f"format version {version!r} is not present in compat.json imports.{compat_source}.detected-formats",
            )


def _check_surface_refs(findings: list[Finding], entry: dict[str, Any]) -> None:
    source = _source(entry)
    reqs = entry.get("runtime_requirements") or {}
    if not isinstance(reqs.get("sdk_min"), str) or not reqs.get("sdk_min"):
        _add(findings, "missing-sdk-min", source, "runtime_requirements.sdk_min must be a string")
    for ref in _as_list(reqs.get("surface_refs")):
        if not isinstance(ref, str) or not ref.startswith(KNOWN_SURFACE_REFS):
            _add(findings, "invalid-surface-ref", source, f"unknown surface ref owner: {ref!r}")


def _check_cadence(findings: list[Finding], entry: dict[str, Any], today: _dt.date) -> None:
    source = _source(entry)
    last_verified = entry.get("last_verified")
    if last_verified == "unverified":
        _add(findings, "unverified-source", source, "source contract has not been fully verified")
        return
    if entry.get("recheck_interval") == "manual":
        return
    days = entry.get("recheck_interval_days")
    if not isinstance(days, int):
        return
    if not isinstance(last_verified, str):
        _add(findings, "invalid-last-verified", source, "last_verified must be ISO date or unverified")
        return
    try:
        verified_date = _dt.date.fromisoformat(last_verified)
    except ValueError:
        _add(findings, "invalid-last-verified", source, f"invalid ISO date: {last_verified!r}")
        return
    due = verified_date + _dt.timedelta(days=days)
    if due < today:
        _add(
            findings,
            "stale-source-contract",
            source,
            f"source contract recheck is overdue: last verified {verified_date}, due {due}",
        )


def _check_roundtrip_literals(findings: list[Finding], root: Path, entry: dict[str, Any]) -> None:
    source = _source(entry)
    validation = entry.get("validation") or {}
    script = validation.get("roundtrip_script")
    if not isinstance(script, str):
        return
    script_path = root / script
    if not script_path.exists():
        return

    fixtures = _entry_fixture_paths(entry)
    references = _entry_reference_paths(entry)
    for name, rel_path in _literal_roundtrip_assignments(script_path):
        if "REFERENCE" in name:
            if rel_path not in references:
                _add(
                    findings,
                    "roundtrip-reference-drift",
                    source,
                    f"{script} literal {name}={rel_path} is absent from validation.pulp_render_reference",
                    path=script,
                )
        elif rel_path not in fixtures:
            _add(
                findings,
                "roundtrip-fixture-drift",
                source,
                f"{script} literal {name}={rel_path} is absent from fixtures",
                path=script,
            )


def _check_coverage_symmetry(
    findings: list[Finding],
    root: Path,
    entries: list[dict[str, Any]],
    design_source_labels: set[str],
) -> None:
    row_sources = {_source(entry) for entry in entries}
    for label in sorted(design_source_labels):
        if label not in row_sources:
            _add(
                findings,
                "missing-source-row",
                label,
                f"parse_design_source() label has no source-contract row: {label}",
            )

    referenced_scripts = {
        (entry.get("validation") or {}).get("roundtrip_script")
        for entry in entries
        if isinstance((entry.get("validation") or {}).get("roundtrip_script"), str)
    }
    for script in sorted((root / "tools/import-validation").glob("*-roundtrip.sh")):
        rel = script.relative_to(root).as_posix()
        if script.name in ROUNDTRIP_EXEMPT:
            continue
        if rel not in referenced_scripts:
            _add(
                findings,
                "unreferenced-roundtrip-script",
                script.stem.removesuffix("-roundtrip"),
                f"roundtrip script is not referenced by any source contract: {rel}",
                path=rel,
            )


def compute_findings(
    root: Path = REPO_ROOT,
    registry_path: Path = DEFAULT_REGISTRY,
    *,
    today: _dt.date | None = None,
) -> list[Finding]:
    root = root.resolve()
    registry = load_registry(registry_path)
    findings: list[Finding] = []
    today = today or _dt.date.today()

    _check_schema(findings, registry)
    contracts = _as_list(registry.get("contracts"))
    entries = [entry for entry in contracts if isinstance(entry, dict)]
    if len(entries) != len(contracts):
        _add(findings, "invalid-contract-entry", "<registry>", "all contracts entries must be objects")

    design_import_text = _design_import_text(root)
    design_source_labels = _parse_design_source_labels(design_import_text)
    imports = _compat_imports(root)

    for entry in entries:
        _check_entry_shape(findings, entry)
        _check_dispatch_and_symbols(findings, root, entry, design_source_labels)
        _check_referential_integrity(findings, root, entry)
        _check_test_tags(findings, root, entry)
        _check_compat(findings, imports, entry)
        _check_surface_refs(findings, entry)
        _check_cadence(findings, entry, today)
        _check_roundtrip_literals(findings, root, entry)

    _check_coverage_symmetry(findings, root, entries, design_source_labels)
    return findings


def render_report(findings: list[Finding], *, fmt: str = "text") -> str:
    if fmt == "markdown":
        if not findings:
            return "Source-contract check: no findings.\n"
        lines = [
            "| Severity | Code | Source | Path | Message |",
            "|---|---|---|---|---|",
        ]
        for finding in findings:
            path = finding.path or ""
            message = finding.message.replace("|", "\\|")
            lines.append(
                f"| {finding.severity} | `{finding.code}` | `{finding.source}` | `{path}` | {message} |"
            )
        return "\n".join(lines) + "\n"

    if not findings:
        return "source-contracts: no findings\n"
    lines = []
    for finding in findings:
        path = f" ({finding.path})" if finding.path else ""
        lines.append(
            f"[{finding.severity}] {finding.code} {finding.source}: {finding.message}{path}"
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--strict", action="store_true", help="exit 1 on warning/error findings")
    parser.add_argument("--check-upstream", action="store_true", help="accepted for future network rechecks")
    parser.add_argument("--format", choices=("text", "markdown"), default="text")
    args = parser.parse_args(argv)

    findings = compute_findings(root=args.root, registry_path=args.registry)
    sys.stdout.write(render_report(findings, fmt=args.format))
    if args.strict and any(f.severity in {"warning", "error"} for f in findings):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
