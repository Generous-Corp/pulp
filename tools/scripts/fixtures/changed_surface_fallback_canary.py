#!/usr/bin/env python3
"""Bounded real-process canary for target-closure shadow fallback."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import stat
import sys
from pathlib import Path


SCRIPTS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS_DIR))

import run_changed_surface_tests as runner


def write_executable(path: Path, payload: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(payload, encoding="utf-8")
    path.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)


def encode_receipt() -> tuple[str, str]:
    selected_tests = ["canary-test"]
    selected_targets = ["closed-target"]
    tests_payload = b"canary-test\n"
    targets_payload = b"closed-target\n"
    receipt = {
        "schema_version": 2,
        "repository": "Generous-Corp/pulp",
        "pull_request": 42,
        "target": "mac",
        "base_sha": "a" * 40,
        "head_sha": "b" * 40,
        "tree_sha": "c" * 40,
        "policy_digest": "d" * 64,
        "selection_receipt_digest": "e" * 64,
        "validation_contract_digest": "f" * 64,
        "workflow_digest": "0" * 64,
        "selected_tests_digest": hashlib.sha256(tests_payload).hexdigest(),
        "selected_tests": selected_tests,
        "selected_build_targets_digest": hashlib.sha256(targets_payload).hexdigest(),
        "selected_build_targets": selected_targets,
    }
    payload = json.dumps(receipt, separators=(",", ":")).encode("utf-8")
    return (
        base64.urlsafe_b64encode(payload).decode("ascii").rstrip("="),
        hashlib.sha256(payload).hexdigest(),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    build_dir = root / "build"
    result_dir = root / "results"
    lock_dir = root / "locks"
    command_log = root / "commands.log"
    fake_root = root / "fake-repo"
    reply_dir = build_dir / ".cmake" / "api" / "v1" / "reply"
    reply_dir.mkdir(parents=True)
    (reply_dir / "index-canary.json").write_text(
        json.dumps(
            {"reply": {"codemodel-v2": {"jsonFile": "codemodel.json"}}}
        ),
        encoding="utf-8",
    )
    (reply_dir / "codemodel.json").write_text(
        json.dumps({"configurations": [{"targets": []}]}),
        encoding="utf-8",
    )
    command_script = """#!/bin/sh
set -eu
printf '%s\n' "$1" >> "$PULP_FALLBACK_CANARY_LOG"
exit 0
"""
    sentinel_script = """#!/bin/sh
set -eu
printf '%s\n' sentinel >> "$PULP_FALLBACK_CANARY_LOG"
exit 0
"""
    write_executable(fake_root / "tools" / "ci" / "governed-build.sh", command_script)
    write_executable(
        fake_root / "tools" / "ci" / "build-dir-sentinel.sh", sentinel_script
    )
    encoded, digest = encode_receipt()
    namespace = argparse.Namespace(
        build_dir=build_dir,
        selection_receipt_b64=encoded,
        selection_receipt_sha256=digest,
    )

    def refuse_target_closure(
        received: argparse.Namespace, received_build_dir: Path
    ) -> int:
        received._changed_surface_full_authority_started = False
        received._changed_surface_fallback_safe = True
        received._changed_surface_receipt_identity_verified = True
        received._changed_surface_selected_build_completed = False
        received._changed_surface_selected_build_seconds = None
        received._changed_surface_selected_build_result = None
        runner.validate_build_target_projection(
            build_dir=received_build_dir,
            selected_tests=[],
            selected_build_targets=["closed-target"],
        )
        raise AssertionError("missing target closure did not refuse")

    runner.REPO_ROOT = fake_root
    runner.run_locked = refuse_target_closure
    os.environ.update(
        {
            "SHIPYARD_CHANGED_SURFACE_COMPARE_FULL": "1",
            "SHIPYARD_CHANGED_SURFACE_RESULT_DIR": str(result_dir),
            "PULP_BUILD_DIR_LOCK_ROOT": str(lock_dir),
            "PULP_FALLBACK_CANARY_LOG": str(command_log),
        }
    )
    result = runner.run(namespace)
    receipts = list(result_dir.glob("result-*.json"))
    if result != 0 or len(receipts) != 1:
        raise RuntimeError("fallback canary did not publish exactly one passing receipt")
    receipt = json.loads(receipts[0].read_text(encoding="utf-8"))
    if (
        receipt["selected_registration_count"] is not None
        or receipt["full_registration_count"] is not None
        or receipt["selected_returncode"] is not None
        or "absent from the codemodel" not in receipt["fallback_reason"]
        or stat.S_IMODE(receipts[0].stat().st_mode) != stat.S_IRUSR
    ):
        raise RuntimeError("fallback receipt fabricated an unobserved result")
    commands = command_log.read_text(encoding="utf-8").splitlines()
    print(
        json.dumps(
            {
                "commands": commands,
                "disposition": receipt["selected_execution_disposition"],
                "graduation_eligible": receipt["graduation_eligible"],
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
