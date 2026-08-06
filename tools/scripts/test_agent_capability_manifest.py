#!/usr/bin/env python3
"""Negative, evolution, surface, and compatibility contract tests."""
from __future__ import annotations

import copy
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile

import agent_capability_manifest as manifest
import agent_capability_surface as surface
import json_schema_lite


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


def expect_schema_failure(document: dict, needle: str) -> None:
    schema = json.loads((ROOT / manifest.MANIFEST_SCHEMA_FILE).read_text())
    problems = json_schema_lite.validate(document, schema)
    assert problems, "fixture unexpectedly passed the public schema"
    assert needle in "\n".join(problems), problems


def refresh_digest(row: dict) -> None:
    row["contract_digest"] = surface.canonical_digest(manifest.contract_payload(row))


def expect_problem(problems: list[str], needle: str) -> None:
    joined = "\n".join(problems)
    assert needle in joined, f"expected {needle!r} in {joined!r}"


def exercise_manifest_mutations(canonical: dict) -> int:
    checks = 0

    for document, hidden_schema in (
        ({}, {"properties": {"absent": {"unsupportedKeyword": True}}}),
        ([], {"items": {"unsupportedKeyword": True}}),
        ({}, {"additionalProperties": {"unsupportedKeyword": True}}),
    ):
        try:
            json_schema_lite.validate(document, hidden_schema)
        except json_schema_lite.UnsupportedKeyword:
            checks += 1
        else:
            raise AssertionError("unsupported keyword escaped schema preflight")

    assert manifest._minimal_target_for_include("pulp/signal/fft_backend.hpp") == (
        "Pulp::signal-fft-backend"
    )
    checks += 1
    assert manifest._minimal_target_for_include("pulp/signal/modal_spec.hpp") == (
        "Pulp::signal-modal-spec"
    )
    checks += 1
    assert manifest._minimal_target_for_include("pulp/signal/sos_cascade.hpp") == (
        "Pulp::signal"
    )
    checks += 1
    for include in (
        "pulp/timebase/beat_division.hpp",
        "pulp/timebase/coordinate_random.hpp",
        "pulp/timebase/grid_projection.hpp",
        "pulp/timebase/groove_kernel.hpp",
        "pulp/timebase/ratchet.hpp",
        "pulp/timebase/trigger_grid.hpp",
    ):
        assert manifest._minimal_target_for_include(include) == "Pulp::timebase"
        checks += 1
    expected_timebase_keys = {
        "timebase.beat-division",
        "timebase.coordinate-random",
        "timebase.grid-projection",
        "timebase.groove-kernel",
        "timebase.ratchet",
        "timebase.swing",
        "timebase.tick",
        "timebase.trigger-grid",
    }
    assert {
        row["key"] for row in canonical["capabilities"] if row["domain"] == "timebase"
    } == expected_timebase_keys
    checks += 1
    assert manifest._minimal_target_for_include("pulp/signal/future_source_api.hpp") is None
    checks += 1

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

    reversed_enum = copy.deepcopy(canonical)
    enum_pair = next(
        pair
        for classes in reversed_enum["compatibility"]["signal_vocabulary"]["entries"].values()
        for class_row in classes
        for pair in class_row["enums"]
    )
    enum_pair.reverse()
    expect_schema_failure(reversed_enum, "expected type")
    checks += 1

    scalar_enum_values = copy.deepcopy(canonical)
    enum_pair = next(
        pair
        for classes in scalar_enum_values["compatibility"]["signal_vocabulary"]["entries"].values()
        for class_row in classes
        for pair in class_row["enums"]
    )
    enum_pair[1] = enum_pair[1][0]
    expect_schema_failure(scalar_enum_values, "expected type array")
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

    missing_determinism = copy.deepcopy(canonical)
    missing_determinism["capabilities"][0].pop("determinism")
    refresh_digest(missing_determinism["capabilities"][0])
    expect_schema_failure(missing_determinism, "oneOf")
    expect_validation_failure(
        missing_determinism,
        "requires determinism because the manifest requires determinism-contract-v1",
    )
    checks += 2

    invalid_determinism = copy.deepcopy(canonical)
    invalid_determinism["capabilities"][0]["determinism"]["repeatability"] = "usually"
    refresh_digest(invalid_determinism["capabilities"][0])
    expect_schema_failure(invalid_determinism, "is not one of")
    checks += 1

    legacy_minor_zero = copy.deepcopy(canonical)
    legacy_minor_zero["schema_minor"] = 0
    legacy_minor_zero["required_features"].remove("determinism-contract-v1")
    for row in legacy_minor_zero["capabilities"]:
        row.pop("determinism")
        refresh_digest(row)
    schema = json.loads((ROOT / manifest.MANIFEST_SCHEMA_FILE).read_text())
    assert not json_schema_lite.validate(legacy_minor_zero, schema)
    expect_validation_failure(legacy_minor_zero, "schema_minor must be exactly 1")
    checks += 2

    mismatched_minor_feature = copy.deepcopy(legacy_minor_zero)
    mismatched_minor_feature["required_features"].append("determinism-contract-v1")
    expect_schema_failure(mismatched_minor_feature, "oneOf")
    checks += 1

    unknown_required_feature = copy.deepcopy(canonical)
    unknown_required_feature["required_features"].append("future-contract-v99")
    expect_validation_failure(
        unknown_required_feature, "required_features must be the sorted supported feature set"
    )
    checks += 1

    malformed_required_features = copy.deepcopy(canonical)
    malformed_required_features["required_features"] = None
    expect_validation_failure(malformed_required_features, "expected type array")
    checks += 1

    wrong_absence = copy.deepcopy(canonical)
    wrong_absence["coverage"]["absence_semantics"] = "unsupported"
    expect_validation_failure(wrong_absence, "expected const 'unknown'")
    checks += 1

    negative_version = copy.deepcopy(canonical)
    negative_version["capabilities"][0]["contract_version"]["minor"] = -1
    expect_validation_failure(negative_version, "less than minimum 0")
    checks += 1

    negative_lifecycle = copy.deepcopy(canonical)
    negative_lifecycle["capabilities"][0]["evolution"]["introduced_in"]["major"] = -1
    refresh_digest(negative_lifecycle["capabilities"][0])
    expect_validation_failure(negative_lifecycle, "introduced_in is invalid")
    checks += 1

    deprecated_missing_fields = copy.deepcopy(canonical)
    row = deprecated_missing_fields["capabilities"][0]
    row["status"] = "deprecated"
    row["evolution"]["state"] = "deprecated"
    expect_schema_failure(deprecated_missing_fields, "oneOf")
    checks += 1

    active_with_deprecated_fields = copy.deepcopy(canonical)
    row = active_with_deprecated_fields["capabilities"][0]
    row["evolution"]["deprecated_in"] = {"major": 1, "minor": 0}
    row["evolution"]["replacement_key"] = None
    expect_schema_failure(active_with_deprecated_fields, "oneOf")
    checks += 1

    schema_status_mismatch = copy.deepcopy(canonical)
    schema_status_mismatch["capabilities"][0]["status"] = "deprecated"
    expect_schema_failure(schema_status_mismatch, "oneOf")
    checks += 1

    status_mismatch = copy.deepcopy(canonical)
    row = status_mismatch["capabilities"][0]
    row["evolution"] = {
        "state": "deprecated",
        "introduced_in": {"major": 1, "minor": 0},
        "deprecated_in": {"major": 1, "minor": 0},
        "replacement_key": status_mismatch["capabilities"][1]["key"],
    }
    refresh_digest(row)
    expect_validation_failure(status_mismatch, "requires deprecated status")
    checks += 1

    missing_replacement = copy.deepcopy(canonical)
    row = missing_replacement["capabilities"][0]
    row["status"] = "deprecated"
    row["evolution"] = {
        "state": "deprecated",
        "introduced_in": {"major": 1, "minor": 0},
        "deprecated_in": {"major": 1, "minor": 0},
        "replacement_key": "audio.does-not-exist",
    }
    refresh_digest(row)
    expect_validation_failure(missing_replacement, "does not name a live capability")
    checks += 1

    self_replacement = copy.deepcopy(canonical)
    row = self_replacement["capabilities"][0]
    row["status"] = "deprecated"
    row["evolution"] = {
        "state": "deprecated",
        "introduced_in": {"major": 1, "minor": 0},
        "deprecated_in": {"major": 1, "minor": 0},
        "replacement_key": row["key"],
    }
    refresh_digest(row)
    expect_validation_failure(self_replacement, "may not reference itself")
    checks += 1

    cycle = copy.deepcopy(canonical)
    first, second = cycle["capabilities"][:2]
    for row, replacement in ((first, second["key"]), (second, first["key"])):
        row["status"] = "deprecated"
        row["evolution"] = {
            "state": "deprecated",
            "introduced_in": {"major": 1, "minor": 0},
            "deprecated_in": {"major": 1, "minor": 0},
            "replacement_key": replacement,
        }
        refresh_digest(row)
    expect_validation_failure(cycle, "replacement_key cycle")
    checks += 1

    wrong_target = copy.deepcopy(canonical)
    wrong_target["capabilities"][0]["bindings"][0]["target"] = "Pulp::platform"
    refresh_digest(wrong_target["capabilities"][0])
    expect_validation_failure(wrong_target, "target must be minimal owning target")
    checks += 1

    reversed_lifecycle = copy.deepcopy(canonical)
    row = reversed_lifecycle["capabilities"][0]
    row["evolution"]["introduced_in"] = {"major": 2, "minor": 0}
    refresh_digest(row)
    expect_validation_failure(reversed_lifecycle, "introduced_in exceeds contract_version")
    checks += 1

    wrong_removed_status = copy.deepcopy(canonical)
    wrong_removed_status["tombstones"].append({
        "key": "audio.removed-example",
        "last_contract_version": {"major": 1, "minor": 0},
        "last_contract_digest": "sha256:" + "0" * 64,
        "status": "deprecated",
        "introduced_in": {"major": 1, "minor": 0},
        "deprecated_in": {"major": 1, "minor": 0},
        "removed_in_manifest_revision": 1,
        "reason": "Synthetic tombstone proves removed status is schema-enforced.",
        "replacement_key": None,
    })
    expect_validation_failure(wrong_removed_status, "expected const 'removed'")
    checks += 1

    saturator = next(row for row in manifest.EXPORTS if row["key"] == "signal.saturator")
    held_probes = saturator.pop("_link_probes")
    try:
        expect_problem(
            manifest.validate(canonical, ROOT),
            "requires operational installed-consumer probes",
        )
    finally:
        saturator["_link_probes"] = held_probes
    checks += 1

    held_probes = saturator["_link_probes"]
    saturator["_link_probes"] = [{
        "role": "entrypoint",
        "binding": "not::advertised",
        "operation": "construct",
        "arguments": "",
    }]
    try:
        expect_problem(
            manifest.validate(canonical, ROOT),
            "must name an advertised binding",
        )
    finally:
        saturator["_link_probes"] = held_probes
    checks += 1

    role_distinct_duplicate = copy.deepcopy(saturator)
    duplicate_binding = copy.deepcopy(role_distinct_duplicate["bindings"][0])
    duplicate_binding["role"] = "alternate-entrypoint"
    role_distinct_duplicate["bindings"].append(duplicate_binding)
    expect_problem(
        manifest._link_probe_problems(role_distinct_duplicate),
        f"alternate-entrypoint:{duplicate_binding['qualified_name']}",
    )
    checks += 1

    swing = next(row for row in manifest.EXPORTS if row["key"] == "timebase.swing")
    held_probes = swing["_link_probes"]
    swing["_link_probes"] = held_probes[:-1]
    try:
        expect_problem(
            manifest.validate(canonical, ROOT),
            "advertised bindings lack operational probes",
        )
    finally:
        swing["_link_probes"] = held_probes
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

    for field, weakened in (
        ("repeatability", "not_promised"),
        ("platform_scope", "same_build"),
        ("transport_history", "input"),
    ):
        weakening = copy.deepcopy(canonical)
        weakening["manifest_revision"] += 1
        row = next(
            item for item in weakening["capabilities"] if item["key"] == "timebase.tick"
        )
        row["determinism"][field] = weakened
        row["contract_version"]["minor"] += 1
        refresh_digest(row)
        expect_problem(
            manifest.evolution_problems(
                canonical, weakening, allow_unpublished_migration=False
            ),
            "breaking change without a major increase",
        )
        checks += 1

    partition_weakening = copy.deepcopy(canonical)
    partition_weakening["manifest_revision"] += 1
    row = next(
        item
        for item in partition_weakening["capabilities"]
        if item["key"] == "sequence.host-transport-projector"
    )
    row["determinism"]["block_partition"] = "not_applicable"
    row["contract_version"]["minor"] += 1
    refresh_digest(row)
    expect_problem(
        manifest.evolution_problems(
            canonical, partition_weakening, allow_unpublished_migration=False
        ),
        "breaking change without a major increase",
    )
    checks += 1

    invariant_previous = copy.deepcopy(canonical)
    row = next(
        item
        for item in invariant_previous["capabilities"]
        if item["key"] == "sequence.host-transport-projector"
    )
    row["determinism"]["block_partition"] = "invariant"
    refresh_digest(row)
    fixed_partition = copy.deepcopy(invariant_previous)
    fixed_partition["manifest_revision"] += 1
    row = next(
        item
        for item in fixed_partition["capabilities"]
        if item["key"] == "sequence.host-transport-projector"
    )
    row["determinism"]["block_partition"] = "fixed_partition_only"
    row["contract_version"]["minor"] += 1
    refresh_digest(row)
    expect_problem(
        manifest.evolution_problems(
            invariant_previous, fixed_partition, allow_unpublished_migration=False
        ),
        "breaking change without a major increase",
    )
    checks += 1

    removed_determinism = copy.deepcopy(canonical)
    removed_determinism["manifest_revision"] += 1
    row = removed_determinism["capabilities"][0]
    row.pop("determinism")
    row["contract_version"]["minor"] += 1
    refresh_digest(row)
    expect_problem(
        manifest.evolution_problems(
            canonical, removed_determinism, allow_unpublished_migration=False
        ),
        "breaking change without a major increase",
    )
    checks += 1

    stronger_previous = copy.deepcopy(canonical)
    stronger_previous["manifest_revision"] -= 1
    row = stronger_previous["capabilities"][0]
    row["contract_version"] = {"major": 1, "minor": 0}
    row["determinism"]["repeatability"] = "not_promised"
    refresh_digest(row)
    assert not manifest.evolution_problems(
        stronger_previous, canonical, allow_unpublished_migration=False
    )
    checks += 1

    major_weakening = copy.deepcopy(canonical)
    major_weakening["manifest_revision"] += 1
    row = next(
        item for item in major_weakening["capabilities"] if item["key"] == "timebase.tick"
    )
    row["determinism"]["repeatability"] = "not_promised"
    row["contract_version"] = {"major": 2, "minor": 0}
    refresh_digest(row)
    assert not manifest.evolution_problems(
        canonical, major_weakening, allow_unpublished_migration=False
    )
    checks += 1

    additive_units = copy.deepcopy(canonical)
    additive_units["manifest_revision"] += 1
    row = additive_units["capabilities"][0]
    row["units"].append("normalized")
    row["contract_version"]["minor"] += 1
    refresh_digest(row)
    assert not manifest.evolution_problems(
        canonical, additive_units, allow_unpublished_migration=False
    )
    checks += 1

    removed_units = copy.deepcopy(canonical)
    removed_units["manifest_revision"] += 1
    row = next(
        item for item in removed_units["capabilities"] if len(item["units"]) > 1
    )
    row["units"].pop()
    row["contract_version"]["minor"] += 1
    refresh_digest(row)
    expect_problem(
        manifest.evolution_problems(
            canonical, removed_units, allow_unpublished_migration=False
        ),
        "breaking change without a major increase",
    )
    checks += 1

    successor = copy.deepcopy(canonical)
    successor["manifest_revision"] += 1
    successor_row = copy.deepcopy(successor["capabilities"][0])
    successor_row["key"] += "-relaxed"
    successor_row["contract_version"] = {"major": 1, "minor": 0}
    successor_row["determinism"]["repeatability"] = "not_promised"
    refresh_digest(successor_row)
    old_row = successor["capabilities"][0]
    old_row["status"] = "deprecated"
    old_row["contract_version"] = {"major": 2, "minor": 0}
    old_row["evolution"] = {
        "state": "deprecated",
        "introduced_in": {"major": 1, "minor": 0},
        "deprecated_in": {"major": 2, "minor": 0},
        "replacement_key": successor_row["key"],
    }
    refresh_digest(old_row)
    successor["capabilities"].append(successor_row)
    assert not manifest.evolution_problems(
        canonical, successor, allow_unpublished_migration=False
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

    active_removed = copy.deepcopy(canonical)
    active_removed["manifest_revision"] += 1
    old = active_removed["capabilities"].pop(0)
    active_removed["tombstones"] = [{
        "key": old["key"],
        "last_contract_version": old["contract_version"],
        "last_contract_digest": old["contract_digest"],
        "status": "removed",
        "introduced_in": old["evolution"]["introduced_in"],
        "deprecated_in": old["contract_version"],
        "removed_in_manifest_revision": active_removed["manifest_revision"],
        "reason": "Synthetic direct removal must fail without a published window.",
        "replacement_key": None,
    }]
    expect_problem(
        manifest.evolution_problems(
            canonical, active_removed, allow_unpublished_migration=False
        ),
        "only after a published deprecated revision",
    )
    checks += 1

    deprecated = copy.deepcopy(canonical)
    deprecated["manifest_revision"] += 1
    old = deprecated["capabilities"][0]
    old["contract_version"] = {"major": 2, "minor": 0}
    old["status"] = "deprecated"
    old["evolution"] = {
        "state": "deprecated",
        "introduced_in": {"major": 1, "minor": 0},
        "deprecated_in": {"major": 2, "minor": 0},
        "replacement_key": deprecated["capabilities"][1]["key"],
    }
    refresh_digest(old)
    assert not manifest.evolution_problems(
        canonical, deprecated, allow_unpublished_migration=False
    )
    removed_after_window = copy.deepcopy(deprecated)
    removed_after_window["manifest_revision"] += 1
    old = removed_after_window["capabilities"].pop(0)
    removed_after_window["tombstones"] = [{
        "key": old["key"],
        "last_contract_version": old["contract_version"],
        "last_contract_digest": old["contract_digest"],
        "status": "removed",
        "introduced_in": old["evolution"]["introduced_in"],
        "deprecated_in": old["evolution"]["deprecated_in"],
        "removed_in_manifest_revision": removed_after_window["manifest_revision"],
        "reason": "Synthetic removal follows a separately published deprecated revision.",
        "replacement_key": old["evolution"]["replacement_key"],
    }]
    assert not manifest.evolution_problems(
        deprecated, removed_after_window, allow_unpublished_migration=False
    )
    checks += 2

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
            "status": "removed",
            "introduced_in": {"major": 1, "minor": 0},
            "deprecated_in": {"major": 1, "minor": 0},
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

    history = manifest.history_document([
        manifest.history_entry(
            canonical,
            json.loads((ROOT / surface.SURFACE_SNAPSHOT).read_text()),
        )
    ])
    bypass = copy.deepcopy(canonical)
    bypass["capabilities"][0]["scheduling"] = "edited-source-and-snapshot"
    refresh_digest(bypass["capabilities"][0])
    expect_problem(
        manifest.history_problems(
            history,
            bypass,
            json.loads((ROOT / surface.SURFACE_SNAPSHOT).read_text()),
        ),
        "changed without a contract_version increase",
    )
    checks += 1

    rewritten = copy.deepcopy(history)
    rewritten["entries"][0]["entry_digest"] = "sha256:" + "0" * 64
    expect_problem(
        manifest.append_only_history_problems(history, rewritten),
        "not append-only",
    )
    checks += 1

    protected_sha = subprocess.run(
        ["git", "rev-parse", "origin/main^{commit}"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    with tempfile.TemporaryDirectory(prefix="pulp-agent-shallow-base-") as temp:
        event_path = pathlib.Path(temp) / "event.json"
        event_path.write_text(
            json.dumps({"pull_request": {"base": {"sha": protected_sha}}})
        )
        held = {
            name: os.environ.get(name)
            for name in (
                "GITHUB_ACTIONS",
                "GITHUB_EVENT_PATH",
                "GITHUB_SHA",
                "PULP_AGENT_CAPABILITY_BASE_REF",
            )
        }
        os.environ.pop("PULP_AGENT_CAPABILITY_BASE_REF", None)
        os.environ["GITHUB_ACTIONS"] = "true"
        os.environ["GITHUB_EVENT_PATH"] = str(event_path)
        os.environ["GITHUB_SHA"] = "f" * 40
        try:
            assert manifest._resolve_protected_tip(
                ROOT, "refs/remotes/origin/definitely-missing"
            ) == protected_sha
            event_base = subprocess.run(
                ["git", "rev-parse", "HEAD^{commit}"],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=True,
            ).stdout.strip()
            event_path.write_text(
                json.dumps({"pull_request": {"base": {"sha": event_base}}})
            )
            assert manifest._resolve_protected_tip(ROOT, "origin/main") == event_base
            event_path.write_text(
                json.dumps({"merge_group": {"base_sha": protected_sha}})
            )
            assert manifest._resolve_protected_tip(
                ROOT, "refs/remotes/origin/definitely-missing"
            ) == protected_sha
            event_path.write_text("{}")
            assert manifest._resolve_protected_tip(
                ROOT, "refs/remotes/origin/definitely-missing"
            ) is None
        finally:
            for name, value in held.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value
    checks += 4

    held_path = os.environ.get("PATH")
    os.environ["PATH"] = ""
    try:
        assert manifest._git_output(ROOT, ["rev-parse", "HEAD"]) is None
    finally:
        if held_path is None:
            os.environ.pop("PATH", None)
        else:
            os.environ["PATH"] = held_path
    checks += 1

    return checks


def exercise_surface_mutations() -> int:
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pulp-agent-surface-") as temp:
        root = pathlib.Path(temp)
        for public_root in surface.PUBLIC_ROOTS:
            (root / public_root["source"]).mkdir(parents=True)
        directory = root / "core/signal/include/pulp/signal"
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

        missing_root = root / surface.PUBLIC_ROOTS[0]["source"]
        missing_root.rmdir()
        try:
            surface.discover_headers(root)
        except RuntimeError as error:
            assert "declared public capability root is missing" in str(error)
        else:
            raise AssertionError("missing declared public root was silently ignored")
        checks += 1

    canonical_surface, surface_problems = manifest.build_surface(ROOT)
    assert not surface_problems, surface_problems
    surface_schema = json.loads((ROOT / surface.SURFACE_SCHEMA_FILE).read_text())
    duplicate_root = copy.deepcopy(canonical_surface)
    duplicate_root["roots"].append(copy.deepcopy(duplicate_root["roots"][0]))
    expect_problem(
        json_schema_lite.validate(duplicate_root, surface_schema),
        "array items are not unique",
    )
    checks += 1

    reordered_duplicate_root = copy.deepcopy(canonical_surface)
    first_root = reordered_duplicate_root["roots"][0]
    reordered_duplicate_root["roots"].append(
        {
            "source": first_root["source"],
            "install_prefix": first_root["install_prefix"],
            "domain": first_root["domain"],
        }
    )
    expect_problem(
        json_schema_lite.validate(reordered_duplicate_root, surface_schema),
        "array items are not unique",
    )
    checks += 1

    invalid_domain = copy.deepcopy(canonical_surface)
    invalid_domain["roots"][0]["domain"] = "invented"
    expect_problem(
        json_schema_lite.validate(invalid_domain, surface_schema),
        "is not one of",
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

    changed_reviewed = copy.deepcopy(previous_surface)
    changed_reviewed["inventory_version"] = 2
    changed_reviewed["headers"][0]["fingerprint"] = "sha256:" + "3" * 64
    assert not manifest.surface_evolution_problems(
        previous_surface, changed_reviewed, surface.SURFACE_SCHEMA
    )
    removed_reviewed = copy.deepcopy(changed_reviewed)
    removed_reviewed["inventory_version"] = 3
    removed_reviewed["headers"] = []
    removed_reviewed["tombstones"] = [{
        "include": "pulp/signal/reviewed.hpp",
        "last_fingerprint": "sha256:" + "3" * 64,
        "removed_in_inventory_version": 3,
        "reason": "Synthetic reviewed header removal uses its immediately prior fingerprint.",
    }]
    assert not manifest.surface_evolution_problems(
        changed_reviewed, removed_reviewed, surface.SURFACE_SCHEMA
    )
    retained_tombstone = copy.deepcopy(removed_reviewed)
    retained_tombstone["inventory_version"] = 4
    retained_tombstone["headers"] = [{
        "include": "pulp/signal/other.hpp",
        "fingerprint": "sha256:" + "4" * 64,
        "disposition": "infrastructure",
    }]
    assert not manifest.surface_evolution_problems(
        removed_reviewed, retained_tombstone, surface.SURFACE_SCHEMA
    )
    checks += 3

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
    assert list(header_projection) == sorted(header_projection), (
        "signal vocabulary projection depends on filesystem traversal order"
    )
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
