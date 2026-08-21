#!/usr/bin/env python3
"""Calibrated positive and negative controls for sequencer_exposure_check."""

from __future__ import annotations

import copy
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

from sequencer_exposure_check import (
    _load_base_transition,
    _semantic_added_paths,
    is_sequencer_owned_path,
    validate_document,
    validate_git_provenance,
    validate_release_evidence,
    validate_schema_contract,
    validate_tombstone_provenance,
    validate_transition,
)


SHA_A = "1" * 40
SHA_B = "2" * 40


def write(root: Path, path: str, text: str) -> None:
    destination = root / path
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text, encoding="utf-8")


def evidence(kind: str, path: str, *needles: str) -> dict:
    return {"kind": kind, "path": path, "needles": list(needles)}


def row_evidence(path: str, *needles: str) -> dict:
    return {"path": path, "needles": list(needles)}


def surface(disposition: str, rationale: str, items: list[dict] | None = None,
            *, owner: str | None = None, dependencies: list[str] | None = None) -> dict:
    result = {
        "disposition": disposition,
        "rationale": rationale,
        "dependencies": dependencies or [],
        "evidence": items or [],
    }
    if owner is not None:
        result["owner"] = owner
    return result


def fixture(root: Path, accepted_head: str, merge_sha: str) -> dict:
    files = {
        "docs/release.txt": "protected merge evidence",
        "core/timeline/schema/timeline_cli_verbs.json": "timeline.open definition",
        "tools/cli/cmd_seq.cpp": "int cmd_seq timeline.open handler",
        "core/timeline/schema/timeline_mcp_tools.json": "pulp_timeline_project_open definition",
        "tools/mcp/mcp_timeline_tools.cpp": (
            "handle_timeline_project_open ToolBinding pulp_timeline_project_open"
        ),
        "inspect/include/pulp/inspect/capability_definitions.inc": (
            "PULP_INSPECT_CAPABILITY SequencerControl dev.pulp.sequencer/control@1"
        ),
        "inspect/src/control_manifest.cpp": (
            "kControlOperations sequencer.control ControlOperationDescriptor"
        ),
        "inspect/src/control_installed_host.cpp": (
            "executor binding slot.install sequencer.control"
        ),
        "inspect/src/capabilities.cpp": "profile policy SequencerControl develop",
        "tools/cli/cmd_control.cpp": "sequencer.control CLI projection",
        "tools/mcp/mcp_control_tools.cpp": "sequencer.control MCP projection",
        "test/test_control.cpp": "sequencer.control executor acceptance test",
    }
    for path, text in files.items():
        write(root, path, text)

    na = lambda why: surface("not_applicable", why)  # noqa: E731
    return {
        "schema_version": 1,
        "ledger_id": "dev.pulp.sequencer-exposure@1",
        "audit": {
            "status": "complete",
            "scope": "Calibrated checker fixture.",
            "owner": "checker-selftest",
            "dependencies": [],
            "gaps": [],
        },
        "rows": [
            {
                "id": "SEQ-OFFLINE",
                "title": "Offline Timeline operations",
                "delivery_state": "released",
                "claim_id": "fixture-offline-claim",
                "owned_paths": ["tools/cli/cmd_seq.cpp", "tools/mcp/mcp_timeline_tools.cpp"],
                "classification": "offline_cli_mcp",
                "release": {
                    "pr": 101,
                    "accepted_head": accepted_head,
                    "merge_sha": merge_sha,
                    "integration_mode": "merge",
                },
                "evidence": [row_evidence("docs/release.txt", "protected merge")],
                "surfaces": {
                    "installed_sdk": na("Tool integration, not a new installed SDK surface."),
                    "offline_timeline_cli": surface("exposed", "Real CLI definition and handler.", [
                        evidence("cli_definition", "core/timeline/schema/timeline_cli_verbs.json", "timeline.open"),
                        evidence("cli_handler", "tools/cli/cmd_seq.cpp", "cmd_seq", "timeline.open"),
                    ]),
                    "offline_timeline_mcp": surface("exposed", "Real MCP definition and handler.", [
                        evidence("mcp_definition", "core/timeline/schema/timeline_mcp_tools.json", "pulp_timeline_project_open"),
                        evidence("mcp_handler", "tools/mcp/mcp_timeline_tools.cpp", "handle_timeline_project_open", "ToolBinding"),
                    ]),
                    "live_product_control": na("Offline project operation; no live instance authority."),
                    "design_time_agent_manifest": na("Runtime tool operation, not generator SDK discovery."),
                },
            },
            {
                "id": "SEQ-LIVE",
                "title": "Live Product-A sequencer control",
                "delivery_state": "released",
                "claim_id": "fixture-live-claim",
                "owned_paths": ["inspect/src/control_installed_host.cpp"],
                "classification": "live_product_control",
                "release": {
                    "pr": 101,
                    "accepted_head": accepted_head,
                    "merge_sha": merge_sha,
                    "integration_mode": "merge",
                },
                "evidence": [row_evidence("docs/release.txt", "merge evidence")],
                "surfaces": {
                    "installed_sdk": na("Control protocol operation, not a public engine SDK primitive."),
                    "offline_timeline_cli": na("Live exact-instance operation, not offline project editing."),
                    "offline_timeline_mcp": na("Live exact-instance operation, not offline project editing."),
                    "live_product_control": surface("exposed", "Canonical typed Product-A operation.", [
                        evidence("capability_definition", "inspect/include/pulp/inspect/capability_definitions.inc", "SequencerControl", "dev.pulp.sequencer/control@1"),
                        evidence("operation_definition", "inspect/src/control_manifest.cpp", "kControlOperations", "sequencer.control"),
                        evidence("executor_binding", "inspect/src/control_installed_host.cpp", "slot.install", "sequencer.control"),
                        evidence("profile_policy", "inspect/src/capabilities.cpp", "profile", "SequencerControl"),
                        evidence("cli_projection", "tools/cli/cmd_control.cpp", "sequencer.control"),
                        evidence("mcp_projection", "tools/mcp/mcp_control_tools.cpp", "sequencer.control"),
                        evidence("test", "test/test_control.cpp", "sequencer.control", "acceptance"),
                    ]),
                    "design_time_agent_manifest": na("Live authority is separate from design-time capability discovery."),
                },
            },
        ],
        "tombstones": [],
    }


