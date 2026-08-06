#!/usr/bin/env python3
"""Negative, evolution, surface, and compatibility contract tests."""
from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

import agent_capability_manifest as manifest
import agent_capability_surface as surface


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/scripts/agent_capability_manifest.py"


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        cwd=ROOT,
        text=True,
        capture_output=True,
    )


def expect_validation_failure(document: dict, needle: str) -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-agent-capability-fixture-") as temp:
        path = pathlib.Path(temp) / "fixture.json"
        path.write_text(json.dumps(document, indent=2) + "\n")
        result = run("--validate", str(path))
    assert result.returncode != 0, f"fixture unexpectedly passed: {needle}"
    assert needle in result.stderr, f"expected {needle!r} in {result.stderr!r}"


def refresh_digest(row: dict) -> None:
    row["contract_digest"] = surface.canonical_digest(manifest.contract_payload(row))


def expect_problem(problems: list[str], needle: str) -> None:
    joined = "\n".join(problems)
    assert needle in joined, f"expected {needle!r} in {joined!r}"


def exercise_manifest_mutations(canonical: dict) -> int:
    checks = 0

    duplicate = copy.deepcopy(canonical)
    duplicate["capabilities"].append(copy.deepcopy(duplicate["capabilities"][0]))
    expect_validation_failure(duplicate, "duplicate capability key")
    checks += 1

    missing_binding = copy.deepcopy(canonical)
    missing_binding["capabilities"][0]["bindings"][0]["qualified_name"] += "Missing"
    refresh_digest(missing_binding["capabilities"][0])
    expect_validation_failure(missing_binding, "outside curated exports")
    checks += 1

    omitted_binding = copy.deepcopy(canonical)
    swing = next(
        row for row in omitted_binding["capabilities"] if row["key"] == "timebase.swing"
    )
    swing["bindings"].pop()
    refresh_digest(swing)
    expect_validation_failure(omitted_binding, "bindings must exactly match curated exports")
    checks += 1

    omitted_capability = copy.deepcopy(canonical)
    omitted_capability["capabilities"].pop()
    expect_validation_failure(
        omitted_capability, "capability keys must exactly match curated exports"
    )
    checks += 1

    missing_descriptor = copy.deepcopy(canonical)
    saturator = next(
        row for row in missing_descriptor["capabilities"] if row["key"] == "signal.saturator"
    )
    saturator["forge_descriptor"]["node_key"] = "not-a-node"
    refresh_digest(saturator)
    expect_validation_failure(missing_descriptor, "references missing Forge descriptor")
    checks += 1

    wrong_type = copy.deepcopy(canonical)
    wrong_type["capabilities"][0]["summary"] = 42
    expect_validation_failure(wrong_type, "expected type string")
    checks += 1

    wrong_key_type = copy.deepcopy(canonical)
    wrong_key_type["capabilities"][0]["key"] = []
    refresh_digest(wrong_key_type["capabilities"][0])
    expect_validation_failure(wrong_key_type, "expected type string")
    checks += 1

    nested_wrong_type = copy.deepcopy(canonical)
    nested_wrong_type["capabilities"][0]["units"] = ["frames", 4]
    expect_validation_failure(nested_wrong_type, "expected type string")
    checks += 1

    count_drift = copy.deepcopy(canonical)
    count_drift["counts"]["total"] += 1
    expect_validation_failure(count_drift, "counts must exactly match")
    checks += 1

    unknown_field = copy.deepcopy(canonical)
    unknown_field["capabilities"][0]["schedulling"] = "typo"
    expect_validation_failure(unknown_field, "unexpected property 'schedulling'")
    checks += 1

    runtime_control = copy.deepcopy(canonical)
    runtime_control["capabilities"][0]["operation_id"] = "runtime-op"
    expect_validation_failure(runtime_control, "contains runtime control field 'operation_id'")
    checks += 1

    numeric_contract = copy.deepcopy(canonical)
    numeric_contract["capabilities"][0]["min"] = 0
    expect_validation_failure(numeric_contract, "duplicates Forge numeric contract field 'min'")
    checks += 1

    wrong_digest = copy.deepcopy(canonical)
    wrong_digest["capabilities"][0]["contract_digest"] = "sha256:" + "0" * 64
    expect_validation_failure(wrong_digest, "contract_digest does not match")
    checks += 1

    wrong_absence = copy.deepcopy(canonical)
    wrong_absence["coverage"]["absence_semantics"] = "unsupported"
    expect_validation_failure(wrong_absence, "expected const 'unknown'")
    checks += 1

    return checks


