#!/usr/bin/env python3
"""Validate immutable Vellum expansion acceptances and watched Pulp changes."""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any


WATCH_ROOT = pathlib.Path(".github/vellum-expansion-watch")
ACCEPTANCE_PATH = WATCH_ROOT / "full-design-import-render-v1/acceptance.json"
EXACT_BOUNDARY_ACCEPTANCE_PATH = WATCH_ROOT / (
    "full-design-import-render-v1/exact-boundary-acceptance-1.json"
)
README_PATH = WATCH_ROOT / "README.md"
EVENT_ROOT = pathlib.Path(".github/vellum-expansion-watch-events")
EXPECTED_ACCEPTANCE_SHA256 = "1535d76e34dfc80eda55247c1b0d47b9f47b3e58a08b6f1ab4b749692e6056fd"
EXPECTED_README_SHA256 = "1626043485af997f94c4ca8221c936ef5e6f72117b62d988f994e8829e5cb8ef"
EXPECTED_ACCEPTED_AT = "2026-08-11T07:11:14Z"
SHA40 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
EVENT_ID = re.compile(r"^20[0-9]{6}-[a-z0-9]+(?:-[a-z0-9]+)*$")
EXPECTED_COUNTERPART = {
    "proposal_id": "full-design-import-render-v1",
    "repository": "danielraffel/vellum",
    "merge_commit": "bf0559eca2547f9242405cc388888890e80bbd87",
    "merge_parents": [
        "e8c07675332b047c9b3a4f357aebb077cc52a8b6",
        "7f451feaf42c3bd047a038eaae78c455ff0d8780",
    ],
    "proposal_path": (
        "provenance/authority/expansions/"
        "full-design-import-render-v1/proposal.json"
    ),
    "proposal_sha256": (
        "7c2db05e110e7d9834806e08469f6d7cc70f6528b2242d1f6c0255dcdbc0a4c9"
    ),
    "scope_addendum": {
        "repository": "danielraffel/vellum",
        "merge_commit": "8608fac0922577f9f6e87f51b3e6b9eed395d243",
        "merge_parents": [
            "4f654343d97118781fdf7edcf7b5ed2c8bed7a5d",
            "63e800100bc1e684b3f9e4ba7d6b5413703b185b",
        ],
        "addendum_path": (
            "provenance/authority/expansions/full-design-import-render-v1/"
            "scope-addendum-1.json"
        ),
        "addendum_sha256": (
            "91bb269ce5a872037fd67e4735125772bcac50d82cf454edfd8c356b11f5a122"
        ),
    },
}
EXPECTED_PROPOSAL_SCOPES = {
    "design-source-ingest": [
        "tools/import-design/**",
        "tools/cli/**/import_design*",
        "tools/cli/**/*design_import*",
        "tools/mcp/**/*design*",
        ".agents/skills/import-design/**",
    ],
    "chromium-authoring-frontend": [
        "tools/import-design/**/*chrom*",
        "tools/import-design/**/*browser*",
        "tools/cli/**/*chrome*",
        "tools/cli/**/*browser*",
        "tools/scripts/**/*chrome*",
        "tools/scripts/**/*browser*",
    ],
    "design-ir-contract": [
        "core/view/include/pulp/view/design_*.hpp",
        "core/view/include/pulp/view/anchor_strategy.hpp",
        "core/view/src/design_*.cpp",
        "core/view/src/design_*.hpp",
        "core/view/src/anchor_strategy.cpp",
        "packages/pulp-import-ir/**",
    ],
    "render-assets-and-backends": [
        "core/canvas/**",
        "core/render/**",
        "core/view/**/*render*",
        "core/view/**/*skia*",
        "core/view/**/*dawn*",
    ],
    "visual-proof-harness": [
        "core/view/include/pulp/view/screenshot*.hpp",
        "core/view/src/screenshot*.cpp",
        "core/view/platform/**/*capture*",
        "core/view/platform/**/*screenshot*",
        "tools/**/*screenshot*",
        "tools/**/*visual*",
        "tools/**/*golden*",
    ],
    "design-output-and-packaging": [
        "tools/import-design/**",
        "tools/cli/**/import_design*",
        "tools/mcp/**/*design*",
        "templates/**/*design*",
        "docs/**/*design-import*",
    ],
}
EXPECTED_SCOPE_ADDITIONS = {
    "design-source-ingest": [
        ".claude/commands/design.md",
        ".claude/commands/import-design.md",
        "compat/imports.json",
        "compat/rn.json",
        "design/**",
        "experimental/pulp-rs/src/cmd/design.rs",
        "experimental/pulp-rs/src/cmd/mod.rs",
        "experimental/pulp-rs/src/main.rs",
        "tools/figma-plugin/**",
        "tools/figma-import/**",
        "tools/cli/cmd_design*",
        "tools/cli/cmd_import*",
        "tools/cli/design_binding*",
        "tools/cli/import_*",
        "tools/cli/importer_*",
        "tools/design/**",
        "tools/design-ab/**",
        "examples/design-tool/**",
        "examples/design*/**",
        "test/cmake/cli_import_tool_tests.cmake",
        "test/cmake/design_import*.cmake",
        "test/fixtures/imports/**",
        "test/fixtures/figma/**",
        "test/fixtures/pencil/**",
        "test/fixtures/rn/**",
        "test/fixtures/stitch/**",
        "test/fixtures/v0-dev/**",
        "test/test_cli_import*",
        "test/test_cli_design*",
        "test/test_design_*",
        "test/test_design_import*",
        "test/test_figma_*",
        "test/test_import*",
        "docs/guides/design-*",
        "docs/guides/figma-plugin.md",
        "docs/guides/importing-designs.md",
        "docs/reference/compat/imports.md",
        "docs/reference/compat/rn.md",
        "docs/reference/design-*",
        "docs/reference/design-import*",
        "docs/reference/design-ir*",
        "docs/reference/imports/**",
    ],
    "chromium-authoring-frontend": [
        "test/fixtures/agent-panels/**",
        "test/fixtures/browser-capture-*/**",
        "test/fixtures/browser_capture_*",
        "test/fixtures/import-differential/**",
        "test/test_browser_capture*",
        "test/test_browser_import*",
        "test/test_design_browser_capture.cpp",
        "test/test_html_intake.cpp",
        "test/test_html_project_stager.cpp",
        "experimental/pulp-rs/src/cmd/chrome_for_testing.rs",
        "experimental/pulp-rs/src/install_import_design.rs",
        "experimental/pulp-rs/tests/chrome_for_testing_tool_test.rs",
        "tools/packages/chrome-for-testing-verification.md",
    ],
    "design-ir-contract": [
        "core/view/include/pulp/view/jsx_lock.hpp",
        "core/view/include/pulp/view/lock_to_source.hpp",
        "core/view/include/pulp/view/recognition_resolver.hpp",
        "core/view/include/pulp/view/token_lock.hpp",
        "core/view/src/claude_bundle*",
        "core/view/src/jsx_lock.cpp",
        "core/view/src/lock_to_source.cpp",
        "core/view/src/recognition_resolver.cpp",
        "core/view/src/token_lock.cpp",
        "core/view/src/widget_bridge/runtime_import_api*",
        "test/fixtures/design_import_*",
        "test/test_design_ir*",
        "test/test_jsx_lock.cpp",
        "test/test_lock_to_source.cpp",
        "test/test_recognition_resolver.cpp",
        "test/test_token_lock.cpp",
        "test/test_widget_bridge_runtime_import.cpp",
    ],
    "render-assets-and-backends": [
        ".agents/skills/skia-gpu-build/**",
        ".github/workflows/non-skia-build-guard.yml",
        "assets/design-system/**",
        "core/view/**",
        "packages/pulp-react/**",
        "test/cmake/*skia*",
        "test/test_skia*",
        "tools/build-skia*",
        "tools/cmake/FindSkia.cmake",
        "tools/scripts/*skia*",
    ],
    "visual-proof-harness": [
        "tools/import-validation/**",
        "tools/harness/**",
        "tools/scripts/figma_import_diff.py",
        "tools/scripts/render-figma-import.sh",
        "tools/local-ci/*capture*",
        "tools/local-ci/*design*",
        "test/harness/**",
        "test/visual/**",
        "test/fixtures/import-fidelity/**",
        "test/cmake/view_widget_bridge_tests.cmake",
        "test/fixtures/fake_screenshot_tool.cpp",
        "test/test_*screenshot*",
        "test/test_screenshot*",
        "test/web-compat/test_screenshot*",
        "examples/ui-preview/**",
        ".github/workflows/visual-harness.yml",
        "ci/visual-harness.Dockerfile",
        "compat.json",
        "external/fonts/**",
        "external/skia-build/**",
        "docs/examples/screenshots.md",
        "docs/reports/harness-coverage.md",
    ],
    "design-output-and-packaging": [
        "tools/import-validation/**",
        "tools/templates/from-figma/**",
        "tools/templates/from-v0/**",
        "templates/swiftui-design-host/**",
        "tools/scripts/check_import_provenance.py",
        "tools/scripts/design_import_benchmark.py",
        "tools/scripts/test_design_import_benchmark.py",
        "tools/scripts/package_cli.py",
        "tools/scripts/test_package_cli.py",
        "tools/packages/test_design_controls.py",
        "tools/rack/export_design_data.py",
        "experimental/pulp-rs/src/install_import_design.rs",
        ".github/workflows/release-cli.yml",
        ".github/workflows/release-dry-run.yml",
        "test/cmake/test_installed_sdk_runtime_staging.cmake",
        "docs/status/cli-commands.yaml",
        "docs/status/tools.yaml",
        "docs/tools/importer-differential-lab.md",
    ],
}
EXPECTED_SCOPES = {
    family: [*selectors, *EXPECTED_SCOPE_ADDITIONS[family]]
    for family, selectors in EXPECTED_PROPOSAL_SCOPES.items()
}
EXPECTED_RETAINED = [
    "pulp-audio-and-dsp-harness",
    "pulp-public-cli-and-control-broker",
    "pulp-product-integration",
    "pulp-runtime-engine-selection",
]
EXPECTED_GATES = {
    "watch_acceptance_may_transfer_authority": False,
    "source_work_authorized": False,
    "pulp_consumption_authorized": False,
    "vellum_watch_acknowledgement_required": True,
    "exact_boundary_acknowledgement_required": True,
}