def expect_red(name: str, document: dict, root: Path, expected: str) -> None:
    errors = validate_document(document, root)
    if not errors:
        raise AssertionError(f"{name}: mutation unexpectedly passed")
    rendered = "\n".join(errors)
    if expected not in rendered:
        raise AssertionError(f"{name}: expected {expected!r}, got:\n{rendered}")


def git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    return result.stdout.strip()


def initialize_history(root: Path) -> tuple[str, str, str, str]:
    git(root, "init", "-q", "-b", "main")
    git(root, "config", "user.name", "Pulp Checker")
    git(root, "config", "user.email", "checker@pulp.invalid")
    git(root, "add", ".")
    git(root, "commit", "-q", "-m", "fixture base")
    git(root, "checkout", "-q", "-b", "accepted")
    write(root, "feature.txt", "accepted feature head")
    git(root, "add", "feature.txt")
    git(root, "commit", "-q", "-m", "fixture feature")
    accepted = git(root, "rev-parse", "HEAD")
    git(root, "checkout", "-q", "main")
    write(root, "main.txt", "concurrent protected main")
    git(root, "add", "main.txt")
    git(root, "commit", "-q", "-m", "fixture main movement")
    git(
        root,
        "merge",
        "-q",
        "--no-ff",
        "accepted",
        "-m",
        "Merge pull request #101 from fixture/accepted",
    )
    merge = git(root, "rev-parse", "HEAD")
    git(root, "checkout", "-q", "-b", "squash-source")
    write(root, "squash.txt", "content-stable squash")
    git(root, "add", "squash.txt")
    git(root, "commit", "-q", "-m", "fixture squash source")
    squash_source = git(root, "rev-parse", "HEAD")
    git(root, "checkout", "-q", "main")
    git(root, "merge", "-q", "--squash", "squash-source")
    git(root, "commit", "-q", "-m", "fixture squash result (#103)")
    return accepted, merge, squash_source, git(root, "rev-parse", "HEAD")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pulp-sequencer-exposure-") as directory:
        root = Path(directory)
        # Materialize evidence before committing a real two-parent merge graph.
        fixture(root, SHA_A, SHA_B)
        accepted_head, merge_sha, squash_source, squash_merge = initialize_history(root)
        valid = fixture(root, accepted_head, merge_sha)
        errors = validate_document(valid, root)
        if errors:
            raise AssertionError("valid fixture failed:\n" + "\n".join(errors))
        provenance_errors = validate_git_provenance(valid, root)
        if provenance_errors:
            raise AssertionError("valid provenance failed:\n" + "\n".join(provenance_errors))
        squash_valid = copy.deepcopy(valid)
        squash_valid["rows"] = [copy.deepcopy(valid["rows"][0])]
        squash_valid["rows"][0]["owned_paths"] = ["squash.txt"]
        squash_valid["rows"][0]["release"] = {
            "pr": 103,
            "accepted_head": squash_source,
            "merge_sha": squash_merge,
            "integration_mode": "squash",
        }
        provenance_errors = validate_git_provenance(squash_valid, root)
        if provenance_errors:
            raise AssertionError("valid squash provenance failed:\n" + "\n".join(provenance_errors))
        remote = root.parent / f"{root.name}-remote.git"
        fresh = root.parent / f"{root.name}-fresh"
        subprocess.run(
            ["git", "clone", "-q", "--bare", str(root), str(remote)], check=True
        )
        git(remote, "update-ref", "refs/pull/103/head", squash_source)
        subprocess.run(
            [
                "git", "clone", "-q", "--no-local", "--single-branch",
                "--branch", "main", str(remote), str(fresh)
            ],
            check=True,
        )
        missing_before_fetch = subprocess.run(
            ["git", "-C", str(fresh), "cat-file", "-e", f"{squash_source}^{{commit}}"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if missing_before_fetch.returncode == 0:
            raise AssertionError("fresh-clone control unexpectedly contained squash PR head")
        provenance_errors = validate_git_provenance(squash_valid, fresh)
        if provenance_errors:
            raise AssertionError(
                "fresh-clone squash fetch proof failed:\n" + "\n".join(provenance_errors)
            )
        shutil.rmtree(fresh)
        shutil.rmtree(remote)

        schema_path = Path(__file__).resolve().parents[2] / "docs/status/sequencer-exposure.schema.json"
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        schema_errors = validate_schema_contract(schema)
        if schema_errors:
            raise AssertionError("schema/checker drift:\n" + "\n".join(schema_errors))
        schema_mutation = copy.deepcopy(schema)
        schema_mutation["$defs"]["row"]["properties"]["classification"]["enum"].pop()
        if not validate_schema_contract(schema_mutation):
            raise AssertionError("schema drift mutation unexpectedly passed")
        schema_mutation = copy.deepcopy(schema)
        gap_rule = schema_mutation["$defs"]["surface"]["allOf"][0]
        gap_rule["then"]["required"] = ["owner"]
        if not any("gap/deferred" in error
                   for error in validate_schema_contract(schema_mutation)):
            raise AssertionError("gap/deferred schema drift mutation unexpectedly passed")
        schema_mutation = copy.deepcopy(schema)
        schema_mutation["$defs"]["tombstone"]["required"].remove("claim_id")
        if not any("tombstone" in error
                   for error in validate_schema_contract(schema_mutation)):
            raise AssertionError("tombstone schema drift mutation unexpectedly passed")

        mutation = copy.deepcopy(valid)
        del mutation["rows"][0]["classification"]
        expect_red("missing classification", mutation, root, "missing fields: classification")

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["delivery_state"] = "pending"
        mutation["rows"][0].pop("release")
        mutation["rows"][0]["evidence"][0]["needles"] = ["not in the file"]
        expect_red("stale evidence", mutation, root, "stale evidence")

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["surfaces"]["installed_sdk"] = surface(
            "gap", "Known SDK projection gap.", dependencies=["SEQ-LIVE"]
        )
        expect_red("ownerless gap", mutation, root, "owner: required for gap")

        mutation = copy.deepcopy(valid)
        mutation["rows"].append(copy.deepcopy(mutation["rows"][0]))
        expect_red("duplicate ID", mutation, root, "duplicate live row ID")

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["surfaces"]["offline_timeline_mcp"]["evidence"] = [
            evidence("mcp_definition", "core/timeline/schema/timeline_mcp_tools.json", "pulp_timeline_project_open")
        ]
        expect_red("generated definition without handler", mutation, root, "missing mcp_handler")

        mutation = copy.deepcopy(valid)
        mutation["rows"][1]["surfaces"]["live_product_control"]["evidence"] = [
            evidence("capability_definition", "inspect/include/pulp/inspect/capability_definitions.inc", "SequencerControl")
        ]
        expect_red("live claim without operation/executor", mutation, root, "live control claim missing")

        mutation = copy.deepcopy(valid)
        mutation["tombstones"] = [{
            "id": "SEQ-OFFLINE",
            "delivery_state": "pending",
            "claim_id": "remove-seq-offline",
            "owned_paths": valid["rows"][0]["owned_paths"],
            "rationale": "Removed after replacement.",
        }]
        expect_red("tombstone reuse", mutation, root, "tombstoned IDs reused")

        mutation = copy.deepcopy(valid)
        mutation["tombstones"] = [{
            "id": "SEQ-REMOVED",
            "delivery_state": "pending",
            "claim_id": "remove-seq-removed",
            "owned_paths": ["docs/removed-sequencer-surface.md"],
            "rationale": "Pending protected removal.",
            "removed_in_merge": merge_sha,
        }]
        expect_red(
            "pending tombstone false merge", mutation, root,
            "pending tombstone cannot claim"
        )

        mutation = copy.deepcopy(valid)
        mutation["tombstones"] = [{
            "id": "SEQ-REMOVED",
            "delivery_state": "released",
            "claim_id": "remove-seq-removed",
            "owned_paths": ["docs/removed-sequencer-surface.md"],
            "rationale": "Released protected removal.",
        }]
        expect_red(
            "released tombstone missing merge", mutation, root,
            "released tombstone requires merge proof"
        )
        mutation["tombstones"][0]["removed_in_merge"] = SHA_A
        tombstone_errors = validate_tombstone_provenance(mutation, root)
        if not any("commit object is unavailable" in error for error in tombstone_errors):
            raise AssertionError(
                f"false tombstone provenance unexpectedly passed: {tombstone_errors}"
            )

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["release"]["accepted_head"] = SHA_A
        provenance_errors = validate_git_provenance(mutation, root)
        if not any("commit object is unavailable" in error for error in provenance_errors):
            raise AssertionError(f"false provenance unexpectedly passed: {provenance_errors}")

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["release"]["integration_mode"] = "squash"
        provenance_errors = validate_git_provenance(mutation, root)
        if not any("squash mode requires exactly one parent" in error
                   for error in provenance_errors):
            raise AssertionError(f"false integration mode unexpectedly passed: {provenance_errors}")

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["release"]["pr"] = 999
        provenance_errors = validate_git_provenance(mutation, root)
        if not any("does not bind PR #999" in error for error in provenance_errors):
            raise AssertionError(f"wrong merge PR unexpectedly passed: {provenance_errors}")

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["evidence"][0]["needles"] = ["future-only historical claim"]
        historical_errors = validate_release_evidence(mutation, root)
        if not any("historical evidence" in error for error in historical_errors):
            raise AssertionError(
                f"false historical evidence unexpectedly passed: {historical_errors}"
            )

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["delivery_state"] = "pending"
        expect_red("pending false release", mutation, root, "pending row cannot claim")

        transition_errors = validate_transition(
            valid, valid, ["core/timeline/src/command.cpp"]
        )
        if not any("not covered" in error for error in transition_errors):
            raise AssertionError(f"uncovered-path mutation unexpectedly passed: {transition_errors}")

        p3_paths = [
            "core/midi/include/pulp/midi/midi_clock.hpp",
            "core/midi/src/clock_chaser.cpp",
            "test/test_midi-clock-chaser.cpp",
        ]
        transition_errors = validate_transition(valid, valid, p3_paths)
        missed_p3_paths = [
            path for path in p3_paths
            if not any(path in error for error in transition_errors)
        ]
        if missed_p3_paths:
            raise AssertionError(
                f"P3-style MIDI-clock paths escaped watcher: {missed_p3_paths}"
            )

        transition_errors = validate_transition(
            valid, valid, ["docs/status/sequencer-exposure.schema.json"]
        )
        if not any("not covered" in error for error in transition_errors):
            raise AssertionError(
                f"unguarded E0 infrastructure change unexpectedly passed: {transition_errors}"
            )
        e0_current = copy.deepcopy(valid)
        e0_pending = copy.deepcopy(valid["rows"][0])
        e0_pending.update({
            "id": "E0-PENDING",
            "title": "Pending E0 governance change",
            "delivery_state": "pending",
            "claim_id": "e0-governance-claim",
            "owned_paths": [
                "docs/status/sequencer-exposure.json",
                "docs/status/sequencer-exposure.schema.json",
                "tools/scripts/sequencer_exposure_check.py",
                "tools/scripts/test_sequencer_exposure_check.py",
                ".github/workflows/version-skill-check.yml",
                "docs/reference/capability-control.md",
            ],
        })
        e0_pending.pop("release")
        e0_current["rows"].append(e0_pending)
        e0_changed_paths = [
            "docs/status/sequencer-exposure.json",
            "docs/status/sequencer-exposure.schema.json",
            "tools/scripts/sequencer_exposure_check.py",
            "tools/scripts/test_sequencer_exposure_check.py",
            ".github/workflows/version-skill-check.yml",
            "docs/reference/capability-control.md",
        ]
        transition_errors = validate_transition(
            valid,
            e0_current,
            e0_changed_paths,
        )
        if transition_errors:
            raise AssertionError(f"covered E0 governance change failed: {transition_errors}")
        bootstrap_errors = validate_transition(None, e0_current, e0_changed_paths)
        if bootstrap_errors:
            raise AssertionError(f"base-less E0 bootstrap failed: {bootstrap_errors}")
        bootstrap_errors = validate_transition(
            None,
            valid,
            ["core/timeline/src/uncovered_bootstrap.cpp"],
        )
        if not any("not covered" in error for error in bootstrap_errors):
            raise AssertionError(
                f"base-less uncovered path unexpectedly passed: {bootstrap_errors}"
            )

        mutation = copy.deepcopy(valid)
        mutation["rows"][0]["title"] = "Unrelated prose edit"
        transition_errors = validate_transition(
            valid, mutation, ["core/timeline/src/command.cpp", "docs/status/sequencer-exposure.json"]
        )
        if not any("not covered" in error for error in transition_errors):
            raise AssertionError(f"unrelated-row bypass unexpectedly passed: {transition_errors}")

        mutation = copy.deepcopy(valid)
        pending = copy.deepcopy(valid["rows"][0])
        pending.update({
            "id": "SEQ-PENDING",
            "title": "Pending source transaction",
            "delivery_state": "pending",
            "claim_id": "pending-claim",
            "owned_paths": ["core/timeline/src/command.cpp"],
        })
        pending.pop("release")
        mutation["rows"].append(pending)
        pending_errors = validate_document(mutation, root)
        if pending_errors:
            raise AssertionError(f"valid pending row failed: {pending_errors}")
        transition_errors = validate_transition(
            valid, mutation, ["core/timeline/src/command.cpp", "docs/status/sequencer-exposure.json"]
        )
        if transition_errors:
            raise AssertionError(f"covered pending transition failed: {transition_errors}")

        mutation = copy.deepcopy(valid)
        rename_pending = copy.deepcopy(pending)
        rename_pending["id"] = "SEQ-RENAME-PENDING"
        rename_pending["owned_paths"] = ["docs/moved-command.cpp"]
        mutation["rows"].append(rename_pending)
        transition_errors = validate_transition(
            valid,
            mutation,
            ["core/timeline/src/command.cpp", "docs/moved-command.cpp"],
        )
        if not any("core/timeline/src/command.cpp" in error
                   for error in transition_errors):
            raise AssertionError(
                f"cross-boundary rename unexpectedly passed: {transition_errors}"
            )

        rename_root = root / "rename-diff-fixture"
        rename_root.mkdir()
        git(rename_root, "init", "-q", "-b", "main")
        git(rename_root, "config", "user.name", "Pulp Checker")
        git(rename_root, "config", "user.email", "checker@pulp.invalid")
        write(rename_root, "core/timeline/src/rename_me.cpp", "watched endpoint")
        git(rename_root, "add", ".")
        git(rename_root, "commit", "-q", "-m", "rename base")
        rename_base = git(rename_root, "rev-parse", "HEAD")
        (rename_root / "docs").mkdir()
        git(
            rename_root,
            "mv",
            "core/timeline/src/rename_me.cpp",
            "docs/renamed_out.cpp",
        )
        git(rename_root, "commit", "-q", "-m", "cross-boundary rename")
        _, changed_paths, _ = _load_base_transition(rename_root, rename_base)
        if set(changed_paths) != {
            "core/timeline/src/rename_me.cpp",
            "docs/renamed_out.cpp",
        }:
            raise AssertionError(f"rename endpoints were coalesced: {changed_paths}")
        for watched_path in (
            "examples/timeline-step-editor/main.cpp",
            ".agents/skills/timeline/references/commands.md",
            "core/view/src/custom_step_grid_adapter.cpp",
        ):
            if not is_sequencer_owned_path(watched_path):
                raise AssertionError(f"semantic watcher missed {watched_path}")
        semantic_base = git(rename_root, "rev-parse", "HEAD")
        semantic_path = "core/state/include/pulp/state/new_control.hpp"
        write(rename_root, semantic_path, "class SequencerStateChannelAdapter {};")
        git(rename_root, "add", semantic_path)
        git(rename_root, "commit", "-q", "-m", "cross-module sequencer semantics")
        semantic_paths, semantic_errors = _semantic_added_paths(
            rename_root, semantic_base, [semantic_path]
        )
        if semantic_errors or semantic_paths != {semantic_path}:
            raise AssertionError(
                f"added-line semantic scan failed: {semantic_paths}, {semantic_errors}"
            )
        semantic_current = copy.deepcopy(valid)
        semantic_pending = copy.deepcopy(pending)
        semantic_pending["id"] = "SEQ-SEMANTIC-PENDING"
        semantic_pending["claim_id"] = "semantic-pending-claim"
        semantic_pending["owned_paths"] = [semantic_path]
        semantic_current["rows"].append(semantic_pending)
        transition_errors = validate_transition(
            valid,
            semantic_current,
            [semantic_path, "docs/status/sequencer-exposure.json"],
            semantic_added_paths={semantic_path},
        )
        if not any("requires a Sequencer-Exposure trailer" in error
                   for error in transition_errors):
            raise AssertionError(
                f"cross-module semantic change without trailer passed: {transition_errors}"
            )
        transition_errors = validate_transition(
            valid,
            semantic_current,
            [semantic_path, "docs/status/sequencer-exposure.json"],
            trailer_ids=["SEQ-SEMANTIC-PENDING"],
            semantic_added_paths={semantic_path},
        )
        if transition_errors:
            raise AssertionError(
                f"covered semantic change with trailer failed: {transition_errors}"
            )
        shutil.rmtree(rename_root)

        mutation = copy.deepcopy(valid)
        mutation["rows"] = mutation["rows"][1:]
        transition_errors = validate_transition(
            valid, mutation, ["docs/status/sequencer-exposure.json"]
        )
        if not any("lacks a newly added pending tombstone" in error
                   for error in transition_errors):
            raise AssertionError(f"row removal mutation unexpectedly passed: {transition_errors}")

        removal = copy.deepcopy(mutation)
        removal["tombstones"] = [{
            "id": "SEQ-OFFLINE",
            "delivery_state": "pending",
            "claim_id": "remove-seq-offline",
            "owned_paths": valid["rows"][0]["owned_paths"],
            "rationale": "Pending protected removal.",
        }]
        removal_errors = validate_document(removal, root)
        removal_errors.extend(
            validate_transition(
                valid, removal, ["docs/status/sequencer-exposure.json"]
            )
        )
        if removal_errors:
            raise AssertionError(f"valid pending tombstone removal failed: {removal_errors}")

        promoted = copy.deepcopy(removal)
        write(
            root,
            "docs/status/sequencer-exposure.json",
            json.dumps(removal, indent=2) + "\n",
        )
        git(root, "add", "docs/status/sequencer-exposure.json")
        git(root, "commit", "-q", "-m", "pending removal transaction")
        removal_merge = git(root, "rev-parse", "HEAD")
        promoted["tombstones"][0]["delivery_state"] = "released"
        promoted["tombstones"][0]["removed_in_merge"] = removal_merge
        promotion_errors = validate_document(promoted, root)
        promotion_errors.extend(
            validate_transition(
                removal, promoted, ["docs/status/sequencer-exposure.json"]
            )
        )
        promotion_errors.extend(validate_tombstone_provenance(promoted, root))
        if promotion_errors:
            raise AssertionError(f"valid tombstone promotion failed: {promotion_errors}")

        stale_removal = copy.deepcopy(promoted)
        stale_removal["tombstones"][0]["removed_in_merge"] = merge_sha
        tombstone_errors = validate_tombstone_provenance(stale_removal, root)
        if not any("removal snapshot" in error for error in tombstone_errors):
            raise AssertionError(
                f"pre-removal ancestor unexpectedly proved removal: {tombstone_errors}"
            )

        changed_published = copy.deepcopy(promoted)
        changed_published["tombstones"][0]["rationale"] = "Rewritten history."
        transition_errors = validate_transition(
            promoted,
            changed_published,
            ["docs/status/sequencer-exposure.json"],
        )
        if not any("published tombstone changed" in error for error in transition_errors):
            raise AssertionError(
                f"published tombstone rewrite unexpectedly passed: {transition_errors}"
            )

    print("sequencer exposure checker selftest: OK (15 green, 29 calibrated red controls)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