def exercise_evolution(canonical: dict) -> int:
    checks = 0

    changed = copy.deepcopy(canonical)
    changed["manifest_revision"] += 1
    changed["capabilities"][0]["scheduling"] = "changed-contract"
    refresh_digest(changed["capabilities"][0])
    expect_problem(
        manifest.evolution_problems(
            canonical, changed, allow_unpublished_migration=False
        ),
        "changed without a contract_version increase",
    )
    checks += 1

    breaking = copy.deepcopy(canonical)
    breaking["manifest_revision"] += 1
    row = breaking["capabilities"][0]
    row["bindings"][0]["qualified_name"] += "Replacement"
    row["contract_version"]["minor"] += 1
    refresh_digest(row)
    expect_problem(
        manifest.evolution_problems(
            canonical, breaking, allow_unpublished_migration=False
        ),
        "breaking change without a major increase",
    )
    checks += 1

    removed = copy.deepcopy(canonical)
    removed["manifest_revision"] += 1
    removed["capabilities"] = removed["capabilities"][1:]
    expect_problem(
        manifest.evolution_problems(
            canonical, removed, allow_unpublished_migration=False
        ),
        "removed capability lacks a tombstone",
    )
    checks += 1

    added = copy.deepcopy(canonical)
    added["manifest_revision"] += 1
    new_row = copy.deepcopy(added["capabilities"][0])
    new_row["key"] = "audio.new-capability"
    new_row["contract_version"] = {"major": 2, "minor": 0}
    refresh_digest(new_row)
    added["capabilities"].append(new_row)
    expect_problem(
        manifest.evolution_problems(
            canonical, added, allow_unpublished_migration=False
        ),
        "new capability must start at contract version 1.0",
    )
    checks += 1

    orphan = copy.deepcopy(canonical)
    orphan["manifest_revision"] += 1
    orphan["tombstones"].append(
        {
            "key": "audio.never-existed",
            "last_contract_version": {"major": 1, "minor": 0},
            "last_contract_digest": "sha256:" + "0" * 64,
            "removed_in_manifest_revision": orphan["manifest_revision"],
            "reason": "This synthetic key never existed in the prior manifest.",
            "replacement_key": None,
        }
    )
    expect_problem(
        manifest.evolution_problems(
            canonical, orphan, allow_unpublished_migration=False
        ),
        "capability tombstone has no removed prior key",
    )
    checks += 1

    return checks