class WatchError(RuntimeError):
    pass


class DuplicateKeyError(ValueError):
    pass


def _pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in items:
        if key in result:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _load_bytes(raw: bytes, where: str) -> Any:
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs)
    except (UnicodeError, json.JSONDecodeError, DuplicateKeyError) as exc:
        raise WatchError(f"{where}: invalid strict JSON: {exc}") from exc


def _exact_keys(value: Any, expected: set[str], where: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        actual = set(value) if isinstance(value, dict) else set()
        raise WatchError(
            f"{where}: keys differ; missing={sorted(expected - actual)} "
            f"unexpected={sorted(actual - expected)}"
        )


def _strings(value: Any, where: str) -> list[str]:
    if (
        not isinstance(value, list)
        or not value
        or not all(isinstance(item, str) and item for item in value)
        or len(value) != len(set(value))
    ):
        raise WatchError(f"{where}: expected unique non-empty strings")
    return value


def _utc(value: Any, where: str) -> None:
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (AttributeError, TypeError, ValueError) as exc:
        raise WatchError(f"{where}: expected UTC timestamp ending in Z") from exc
    if not value.endswith("Z") or parsed.utcoffset() != dt.timedelta(0):
        raise WatchError(f"{where}: expected UTC timestamp ending in Z")


def validate_acceptance(value: Any) -> dict[str, list[str]]:
    top = {
        "schema_version",
        "kind",
        "acceptance_id",
        "state",
        "accepted_at",
        "accepted_by",
        "pulp_repository",
        "pulp_baseline_commit",
        "counterpart",
        "watch_scope",
        "retained_boundaries",
        "interim_maintenance",
        "watch_event_directory",
        "authority_effect",
        "implementation_authority",
        "gates",
    }
    _exact_keys(value, top, "acceptance")
    expected_scalars = {
        "schema_version": 1,
        "kind": "authority-expansion-watch-acceptance",
        "acceptance_id": "full-design-import-render-v1-pulp-watch",
        "state": "accepted",
        "accepted_at": EXPECTED_ACCEPTED_AT,
        "accepted_by": "@danielraffel",
        "pulp_repository": "Generous-Corp/pulp",
        "pulp_baseline_commit": "190c463a0b320f28420c9af177244af04ef84233",
        "watch_event_directory": EVENT_ROOT.as_posix(),
        "authority_effect": "none",
        "implementation_authority": (
            "forbidden-until-exact-boundary-acknowledged"
        ),
    }
    for key, expected in expected_scalars.items():
        if type(value[key]) is not type(expected) or value[key] != expected:
            raise WatchError(f"acceptance.{key}: differs from pinned value")
    _utc(value["accepted_at"], "acceptance.accepted_at")
    if not SHA40.fullmatch(value["pulp_baseline_commit"]):
        raise WatchError("acceptance.pulp_baseline_commit: expected full SHA")
    if value["counterpart"] != EXPECTED_COUNTERPART:
        raise WatchError("acceptance.counterpart: differs from exact Vellum proposal")
    scopes: dict[str, list[str]] = {}
    if not isinstance(value["watch_scope"], list):
        raise WatchError("acceptance.watch_scope: expected array")
    for index, item in enumerate(value["watch_scope"]):
        where = f"acceptance.watch_scope[{index}]"
        _exact_keys(item, {"id", "pulp_selectors"}, where)
        family = item["id"]
        if not isinstance(family, str) or family in scopes:
            raise WatchError(f"{where}.id: missing or duplicate")
        selectors = _strings(item["pulp_selectors"], f"{where}.pulp_selectors")
        scopes[family] = selectors
    if scopes != EXPECTED_SCOPES:
        raise WatchError("acceptance.watch_scope: differs from pinned selectors")
    if value["retained_boundaries"] != EXPECTED_RETAINED:
        raise WatchError("acceptance.retained_boundaries: differs from proposal")
    maintenance = value["interim_maintenance"]
    expected_maintenance = [{
        "id": "capture-primitives-unimplemented-in-vellum",
        "status": "pending-vellum-watch-acknowledgement",
        "expires_at_gate": "5A-P.3-independent-pixel-and-semantic-proof",
    }]
    if maintenance != expected_maintenance:
        raise WatchError("acceptance.interim_maintenance: differs from proposal")
    if value["gates"] != EXPECTED_GATES:
        raise WatchError("acceptance.gates: authority or consumption gate differs")
    return scopes


def _watch_files(root: pathlib.Path) -> set[str]:
    base = root / WATCH_ROOT
    if not base.exists():
        return set()
    if base.is_symlink():
        raise WatchError(f"{WATCH_ROOT}: must not be a symlink")
    files: set[str] = set()
    for path in base.rglob("*"):
        relative = path.relative_to(base).as_posix()
        if path.is_symlink():
            raise WatchError(f"{WATCH_ROOT / relative}: must not be a symlink")
        if path.is_file():
            files.add(relative)
    return files


def load_acceptance(root: pathlib.Path) -> tuple[dict[str, list[str]] | None, bytes | None]:
    files = _watch_files(root)
    if not files:
        return None, None
    expected = {
        README_PATH.relative_to(WATCH_ROOT).as_posix(),
        ACCEPTANCE_PATH.relative_to(WATCH_ROOT).as_posix(),
    }
    exact_relative = EXACT_BOUNDARY_ACCEPTANCE_PATH.relative_to(WATCH_ROOT).as_posix()
    if exact_relative in files:
        expected.add(exact_relative)
    if files != expected:
        raise WatchError(
            f"{WATCH_ROOT}: artifact set differs; expected={sorted(expected)} "
            f"actual={sorted(files)}"
        )
    readme = (root / README_PATH).read_bytes()
    raw = (root / ACCEPTANCE_PATH).read_bytes()
    if hashlib.sha256(readme).hexdigest() != EXPECTED_README_SHA256:
        raise WatchError(f"{README_PATH}: immutable bytes differ")
    if hashlib.sha256(raw).hexdigest() != EXPECTED_ACCEPTANCE_SHA256:
        raise WatchError(f"{ACCEPTANCE_PATH}: immutable bytes differ")
    return validate_acceptance(_load_bytes(raw, ACCEPTANCE_PATH.as_posix())), raw


def _git(root: pathlib.Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    completed = subprocess.run(
        ["git", "-C", str(root), *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check and completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise WatchError(f"git {' '.join(args)} failed: {detail}")
    return completed


def _git_blob(root: pathlib.Path, commit: str, path: pathlib.Path) -> bytes | None:
    completed = _git(root, "show", f"{commit}:{path.as_posix()}", check=False)
    if completed.returncode == 0:
        return completed.stdout
    return None


def _changes(root: pathlib.Path, base: str, head: str) -> list[tuple[str, str]]:
    raw = _git(
        root,
        "diff",
        "--name-status",
        "-z",
        "--no-renames",
        base,
        head,
        "--",
    ).stdout.split(b"\0")
    values = [item.decode("utf-8", errors="surrogateescape") for item in raw if item]
    if len(values) % 2:
        raise WatchError("git diff returned a truncated name-status record")
    return [(values[i], values[i + 1]) for i in range(0, len(values), 2)]


def _matches(path: str, selector: str) -> bool:
    candidates = {selector}
    pending = [selector]
    while pending:
        candidate = pending.pop()
        marker = candidate.find("**/")
        if marker >= 0:
            collapsed = candidate[:marker] + candidate[marker + 3 :]
            if collapsed not in candidates:
                candidates.add(collapsed)
                pending.append(collapsed)
    return any(fnmatch.fnmatchcase(path, candidate) for candidate in candidates)


def _affected(scopes: dict[str, list[str]], paths: set[str]) -> set[str]:
    return {
        family
        for family, selectors in scopes.items()
        if any(_matches(path, selector) for path in paths for selector in selectors)
    }


def validate_event(value: Any, filename: str) -> set[str]:
    common_fields = {
        "schema_version",
        "kind",
        "event_id",
        "created_at",
        "acceptance_id",
        "acceptance_sha256",
        "capability_families",
        "rationale",
        "tests",
        "disposition",
        "authority_effect",
    }
    kind = value.get("kind") if isinstance(value, dict) else None
    if kind not in {
        "authority-expansion-watch-change",
        "authority-expansion-watch-refresh",
    }:
        raise WatchError(f"{filename}: kind differs from watch contract")
    fields = set(common_fields)
    if kind == "authority-expansion-watch-refresh":
        fields.add("refresh")
    _exact_keys(value, fields, filename)
    if value["schema_version"] != 1 or type(value["schema_version"]) is not int:
        raise WatchError(f"{filename}: schema_version must be integer 1")
    expected = {
        "kind": kind,
        "acceptance_id": "full-design-import-render-v1-pulp-watch",
        "acceptance_sha256": EXPECTED_ACCEPTANCE_SHA256,
        "disposition": "watch-only-no-authority",
        "authority_effect": "none",
    }
    for key, wanted in expected.items():
        if value[key] != wanted or type(value[key]) is not type(wanted):
            raise WatchError(f"{filename}: {key} differs from watch contract")
    event_id = value["event_id"]
    if not isinstance(event_id, str) or not EVENT_ID.fullmatch(event_id):
        raise WatchError(f"{filename}: invalid event_id")
    if pathlib.Path(filename).name != f"{event_id}.json":
        raise WatchError(f"{filename}: filename must match event_id")
    _utc(value["created_at"], f"{filename}.created_at")
    if not isinstance(value["rationale"], str) or len(value["rationale"].strip()) < 24:
        raise WatchError(f"{filename}: rationale is too short")
    _strings(value["tests"], f"{filename}.tests")
    families = value["capability_families"]
    if kind == "authority-expansion-watch-refresh":
        if families != []:
            raise WatchError(f"{filename}: refresh events cannot claim capability families")
        refresh = value["refresh"]
        _exact_keys(
            refresh,
            {
                "audited_at",
                "pulp_main_commit",
                "open_pr_audit_complete",
                "open_pr_rows",
                "open_vellum_overlap_count",
            },
            f"{filename}.refresh",
        )
        _utc(refresh["audited_at"], f"{filename}.refresh.audited_at")
        if not isinstance(refresh["pulp_main_commit"], str) or not SHA40.fullmatch(
            refresh["pulp_main_commit"]
        ):
            raise WatchError(f"{filename}.refresh.pulp_main_commit: expected full SHA")
        if refresh["open_pr_audit_complete"] is not True:
            raise WatchError(f"{filename}.refresh.open_pr_audit_complete: expected true")
        if refresh["open_pr_rows"] != []:
            raise WatchError(f"{filename}.refresh.open_pr_rows: expected empty audit")
        if (
            type(refresh["open_vellum_overlap_count"]) is not int
            or refresh["open_vellum_overlap_count"] != 0
        ):
            raise WatchError(
                f"{filename}.refresh.open_vellum_overlap_count: expected zero"
            )
        return set()
    families = _strings(families, f"{filename}.capability_families")
    if families != sorted(families) or not set(families) <= set(EXPECTED_SCOPES):
        raise WatchError(f"{filename}: capability_families must be sorted and known")
    return set(families)


def verify(root: pathlib.Path, base: str | None = None, head: str | None = None) -> dict[str, Any]:
    affected: set[str] = set()
    events: list[str] = []
    try:
        scopes, raw = load_acceptance(root)
        if (base is None) != (head is None):
            raise WatchError("base and head must be supplied together")
        if base is not None and head is not None:
            for commit, label in ((base, "base"), (head, "head")):
                if not SHA40.fullmatch(commit):
                    raise WatchError(f"{label}: expected full commit SHA")
            base_raw = _git_blob(root, base, ACCEPTANCE_PATH)
            if base_raw is not None and raw != base_raw:
                raise WatchError(f"{ACCEPTANCE_PATH}: accepted bytes are immutable")
            if base_raw is not None and scopes is None:
                raise WatchError(f"{ACCEPTANCE_PATH}: accepted artifact cannot be removed")
            changes = _changes(root, base, head)
            changed_paths = {
                path for _, path in changes if not path.startswith(EVENT_ROOT.as_posix() + "/")
            }
            event_changes = [
                (status, path)
                for status, path in changes
                if path.startswith(EVENT_ROOT.as_posix() + "/")
            ]
            if scopes is None:
                if event_changes:
                    raise WatchError("watch events require the installed acceptance")
            else:
                affected = _affected(scopes, changed_paths)
                covered: set[str] = set()
                for status, path in event_changes:
                    if status != "A":
                        raise WatchError(f"{path}: watch events are append-only")
                    if pathlib.PurePosixPath(path).parent != EVENT_ROOT:
                        raise WatchError(f"{path}: watch events must be direct children")
                    file_path = root / path
                    if file_path.is_symlink() or not file_path.is_file():
                        raise WatchError(f"{path}: event must be a regular file")
                    claims = validate_event(_load_bytes(file_path.read_bytes(), path), path)
                    duplicate_claims = covered & claims
                    if duplicate_claims:
                        raise WatchError(
                            f"{path}: duplicate capability-family claims "
                            f"{sorted(duplicate_claims)}"
                        )
                    covered.update(claims)
                    events.append(path)
                if covered != affected:
                    raise WatchError(
                        "watch event family coverage differs; "
                        f"affected={sorted(affected)} covered={sorted(covered)}"
                    )
        return {
            "status": "pass",
            "state": "active" if scopes is not None else "inactive",
            "acceptance_id": (
                "full-design-import-render-v1-pulp-watch" if scopes is not None else None
            ),
            "acceptance_sha256": EXPECTED_ACCEPTANCE_SHA256 if scopes is not None else None,
            "authority_effect": "none",
            "affected_families": sorted(affected),
            "events": sorted(events),
            "errors": [],
        }
    except (OSError, WatchError) as exc:
        return {
            "status": "fail",
            "state": None,
            "acceptance_id": None,
            "acceptance_sha256": None,
            "authority_effect": None,
            "affected_families": [],
            "events": [],
            "errors": [str(exc)],
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--base")
    parser.add_argument("--head")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    report = verify(args.repo.resolve(), args.base, args.head)
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
