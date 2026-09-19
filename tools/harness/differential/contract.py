"""Stable, execution-neutral browser/native differential contract.

The contract deliberately does not render anything. Browser and native adapters
produce observations, then this module validates and normalizes their report so
Chromium stays the appearance authority while later adapters can be added
without changing the receipt shape.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from typing import Any, Mapping

SCHEMA = "pulp-browser-native-differential-report-v1"
MANIFEST_SCHEMA = "pulp-canvas-svg-differential-manifest-v1"
SURFACES = frozenset({"canvas", "svg"})
FINDING_KINDS = frozenset({
    "dropped-material", "wrong-geometry", "wrong-pixels", "unsupported-behavior"
})
_CLASSIFICATION_TO_FINDING = {
    "dropped-material": "dropped-material",
    "geometry": "wrong-geometry",
    "visual": "wrong-pixels",
    "unsupported-behavior": "unsupported-behavior",
}


@dataclass(frozen=True)
class FixtureSpec:
    id: str
    source: str
    surface: str
    features: tuple[str, ...]
    expected_findings: tuple[str, ...]

    @property
    def source_path(self) -> Path:
        return Path(self.source)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_fixture_manifest(path: Path) -> tuple[dict[str, Any], list[FixtureSpec]]:
    """Load and validate a bounded Canvas/SVG manifest."""
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict) or raw.get("schema") != MANIFEST_SCHEMA:
        raise ValueError(f"{path}: expected schema {MANIFEST_SCHEMA}")
    fixtures = raw.get("fixtures")
    if not isinstance(fixtures, list) or not fixtures:
        raise ValueError(f"{path}: fixtures must be a non-empty array")
    specs: list[FixtureSpec] = []
    seen: set[str] = set()
    for item in fixtures:
        if not isinstance(item, dict):
            raise ValueError(f"{path}: fixture entries must be objects")
        fixture_id = item.get("id")
        source = item.get("source")
        surface = item.get("surface")
        if not all(isinstance(v, str) and v for v in (fixture_id, source, surface)):
            raise ValueError(f"{path}: fixture requires non-empty id/source/surface")
        if fixture_id in seen:
            raise ValueError(f"{path}: duplicate fixture id {fixture_id!r}")
        if surface not in SURFACES:
            raise ValueError(f"{path}: unsupported surface {surface!r}")
        source_path = (path.parent / source).resolve()
        if path.parent.resolve() not in source_path.parents:
            raise ValueError(f"{path}: source escapes manifest directory: {source}")
        if not source_path.is_file():
            raise ValueError(f"{path}: source does not exist: {source}")
        features = item.get("features", [])
        expected = item.get("expected_findings", [])
        if not all(isinstance(v, str) and v for v in features):
            raise ValueError(f"{path}: features must be strings for {fixture_id}")
        if not all(v in FINDING_KINDS for v in expected):
            raise ValueError(f"{path}: unknown expected finding for {fixture_id}")
        specs.append(FixtureSpec(fixture_id, source, surface, tuple(features), tuple(expected)))
        seen.add(fixture_id)
    return raw, specs


def _observation(value: Any, fixture: FixtureSpec, side: str) -> dict[str, Any]:
    if value is None:
        value = {}
    if not isinstance(value, Mapping):
        raise ValueError(f"{fixture.id}: {side} observation must be an object")
    status = value.get("status", "not-run")
    if status not in {"pass", "fail", "not-run"}:
        raise ValueError(f"{fixture.id}: invalid {side} status {status!r}")
    findings = value.get("findings", [])
    if not isinstance(findings, list):
        raise ValueError(f"{fixture.id}: {side}.findings must be an array")
    normalized: list[dict[str, Any]] = []
    for finding in findings:
        if not isinstance(finding, Mapping) or finding.get("kind") not in FINDING_KINDS:
            raise ValueError(f"{fixture.id}: invalid {side} finding")
        entry = {"kind": finding["kind"], "message": str(finding.get("message", ""))}
        if "path" in finding:
            entry["path"] = str(finding["path"])
        normalized.append(entry)
    normalized.sort(key=lambda item: (item["kind"], item.get("path", ""), item["message"]))
    evidence = value.get("evidence", [])
    if status == "pass" and (not isinstance(evidence, list) or not evidence or any(not isinstance(item, str) or not item for item in evidence)):
        raise ValueError(f"{fixture.id}: pass {side} observation lacks evidence")
    if not isinstance(evidence, list) or any(not isinstance(item, str) or not item for item in evidence):
        raise ValueError(f"{fixture.id}: {side}.evidence must be a string array")
    return {"status": status, "findings": normalized, "evidence": sorted(evidence)}


def observations_from_lab_reports(
    reports: list[Mapping[str, Any]],
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Map importer-lab reports to contract observations exactly once.

    Reports are execution artifacts, so a missing fixture is left for
    ``normalize_report`` to represent as ``not-run``. A report without an id,
    or two reports for one id, is ambiguous and must fail before publication.
    """
    browser: dict[str, Any] = {}
    native: dict[str, Any] = {}
    for report in reports:
        fixture = report.get("fixture") if isinstance(report, Mapping) else None
        fixture_id = fixture.get("id") if isinstance(fixture, Mapping) else None
        if not isinstance(fixture_id, str) or not fixture_id:
            raise ValueError("lab report lacks a fixture id")
        if fixture_id in browser:
            raise ValueError(f"ambiguous lab observations for fixture {fixture_id!r}")
        browser[fixture_id] = {
            "status": "pass",
            "evidence": ["browser/browser.png", "browser/dom-snapshot.json"],
        }
        findings = []
        for classification in report.get("classifications", []):
            kind = classification.get("kind")
            mapped = _CLASSIFICATION_TO_FINDING.get(kind)
            if mapped:
                findings.append({
                    "kind": mapped,
                    "message": classification.get("detail", kind),
                })
        native[fixture_id] = {
            "status": "fail" if findings else "pass",
            "findings": findings,
            "evidence": ["comparison/report.json"] if findings else [
                "candidate/render.png", "comparison/report.json"
            ],
        }
    return browser, native


