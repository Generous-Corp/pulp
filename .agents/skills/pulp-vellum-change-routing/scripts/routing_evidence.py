#!/usr/bin/env python3
"""Validate and attest Pulp's repository-qualified Vellum routing contract."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any, Iterable


PULP_REPOSITORY = "Generous-Corp/pulp"
VELLUM_REPOSITORY = "Generous-Corp/vellum"
DELIVERY_REPOSITORY = "danielraffel/vellum"
EXPANSION_ID = "full-design-import-render-v1"
MATRIX_ID = "full-design-import-render-v1-compatibility-matrix"
AMENDMENT_ID = "full-design-import-render-v1-exact-boundary-amendment-1"
MATRIX_SHA256 = "1792666eb1dd7d3f46dc607f4ee3dccbbc1232a6c2e6ab2331507c4b87122e1c"
AMENDMENT_SHA256 = "cf9b07233e9c66763e1f68391d2df4252dca01bbc7de5acac10dca006fbf5287"
ROUTE_SET_SHA256 = "5bb857c44cdba4cf4e2d584b49ea197960e9b1b490703ce3500c649dc9995028"
APPROVED_ROUTES_PATH = (
    Path(__file__).resolve().parents[1] / "references/approved-exact-routes.v1.json"
)
WORKFLOW_PATH = ".github/workflows/vellum-routing-contract.yml"
REQUIRED_CASE_IDS = [
    "authority-load",
    "cli-contract",
    "conflicting-owner-fail-closed",
    "exact-projection-expansion",
    "generic-pulp-route",
    "mixed-multi-path-route",
    "pulp-specific-route",
    "vellum-generic-route",
]
SHA40 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
OWNER = re.compile(r"^@[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})$")
CELL_ID = re.compile(r"^[a-z0-9][a-z0-9.-]*$")
SLICE_ID = re.compile(r"^[a-z0-9][a-z0-9-]*$")
EVENT_ID = re.compile(r"^[0-9]{8}-[a-z0-9][a-z0-9-]{2,79}$")
RECORD_PATH = re.compile(
    r"^provenance/authority/records/[a-z0-9][a-z0-9._-]{2,120}\.json$"
)


class RoutingError(RuntimeError):
    """Routing authority is missing, malformed, or contradictory."""


def canonical_sha256(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _strict_keys(value: Any, expected: set[str], where: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != expected:
        raise RoutingError(f"{where} has missing or unknown fields")
    return value


def _utc_timestamp(value: Any, where: str) -> str:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise RoutingError(f"{where} must be an ISO-8601 UTC timestamp")
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as error:
        raise RoutingError(f"{where} must be an ISO-8601 UTC timestamp") from error
    if parsed.utcoffset() != dt.timedelta(0):
        raise RoutingError(f"{where} must be UTC")
    return value


def _exact_path(value: Any, where: str) -> str:
    if not isinstance(value, str):
        raise RoutingError(f"{where} must be a repository-relative path")
    parts = value.split("/")
    if (
        value.startswith("/")
        or value.endswith("/")
        or "\\" in value
        or any(part in {"", ".", ".."} for part in parts)
        or any(marker in value for marker in ("*", "?", "[", "]"))
    ):
        raise RoutingError(f"{where} must be one exact repository-relative path")
    return value


def _canonical_repository(repository: str) -> str:
    if repository == DELIVERY_REPOSITORY:
        return VELLUM_REPOSITORY
    if repository not in {PULP_REPOSITORY, VELLUM_REPOSITORY}:
        raise RoutingError(f"unsupported repository: {repository}")
    return repository


def approved_routes() -> list[dict[str, Any]]:
    try:
        value = json.loads(APPROVED_ROUTES_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RoutingError(f"approved exact-route artifact is unavailable: {error}") from error
    expected = {
        "schema_version": 1,
        "matrix_id": MATRIX_ID,
        "matrix_sha256": MATRIX_SHA256,
        "amendment_id": AMENDMENT_ID,
        "amendment_sha256": AMENDMENT_SHA256,
        "route_set_sha256": ROUTE_SET_SHA256,
    }
    if not isinstance(value, dict) or set(value) != set(expected) | {"routes"}:
        raise RoutingError("approved exact-route artifact fields differ")
    for key, expected_value in expected.items():
        if value.get(key) != expected_value:
            raise RoutingError(f"approved exact-route artifact {key} differs")
    routes = value.get("routes")
    if not isinstance(routes, list) or canonical_sha256(routes) != ROUTE_SET_SHA256:
        raise RoutingError("approved exact-route artifact routes differ")
    return routes


def validate_expansion(expansion: Any) -> dict[str, Any]:
    value = _strict_keys(
        expansion,
        {
            "id",
            "state",
            "accepted_at",
            "accepted_by",
            "amendment_id",
            "matrix_id",
            "matrix_sha256",
            "route_set_sha256",
            "routes",
        },
        "ownership expansion",
    )
    expected = {
        "id": EXPANSION_ID,
        "state": "accepted-pending-vellum-acknowledgement",
        "accepted_by": "@danielraffel",
        "amendment_id": AMENDMENT_ID,
        "matrix_id": MATRIX_ID,
        "matrix_sha256": MATRIX_SHA256,
    }
    for key, expected_value in expected.items():
        if value.get(key) != expected_value:
            raise RoutingError(f"ownership expansion {key} differs")
    _utc_timestamp(value.get("accepted_at"), "ownership expansion accepted_at")
    routes = value.get("routes")
    if not isinstance(routes, list) or not routes:
        raise RoutingError("ownership expansion routes must be a non-empty array")

    previous_key: tuple[str, str] | None = None
    seen: set[tuple[str, str]] = set()
    for index, route in enumerate(routes):
        row = _strict_keys(
            route,
            {"repository", "path", "owner", "cell_roles"},
            f"ownership expansion routes[{index}]",
        )
        repository = row.get("repository")
        if repository not in {PULP_REPOSITORY, VELLUM_REPOSITORY}:
            raise RoutingError(f"ownership expansion routes[{index}] has unknown repository")
        if row.get("owner") != repository:
            raise RoutingError(f"ownership expansion routes[{index}] conflicts with its owner")
        path = _exact_path(row.get("path"), f"ownership expansion routes[{index}].path")
        key = (repository, path)
        if key in seen or (previous_key is not None and key <= previous_key):
            raise RoutingError("ownership expansion routes must be unique and sorted")
        seen.add(key)
        previous_key = key

        roles = row.get("cell_roles")
        if not isinstance(roles, list) or not roles:
            raise RoutingError(f"ownership expansion routes[{index}].cell_roles is empty")
        normalized_roles: list[tuple[str, str]] = []
        allowed_roles = (
            {"pulp_implementation", "pulp_proof"}
            if repository == PULP_REPOSITORY
            else {"vellum_future_implementation", "vellum_future_proof"}
        )
        for role_index, role_value in enumerate(roles):
            role = _strict_keys(
                role_value,
                {"cell_id", "role"},
                f"ownership expansion routes[{index}].cell_roles[{role_index}]",
            )
            cell_id = role.get("cell_id")
            role_name = role.get("role")
            if not isinstance(cell_id, str) or CELL_ID.fullmatch(cell_id) is None:
                raise RoutingError("ownership expansion route has an invalid cell id")
            if role_name not in allowed_roles:
                raise RoutingError("ownership expansion route has a repository-incompatible role")
            normalized_roles.append((cell_id, role_name))
        if normalized_roles != sorted(set(normalized_roles)):
            raise RoutingError("ownership expansion cell roles must be unique and sorted")

    route_set_sha256 = value.get("route_set_sha256")
    if not isinstance(route_set_sha256, str) or SHA256.fullmatch(route_set_sha256) is None:
        raise RoutingError("ownership expansion route_set_sha256 is invalid")
    if route_set_sha256 != canonical_sha256(routes):
        raise RoutingError("ownership expansion route_set_sha256 differs from routes")
    if route_set_sha256 != ROUTE_SET_SHA256 or routes != approved_routes():
        raise RoutingError("ownership expansion routes differ from the approved exact boundary")
    return value


def _validate_base_projection(projection: dict[str, Any]) -> None:
    activation = _strict_keys(
        projection.get("activation"),
        {
            "state",
            "pulp_extraction_base",
            "vellum_authority_commit",
            "authority_record_path",
            "initial_transition_event",
            "accepted_by",
            "accepted_at",
        },
        "ownership projection activation",
    )
    state = activation.get("state")
    if state not in {"prepared", "active"}:
        raise RoutingError("ownership projection activation state differs")
    if not isinstance(activation.get("pulp_extraction_base"), str) or SHA40.fullmatch(
        activation["pulp_extraction_base"]
    ) is None:
        raise RoutingError("ownership projection extraction base is invalid")
    authority_fields = (
        "vellum_authority_commit",
        "authority_record_path",
        "initial_transition_event",
        "accepted_by",
        "accepted_at",
    )
    if state == "prepared":
        if any(activation.get(field) is not None for field in authority_fields):
            raise RoutingError("prepared ownership activation must not carry authority")
    else:
        if not isinstance(activation.get("vellum_authority_commit"), str) or SHA40.fullmatch(
            activation["vellum_authority_commit"]
        ) is None:
            raise RoutingError("active ownership authority commit is invalid")
        if not isinstance(activation.get("authority_record_path"), str) or RECORD_PATH.fullmatch(
            activation["authority_record_path"]
        ) is None:
            raise RoutingError("active ownership authority record is invalid")
        if not isinstance(activation.get("initial_transition_event"), str) or EVENT_ID.fullmatch(
            activation["initial_transition_event"]
        ) is None:
            raise RoutingError("active ownership transition event is invalid")
        if activation.get("accepted_by") != "@danielraffel":
            raise RoutingError("active ownership acceptance owner differs")
        _utc_timestamp(activation.get("accepted_at"), "ownership activation accepted_at")

    slices = projection.get("slices")
    if not isinstance(slices, list) or not slices:
        raise RoutingError("ownership projection slices must be a non-empty array")
    seen_ids: set[str] = set()
    for index, item in enumerate(slices):
        row = _strict_keys(
            item,
            {"id", "state", "paths", "authority"},
            f"ownership projection slices[{index}]",
        )
        slice_id = row.get("id")
        if (
            not isinstance(slice_id, str)
            or SLICE_ID.fullmatch(slice_id) is None
            or slice_id in seen_ids
        ):
            raise RoutingError("ownership projection slice id is invalid or duplicated")
        seen_ids.add(slice_id)
        slice_state = row.get("state")
        if slice_state not in {
            "pulp-authoritative-untransferred",
            "framework-authoritative-transferred",
            "framework-reimplemented-no-transfer",
            "pulp-only",
            "excluded",
        }:
            raise RoutingError(f"ownership projection slice {slice_id} state is invalid")
        paths = row.get("paths")
        if not isinstance(paths, list) or not paths or paths != sorted(set(paths)):
            raise RoutingError(f"ownership projection slice {slice_id} paths are invalid")
        for path in paths:
            if (
                not isinstance(path, str)
                or not path
                or path.startswith(("/", "-"))
                or "\\" in path
                or ".." in Path(path).parts
            ):
                raise RoutingError(f"ownership projection slice {slice_id} path is unsafe")
        authority = row.get("authority")
        if slice_state == "framework-authoritative-transferred":
            authority = _strict_keys(
                authority,
                {"event_id", "vellum_commit", "counterpart", "accepted_by", "accepted_at"},
                f"ownership projection slice {slice_id} authority",
            )
            if (
                not isinstance(authority.get("event_id"), str)
                or EVENT_ID.fullmatch(authority["event_id"]) is None
                or not isinstance(authority.get("vellum_commit"), str)
                or SHA40.fullmatch(authority["vellum_commit"]) is None
                or not isinstance(authority.get("counterpart"), str)
                or RECORD_PATH.fullmatch(authority["counterpart"]) is None
                or authority.get("accepted_by") != "@danielraffel"
            ):
                raise RoutingError(f"ownership projection slice {slice_id} authority is invalid")
            _utc_timestamp(
                authority.get("accepted_at"),
                f"ownership projection slice {slice_id} authority accepted_at",
            )
        elif authority is not None:
            raise RoutingError(f"ownership projection slice {slice_id} has unexpected authority")


def validate_projection(
    projection: Any, *, require_expansion: bool = False
) -> dict[str, Any] | None:
    if not isinstance(projection, dict):
        raise RoutingError("ownership projection must be an object")
    schema = projection.get("schema_version")
    base_keys = {
        "schema_version",
        "framework_repository",
        "freeze_owner",
        "activation",
        "slices",
    }
    expected_keys = base_keys | ({"expansions"} if schema == 3 else set())
    if schema not in {2, 3} or set(projection) != expected_keys:
        raise RoutingError("ownership projection schema or top-level fields differ")
    if projection.get("framework_repository") != VELLUM_REPOSITORY:
        raise RoutingError("ownership projection framework repository differs")
    if projection.get("freeze_owner") != "@danielraffel":
        raise RoutingError("ownership projection freeze owner differs")
    _validate_base_projection(projection)
    if schema == 2:
        if require_expansion:
            raise RoutingError("ownership projection has no accepted exact-route expansion")
        return None
    expansions = projection.get("expansions")
    if not isinstance(expansions, list) or len(expansions) != 1:
        raise RoutingError("ownership projection must contain exactly one accepted expansion")
    return validate_expansion(expansions[0])


def load_projection(path: Path, *, require_expansion: bool = True) -> tuple[dict[str, Any], dict[str, Any] | None]:
    try:
        projection = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RoutingError(f"cannot read ownership projection {path}: {error}") from error
    expansion = validate_projection(projection, require_expansion=require_expansion)
    return projection, expansion


def _transferred_pulp_paths(projection: dict[str, Any]) -> set[str]:
    transferred: set[str] = set()
    for index, item in enumerate(projection["slices"]):
        row = _strict_keys(
            item,
            {"id", "state", "paths", "authority"},
            f"ownership projection slices[{index}]",
        )
        if row.get("state") != "framework-authoritative-transferred":
            continue
        paths = row.get("paths")
        if not isinstance(paths, list) or paths != sorted(set(paths)):
            raise RoutingError("transferred ownership paths must be a sorted unique array")
        for path_index, path_value in enumerate(paths):
            path = _exact_path(
                path_value,
                f"ownership projection slices[{index}].paths[{path_index}]",
            )
            if path in transferred:
                raise RoutingError("transferred ownership paths must be globally unique")
            transferred.add(path)
    return transferred


def route_changes(
    projection: dict[str, Any],
    expansion: dict[str, Any],
    changes: Iterable[tuple[str, str]],
    *,
    claimed_owner: str | None = None,
) -> dict[str, Any]:
    routes = expansion["routes"]
    index = {(row["repository"], row["path"]): row for row in routes}
    transferred_pulp_paths = _transferred_pulp_paths(projection)
    results: list[dict[str, Any]] = []
    for change_index, (repository, path_value) in enumerate(changes):
        canonical_repository = _canonical_repository(repository)
        path = _exact_path(path_value, f"changes[{change_index}].path")
        exact = index.get((canonical_repository, path))
        initial_transfer = (
            exact is None
            and canonical_repository == PULP_REPOSITORY
            and path in transferred_pulp_paths
        )
        if exact is not None:
            owner = exact["owner"]
            route_kind = "exact"
        elif initial_transfer:
            owner = VELLUM_REPOSITORY
            route_kind = "initial-cut-transfer"
        else:
            owner = canonical_repository
            route_kind = "repository-default"
        if claimed_owner is not None and _canonical_repository(claimed_owner) != owner:
            raise RoutingError(
                f"claimed owner {claimed_owner} conflicts with routed owner {owner} for {path}"
            )
        results.append(
            {
                "repository": repository,
                "path": path,
                "owner": owner,
                "route_kind": route_kind,
                "cell_roles": exact["cell_roles"] if exact is not None else [],
            }
        )
    if not results:
        raise RoutingError("at least one changed path is required")
    owners = sorted({row["owner"] for row in results})
    return {
        "schema_version": 1,
        "kind": "pulp-vellum-change-route",
        "expansion_id": expansion["id"],
        "route_set_sha256": expansion["route_set_sha256"],
        "decision": "single-owner" if len(owners) == 1 else "coordinated",
        "owners": owners,
        "changes": results,
    }


def _load_case_results(path: Path) -> list[dict[str, str]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RoutingError(f"cannot read router case results: {error}") from error
    expected = {
        "schema_version": 1,
        "kind": "pulp-vellum-routing-contract-cases",
        "case_results": [
            {"case_id": case_id, "status": "pass"} for case_id in REQUIRED_CASE_IDS
        ],
    }
    if value != expected:
        raise RoutingError("router case results do not prove the closed contract suite")
    return value["case_results"]


def emit_receipt(
    *, projection_path: Path, case_results_path: Path, head_sha: str,
    run_id: int, output: Path,
) -> None:
    if SHA40.fullmatch(head_sha) is None:
        raise RoutingError("receipt head SHA must contain 40 lowercase hex characters")
    if run_id <= 0:
        raise RoutingError("receipt run ID must be positive")
    _projection, expansion = load_projection(projection_path, require_expansion=True)
    assert expansion is not None
    case_results = _load_case_results(case_results_path)
    scripts = Path(__file__).resolve().parent
    receipt = {
        "schema_version": 1,
        "kind": "pulp-vellum-routing-contract-execution",
        "repository": PULP_REPOSITORY,
        "head_sha": head_sha,
        "run_id": run_id,
        "workflow_path": WORKFLOW_PATH,
        "status": "pass",
        "route_set_sha256": expansion["route_set_sha256"],
        "router_sha256": file_sha256(scripts / "route_change.py"),
        "router_contract_test_sha256": file_sha256(scripts / "test_route_change.py"),
        "router_dependency_sha256": file_sha256(scripts / "routing_evidence.py"),
        "case_results": case_results,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate", help="validate the ownership projection")
    validate.add_argument("--projection", type=Path, required=True)
    validate.add_argument("--require-expansion", action="store_true")
    receipt = subparsers.add_parser("receipt", help="emit a run-bound execution receipt")
    receipt.add_argument("--projection", type=Path, required=True)
    receipt.add_argument("--case-results", type=Path, required=True)
    receipt.add_argument("--head-sha", required=True)
    receipt.add_argument("--run-id", type=int, required=True)
    receipt.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "validate":
            load_projection(args.projection, require_expansion=args.require_expansion)
            print(f"validated Vellum ownership projection: {args.projection}")
        else:
            emit_receipt(
                projection_path=args.projection,
                case_results_path=args.case_results,
                head_sha=args.head_sha,
                run_id=args.run_id,
                output=args.output,
            )
            print(f"wrote Vellum routing receipt: {args.output}")
        return 0
    except RoutingError as error:
        print(f"pulp-vellum-routing: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