def exercise_surface_mutations() -> int:
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pulp-agent-surface-") as temp:
        root = pathlib.Path(temp)
        directory = root / "core/signal/include/pulp/signal"
        directory.mkdir(parents=True)
        legacy = directory / "legacy.hpp"
        legacy.write_text("#pragma once\nstruct Existing {};\n")
        baseline = surface.baseline_document(root, [])
        baseline_digest = baseline["entries_digest"]

        added = directory / "new_algorithm.hpp"
        added.write_text("#pragma once\nstruct NewAlgorithm {};\n")
        _, problems = surface.build_surface_document(
            root,
            binding_claims=[],
            reviewed_headers=[],
            tombstones=[],
            baseline=baseline,
            expected_baseline_digest=baseline_digest,
        )
        expect_problem(problems, "unclassified public header: pulp/signal/new_algorithm.hpp")
        checks += 1
        added.unlink()

        legacy.write_text("#pragma once\nstruct Existing {};\nstruct AddedSymbol {};\n")
        _, problems = surface.build_surface_document(
            root,
            binding_claims=[],
            reviewed_headers=[],
            tombstones=[],
            baseline=baseline,
            expected_baseline_digest=baseline_digest,
        )
        expect_problem(problems, "public header fingerprint changed")
        checks += 1

        legacy.unlink()
        _, problems = surface.build_surface_document(
            root,
            binding_claims=[],
            reviewed_headers=[],
            tombstones=[],
            baseline=baseline,
            expected_baseline_digest=baseline_digest,
        )
        expect_problem(problems, "removed without a surface tombstone")
        checks += 1

        grown = copy.deepcopy(baseline)
        grown["entries"].append(
            {"include": "pulp/signal/added.hpp", "fingerprint": "sha256:" + "0" * 64}
        )
        grown["frozen_count"] = len(grown["entries"])
        grown["entries_digest"] = surface.canonical_digest(grown["entries"])
        expect_problem(
            surface.validate_baseline(grown, baseline_digest),
            "legacy baseline digest changed",
        )
        checks += 1

    previous_surface = {
        "schema": surface.SURFACE_SCHEMA,
        "inventory_version": 1,
        "headers": [
            {
                "include": "pulp/signal/reviewed.hpp",
                "fingerprint": "sha256:" + "1" * 64,
                "disposition": "capability_support",
            }
        ],
        "tombstones": [],
    }
    removed_without_tombstone = {
        "schema": surface.SURFACE_SCHEMA,
        "inventory_version": 2,
        "headers": [],
        "tombstones": [],
    }
    expect_problem(
        manifest.surface_evolution_problems(
            previous_surface,
            removed_without_tombstone,
            surface.SURFACE_SCHEMA,
        ),
        "removed reviewed header lacks a tombstone",
    )
    checks += 1

    orphan_surface_tombstone = copy.deepcopy(previous_surface)
    orphan_surface_tombstone["inventory_version"] = 2
    orphan_surface_tombstone["tombstones"] = [
        {
            "include": "pulp/signal/never-existed.hpp",
            "last_fingerprint": "sha256:" + "2" * 64,
            "removed_in_inventory_version": 2,
            "reason": "This synthetic header never existed in the prior ledger.",
        }
    ]
    expect_problem(
        manifest.surface_evolution_problems(
            previous_surface,
            orphan_surface_tombstone,
            surface.SURFACE_SCHEMA,
        ),
        "surface tombstone has no removed prior header",
    )
    checks += 1

    return checks


def exercise_compatibility(canonical: dict) -> int:
    vocabulary_tool = ROOT / "tools/dsp_vocabulary.py"
    spec = importlib.util.spec_from_file_location(
        "pulp_dsp_vocabulary_test", vocabulary_tool
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    header_projection = module.scan_headers()
    expected_json = json.dumps(header_projection, indent=2) + "\n"
    legacy = subprocess.run(
        [sys.executable, str(vocabulary_tool), "--json"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    )
    assert legacy.stdout == expected_json, "legacy JSON output bypassed the manifest"
    legacy_markdown = subprocess.run(
        [sys.executable, str(vocabulary_tool)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    )
    assert legacy_markdown.stdout == module.markdown(header_projection) + "\n"
    vocabulary = json.loads(legacy.stdout)
    manifest_projection = canonical["compatibility"]["signal_vocabulary"]["entries"]
    assert vocabulary == manifest_projection == header_projection
    advertised_signal = [
        row for row in canonical["capabilities"] if row["domain"] == "signal"
    ]
    for row in advertised_signal:
        for item in row["bindings"]:
            if not item["include"].startswith("pulp/signal/"):
                continue
            relative = item["include"].removeprefix("pulp/signal/")
            assert relative in vocabulary, f"legacy vocabulary lost {relative}"
            class_name = item["qualified_name"].split("::")[-1].split("<", 1)[0]
            assert any(entry["class"] == class_name for entry in vocabulary[relative])
    return 2


def main() -> int:
    result = run("--json")
    assert result.returncode == 0, result.stderr
    canonical = json.loads(result.stdout)
    checks = exercise_manifest_mutations(canonical)
    checks += exercise_evolution(canonical)
    checks += exercise_surface_mutations()

    with tempfile.TemporaryDirectory(prefix="pulp-agent-stale-") as temp:
        stale = pathlib.Path(temp) / "agent-capabilities.json"
        stale.write_text(json.dumps({**canonical, "capabilities": canonical["capabilities"][:-1]}))
        result = run("--check", "--snapshot", str(stale))
        assert result.returncode != 0 and "STALE" in result.stderr, result.stderr
        checks += 1

    checks += exercise_compatibility(canonical)
    print(f"agent-capabilities: {checks} negative/evolution/surface/compatibility checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