def normalize_report(
    manifest_path: Path,
    *,
    browser: Mapping[str, Any] | None = None,
    native: Mapping[str, Any] | None = None,
    tool_version: str = "pulp-differential-lab/1",
) -> DifferentialReport:
    """Build a deterministic report from adapter observations."""
    raw_manifest, specs = load_fixture_manifest(manifest_path)
    if raw_manifest.get("reference", {}).get("backend") != "chromium":
        raise ValueError("manifest reference backend must be chromium")
    browser = browser or {}
    native = native or {}
    rows = []
    for spec in specs:
        source_path = manifest_path.parent / spec.source
        rows.append({
            "id": spec.id,
            "surface": spec.surface,
            "source": spec.source,
            "source_sha256": _sha256(source_path),
            "features": list(spec.features),
            "expected_findings": list(spec.expected_findings),
            "browser": _observation(browser.get(spec.id), spec, "browser"),
            "native": _observation(native.get(spec.id), spec, "native"),
        })
    rows.sort(key=lambda row: row["id"])
    return DifferentialReport({
        "schema": SCHEMA,
        "tool": tool_version,
        "reference": {"backend": "chromium", "role": "appearance-authority"},
        "native": {"backend": "pulp", "role": "candidate"},
        "manifest": {"path": manifest_path.name, "schema": MANIFEST_SCHEMA},
        "fixtures": rows,
    })


class DifferentialReport(dict):
    """Mapping wrapper with JSON serialization and contract validation."""

    def to_json(self) -> str:
        validate_report(self)
        return json.dumps(self, indent=2, sort_keys=True) + "\n"


def validate_report(report: Mapping[str, Any]) -> None:
    if report.get("schema") != SCHEMA:
        raise ValueError(f"expected schema {SCHEMA}")
    if report.get("reference") != {"backend": "chromium", "role": "appearance-authority"}:
        raise ValueError("Chromium must remain the appearance authority")
    if report.get("native", {}).get("backend") != "pulp":
        raise ValueError("native backend must be pulp")
    fixtures = report.get("fixtures")
    if not isinstance(fixtures, list) or not fixtures:
        raise ValueError("report fixtures must be a non-empty array")
    if any(not isinstance(row, Mapping) for row in fixtures):
        raise ValueError("report fixtures must contain objects")
    ids = [row.get("id") for row in fixtures]
    if len(ids) != len(set(ids)) or any(not isinstance(i, str) or not i for i in ids):
        raise ValueError("report fixture ids must be unique non-empty strings")
    if ids != sorted(ids):
        raise ValueError("report fixtures must be sorted by id")
    for row in fixtures:
        if not isinstance(row.get("surface"), str) or row["surface"] not in SURFACES:
            raise ValueError(f"{row.get('id')}: invalid surface")
        if not isinstance(row.get("source"), str) or not isinstance(row.get("source_sha256"), str) or len(row["source_sha256"]) != 64:
            raise ValueError(f"{row.get('id')}: source identity is malformed")
        for field in ("features", "expected_findings"):
            values = row.get(field)
            if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
                raise ValueError(f"{row.get('id')}: {field} must be a string array")
        if any(value not in FINDING_KINDS for value in row["expected_findings"]):
            raise ValueError(f"{row.get('id')}: unknown expected finding")
        for side in ("browser", "native"):
            observation = row.get(side)
            if not isinstance(observation, Mapping) or observation.get("status") not in {"pass", "fail", "not-run"}:
                raise ValueError(f"{row.get('id')}: malformed {side} observation")
            findings = observation.get("findings")
            if not isinstance(findings, list):
                raise ValueError(f"{row.get('id')}: malformed {side}.findings")
            for finding in findings:
                if not isinstance(finding, Mapping) or finding.get("kind") not in FINDING_KINDS or not isinstance(finding.get("message", ""), str):
                    raise ValueError(f"{row.get('id')}: malformed {side} finding")
            if observation["status"] == "pass":
                evidence = observation.get("evidence")
                if not isinstance(evidence, list) or not evidence or any(not isinstance(item, str) or not item for item in evidence):
                    raise ValueError(f"{row.get('id')}: pass {side} observation lacks evidence")
