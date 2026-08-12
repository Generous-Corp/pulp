#!/usr/bin/env python3
"""Closed contract suite for Pulp/Vellum change routing."""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Callable, Iterable

from routing_evidence import (
    AMENDMENT_ID,
    DELIVERY_REPOSITORY,
    EXPANSION_ID,
    MATRIX_ID,
    MATRIX_SHA256,
    PULP_REPOSITORY,
    REQUIRED_CASE_IDS,
    RoutingError,
    VELLUM_REPOSITORY,
    canonical_sha256,
    approved_routes,
    load_projection,
    route_changes,
    validate_expansion,
    validate_projection,
)


SCRIPTS = Path(__file__).resolve().parent
ROUTER = SCRIPTS / "route_change.py"
ROOT = SCRIPTS.parents[3]
WORKFLOW = ROOT / ".github/workflows/vellum-routing-contract.yml"
PULP_EXACT_PATH = "tools/scripts/package_cli.py"
VELLUM_EXACT_PATH = "cli/vellum"
TRANSFERRED_PULP_PATH = "core/canvas/src/skia_canvas.cpp"


def fixture_projection() -> dict:
    routes = approved_routes()
    authority = {
        "event_id": "20260724-authority-activation-attempt-2",
        "vellum_commit": "a106a02816a0cde53daac83f36a6630d664f6637",
        "counterpart": "provenance/authority/records/native-design-kernel-v1-attempt-2.json",
        "accepted_by": "@danielraffel",
        "accepted_at": "2026-07-24T12:03:00Z",
    }
    return {
        "schema_version": 3,
        "framework_repository": VELLUM_REPOSITORY,
        "freeze_owner": "@danielraffel",
        "activation": {
            "state": "active",
            "pulp_extraction_base": "2ccff748f0d59da34b01ce1fbceabcf19f452731",
            "vellum_authority_commit": authority["vellum_commit"],
            "authority_record_path": authority["counterpart"],
            "initial_transition_event": authority["event_id"],
            "accepted_by": authority["accepted_by"],
            "accepted_at": authority["accepted_at"],
        },
        "slices": [
            {
                "id": "canvas-kernel",
                "state": "framework-authoritative-transferred",
                "paths": [TRANSFERRED_PULP_PATH],
                "authority": authority,
            }
        ],
        "expansions": [
            {
                "id": EXPANSION_ID,
                "state": "accepted-pending-vellum-acknowledgement",
                "accepted_at": "2026-08-12T00:00:00Z",
                "accepted_by": "@danielraffel",
                "amendment_id": AMENDMENT_ID,
                "matrix_id": MATRIX_ID,
                "matrix_sha256": MATRIX_SHA256,
                "route_set_sha256": canonical_sha256(routes),
                "routes": routes,
            }
        ],
    }


def expansion() -> dict:
    value = validate_projection(fixture_projection(), require_expansion=True)
    assert value is not None
    return value


def case_authority_load() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "projection.json"
        path.write_text(json.dumps(fixture_projection()), encoding="utf-8")
        projection, loaded = load_projection(path, require_expansion=True)
        assert projection["schema_version"] == 3
        assert loaded is not None and loaded["id"] == EXPANSION_ID
    malformed = fixture_projection()
    malformed["activation"] = {}
    try:
        validate_projection(malformed, require_expansion=True)
    except RoutingError as error:
        assert "activation" in str(error)
    else:
        raise AssertionError("malformed activation authority was accepted")
    malformed = fixture_projection()
    malformed["slices"][0]["authority"] = {}
    try:
        validate_projection(malformed, require_expansion=True)
    except RoutingError as error:
        assert "authority" in str(error)
    else:
        raise AssertionError("malformed slice authority was accepted")


def case_cli_contract() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "projection.json"
        path.write_text(json.dumps(fixture_projection()), encoding="utf-8")
        completed = subprocess.run(
            [
                sys.executable,
                str(ROUTER),
                "--projection",
                str(path),
                "--repository",
                PULP_REPOSITORY,
                "--json",
                PULP_EXACT_PATH,
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert completed.returncode == 0, completed.stderr
        result = json.loads(completed.stdout)
        assert set(result) == {
            "schema_version",
            "kind",
            "expansion_id",
            "route_set_sha256",
            "decision",
            "owners",
            "changes",
        }
        assert result["changes"][0]["route_kind"] == "exact"
    workflow = WORKFLOW.read_text(encoding="utf-8")
    for required in (
        "push:\n    branches: [main]",
        "name: vellum-routing-contract",
        "receipt \\",
        "name: pulp-vellum-routing-contract-execution",
        "retention-days: 90",
    ):
        assert required in workflow, f"workflow contract is missing {required!r}"
    trusted_receipt_condition = (
        "if: steps.projection.outputs.has_expansion == 'true' && "
        "github.event_name == 'push' && github.ref == 'refs/heads/main'"
    )
    assert workflow.count(trusted_receipt_condition) == 2


def case_conflicting_owner_fail_closed() -> None:
    value = copy.deepcopy(expansion())
    value["routes"][0]["owner"] = VELLUM_REPOSITORY
    value["route_set_sha256"] = canonical_sha256(value["routes"])
    try:
        validate_expansion(value)
    except RoutingError as error:
        assert "conflicts" in str(error)
    else:
        raise AssertionError("conflicting route owner was accepted")


def case_exact_projection_expansion() -> None:
    value = expansion()
    assert value["route_set_sha256"] == canonical_sha256(value["routes"])
    assert len(value["routes"]) == 223
    changed = copy.deepcopy(value)
    changed["routes"][0]["path"] = "compat/forged.json"
    changed["route_set_sha256"] = canonical_sha256(changed["routes"])
    try:
        validate_expansion(changed)
    except RoutingError as error:
        assert "approved exact boundary" in str(error)
    else:
        raise AssertionError("self-hashed routes outside the approved boundary were accepted")


def case_generic_pulp_route() -> None:
    projection = fixture_projection()
    result = route_changes(
        projection, expansion(), [(PULP_REPOSITORY, "docs/reference/cli.md")]
    )
    assert result["decision"] == "single-owner"
    assert result["changes"][0]["owner"] == PULP_REPOSITORY
    assert result["changes"][0]["route_kind"] == "repository-default"
    transferred = route_changes(
        projection, expansion(), [(PULP_REPOSITORY, TRANSFERRED_PULP_PATH)]
    )
    assert transferred["changes"][0]["owner"] == VELLUM_REPOSITORY
    assert transferred["changes"][0]["route_kind"] == "initial-cut-transfer"


def case_mixed_multi_path_route() -> None:
    result = route_changes(
        fixture_projection(),
        expansion(),
        [
            (PULP_REPOSITORY, PULP_EXACT_PATH),
            (DELIVERY_REPOSITORY, VELLUM_EXACT_PATH),
        ],
    )
    assert result["decision"] == "coordinated"
    assert result["owners"] == [PULP_REPOSITORY, VELLUM_REPOSITORY]
    assert all(row["route_kind"] == "exact" for row in result["changes"])


def case_pulp_specific_route() -> None:
    result = route_changes(
        fixture_projection(), expansion(), [(PULP_REPOSITORY, PULP_EXACT_PATH)]
    )
    row = result["changes"][0]
    assert row["owner"] == PULP_REPOSITORY
    assert row["cell_roles"] == [
        {"cell_id": "output.cli-and-immutable-runtime-assets", "role": "pulp_implementation"}
    ]


def case_vellum_generic_route() -> None:
    result = route_changes(
        fixture_projection(),
        expansion(),
        [(VELLUM_REPOSITORY, "docs/reference/public-api.md")],
    )
    assert result["changes"][0]["owner"] == VELLUM_REPOSITORY
    assert result["changes"][0]["route_kind"] == "repository-default"


CASES: dict[str, Callable[[], None]] = {
    "authority-load": case_authority_load,
    "cli-contract": case_cli_contract,
    "conflicting-owner-fail-closed": case_conflicting_owner_fail_closed,
    "exact-projection-expansion": case_exact_projection_expansion,
    "generic-pulp-route": case_generic_pulp_route,
    "mixed-multi-path-route": case_mixed_multi_path_route,
    "pulp-specific-route": case_pulp_specific_route,
    "vellum-generic-route": case_vellum_generic_route,
}


def run_cases() -> tuple[list[dict[str, str]], list[str]]:
    if list(CASES) != REQUIRED_CASE_IDS:
        return [], ["case manifest differs from the required closed suite"]
    results: list[dict[str, str]] = []
    failures: list[str] = []
    for case_id, case in CASES.items():
        try:
            case()
            results.append({"case_id": case_id, "status": "pass"})
        except Exception as error:  # noqa: BLE001 - every case must report a verdict
            results.append({"case_id": case_id, "status": "fail"})
            failures.append(f"{case_id}: {type(error).__name__}: {error}")
    return results, failures


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-report", type=Path)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    results, failures = run_cases()
    report = {
        "schema_version": 1,
        "kind": "pulp-vellum-routing-contract-cases",
        "case_results": results,
    }
    if args.json_report is not None:
        args.json_report.parent.mkdir(parents=True, exist_ok=True)
        args.json_report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    for result in results:
        print(f"{result['status'].upper()}: {result['case_id']}")
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
