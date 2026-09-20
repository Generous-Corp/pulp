#!/usr/bin/env python3
"""Prove the real measurement producer's calibration failure path is evidence.

This drives the product measurement binary against the frozen
``shader-heavy-controls`` fixture at exact DPR 1 and requires that the receipt
it emits when the GPU timer cannot resolve the known-extra-work control is
retained, contained, digest-exact evidence rather than a bare reason string.

The receipt under test is produced by the binary, never assembled here. A
hand-written receipt proves the serializer; only a producer-emitted one proves
that the failure path an operator actually hits carries its diagnostics.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import gpu_dpr_evidence as evidence  # noqa: E402
import gpu_dpr_experiment as experiment  # noqa: E402
import gpu_dpr_pulp_native_adapter as native_adapter  # noqa: E402
import gpu_dpr_runner as runner  # noqa: E402

ROOT = SCRIPT_DIR.parent.parent
ADAPTER = SCRIPT_DIR / "gpu_dpr_pulp_native_adapter.py"
SCENARIO = "shader-heavy-controls"
CALIBRATION_DEPENDENCY = "gpu:timer-calibration"
DETECTION_FAILURE_CLASSES = {"timer_quantization", "insufficient_extra_work"}
# A dependency the host cannot satisfy is a missing premise, not a defect: the
# producer is a tooling-only target and needs a PULP_BENCHMARK + PULP_TRACING
# build plus a usable GPU surface.
PREMISE_DEPENDENCIES = {"build:benchmark-and-tracing", "gpu:measurement-surface"}
SKIP_EXIT = 77


class Skip(Exception):
    """The host cannot host this proof's premise."""


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git_head() -> str:
    return subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()


def measurement_producer() -> Path:
    candidates = [
        sys.argv[1] if len(sys.argv) > 1 else None,
        os.environ.get("PULP_DPR_NATIVE_MEASUREMENT_BIN"),
    ]
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate).expanduser()
        if path.is_file() and os.access(path, os.X_OK):
            return path.resolve()
    raise Skip(
        "the native DPR measurement producer is not built; build "
        "pulp-gpu-dpr-native-measurement from a PULP_BENCHMARK=ON "
        "PULP_TRACING=ON GPU configuration"
    )


def trace_analyzer(root: Path) -> dict[str, str]:
    """Pin a stub analyzer the incomplete-receipt path never invokes.

    Run-state initialization requires an exact analyzer identity. A receipt that
    stops at calibration returns before any trace is analyzed, so binding the
    product analyzer here would assert nothing this proof measures.
    """
    script = root / "unused-trace-analyzer.py"
    script.write_text(
        "#!/usr/bin/env python3\n"
        "raise SystemExit('calibration receipts are never trace-analyzed')\n",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return {"path": str(script.resolve()), "sha256": digest(script)}


def planned_experiment(manifest: dict[str, Any]) -> dict[str, Any]:
    class Args:
        experiment_id = "a4-calibration-failure-proof"
        plan_revision = git_head()
        pulp_sha = git_head()
        forge_sha = "0" * 40

    return experiment.planned_result(Args(), manifest)


def rejects(
    label: str, expected: str, call: Any, *arguments: Any,
) -> None:
    """Require one exact rejection, not merely that something was raised."""
    try:
        call(*arguments)
    except ValueError as error:
        if expected not in str(error):
            raise AssertionError(
                f"{label}: rejected for the wrong reason: {error}"
            ) from error
        return
    raise AssertionError(f"{label}: planted evidence was accepted")


def run_producer(
    producer: Path, run_dir: Path, state: dict[str, Any],
    manifest: dict[str, Any],
) -> tuple[str, str, Path, Path, str]:
    key = runner.cell_key(SCENARIO, "exact", 1)
    nonce, request_path = runner.issue_attempt(run_dir, state, manifest, key)
    cell_dir = runner.checked_cell_directory(run_dir, key)
    receipt_path = cell_dir / f"receipt-attempt-{nonce}.json"
    environment = dict(os.environ)
    environment["PULP_DPR_NATIVE_MEASUREMENT_BIN"] = str(producer)
    completed = subprocess.run(
        [sys.executable, str(ADAPTER), "--request", str(request_path),
         "--receipt", str(receipt_path)],
        cwd=cell_dir, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=1500,
    )
    if not receipt_path.is_file():
        raise AssertionError(
            f"adapter exited {completed.returncode} without a receipt: "
            f"{completed.stderr.strip()[:2000]}"
        )
    return key, nonce, receipt_path, cell_dir, completed.stderr


def classify(receipt: dict[str, Any], stderr: str = "") -> None:
    outcome = receipt.get("outcome")
    dependencies = receipt.get("dependencies") or []
    if outcome in {"pass", "fail"}:
        raise Skip(
            f"the GPU timer resolved its control on this host (outcome "
            f"{outcome}), so the calibration failure path was not exercised"
        )
    if outcome != "inconclusive":
        raise AssertionError(f"unexpected producer outcome {outcome!r}")
    if CALIBRATION_DEPENDENCY in dependencies:
        return
    premise = sorted(set(dependencies) & PREMISE_DEPENDENCIES)
    if premise:
        raise Skip(
            f"the producer stopped on an unmet host premise {premise}: "
            f"{receipt.get('reason')}"
        )
    raise AssertionError(
        f"producer stopped on {dependencies} rather than reaching calibration: "
        f"{receipt.get('reason')} {stderr[:1000]}"
    )


def assert_fail_closed_with_diagnostics(receipt: dict[str, Any]) -> None:
    """Shader-heavy controls that do not pass must fail closed with evidence."""
    diagnostics = receipt["diagnostics"]
    failure_class = diagnostics.get("failure_class")
    assert failure_class in DETECTION_FAILURE_CLASSES, (
        f"calibration failed as {failure_class!r}, which is not a control-"
        "detection verdict; the producer could not sample its own timer"
    )
    assert diagnostics.get("baseline_samples_ms"), "calibration retained no baseline samples"


def prove(producer: Path) -> None:
    manifest_path = experiment.DEFAULT_MANIFEST
    manifest = runner.load_json(manifest_path)
    with tempfile.TemporaryDirectory(prefix="pulp-dpr-calibration-proof-") as temporary:
        temporary_root = Path(temporary)
        state = runner.initial_state(
            planned_experiment(manifest), manifest, manifest_path,
            trace_analyzer(temporary_root),
        )
        run_dir = temporary_root / "run"
        runner.save_state(run_dir, state)
        key, nonce, receipt_path, cell_dir, stderr = run_producer(
            producer, run_dir, state, manifest
        )
        state = runner.load_state(run_dir)
        receipt = runner.load_json(receipt_path)
        classify(receipt, stderr)

        request = runner.load_json(cell_dir / f"request-attempt-{nonce}.json")
        # The adapter runs a copy it pins into the cell; that copy is what the
        # producer digested as its own identity.
        pinned = cell_dir / f"measurement-producer-{nonce}{producer.suffix}"
        assert receipt["dependencies"] == [CALIBRATION_DEPENDENCY], receipt["dependencies"]
        assert receipt["attempt_nonce"] == nonce
        assert receipt["scenario_id"] == SCENARIO
        assert receipt["producer_sha256"] == digest(pinned), (
            "calibration receipt is not bound to the pinned producer binary"
        )
        binding = receipt["diagnostics_artifact"]
        assert binding["schema"] == "pulp.gpu-dpr-diagnostics-artifact.v1"
        artifact = cell_dir / binding["path"]
        assert artifact.is_file() and not artifact.is_symlink()
        assert binding["sha256"] == digest(artifact)
        assert json.loads(artifact.read_text(encoding="utf-8")) == receipt["diagnostics"]
        assert_fail_closed_with_diagnostics(receipt)

        original_receipt = json.loads(json.dumps(receipt))
        original_bytes = artifact.read_bytes()

        def restore() -> None:
            if artifact.is_symlink():
                artifact.unlink()
            artifact.write_bytes(original_bytes)
            receipt_path.write_text(
                json.dumps(original_receipt) + "\n", encoding="utf-8"
            )

        def plant(mutate: Any) -> dict[str, Any]:
            restore()
            planted = json.loads(json.dumps(original_receipt))
            mutate(planted)
            receipt_path.write_text(json.dumps(planted) + "\n", encoding="utf-8")
            return planted

        def observe() -> tuple[str, Any, list[str]]:
            return evidence.receipt_observation(
                receipt_path, state, manifest, manifest_path, run_dir
            )

        # Positive control. Every rejection below is meaningless unless the
        # unmodified producer receipt is accepted by the same call.
        observed_key, observation, dependencies = observe()
        assert observed_key == key and observation is None
        assert dependencies == [CALIBRATION_DEPENDENCY]
        native_adapter.validate_measurement_receipt(
            request, json.loads(json.dumps(original_receipt)), cell_dir, pinned
        )

        def escape(planted: dict[str, Any]) -> None:
            planted["diagnostics_artifact"]["path"] = "../escape.json"

        plant(escape)
        rejects("path escape", "artifact escapes its cell directory", observe)
        rejects(
            "path escape (adapter)",
            "calibration diagnostics artifact path must stay inside the cell",
            native_adapter.validate_measurement_receipt,
            request, plant(escape), cell_dir, pinned,
        )

        restore()
        outside = cell_dir.parent / "outside-diagnostics.json"
        outside.write_bytes(original_bytes)
        artifact.unlink()
        artifact.symlink_to(outside)
        rejects("symlinked artifact", "artifact must not be a symlink", observe)
        rejects(
            "symlinked artifact (adapter)",
            "calibration diagnostics artifact escapes the cell",
            native_adapter.validate_measurement_receipt,
            request, json.loads(json.dumps(original_receipt)), cell_dir, pinned,
        )

        restore()
        truncated = original_bytes[: max(1, len(original_bytes) // 2)]
        try:
            json.loads(truncated.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            pass
        else:
            raise AssertionError("truncated diagnostics still parse as JSON")
        artifact.write_bytes(truncated)

        def rebind_truncated(planted: dict[str, Any]) -> None:
            planted["diagnostics_artifact"]["sha256"] = hashlib.sha256(
                truncated
            ).hexdigest()

        plant(rebind_truncated)
        artifact.write_bytes(truncated)
        rejects(
            "malformed artifact JSON",
            "calibration diagnostics artifact is not valid JSON", observe,
        )
        planted = json.loads(json.dumps(original_receipt))
        rebind_truncated(planted)
        rejects(
            "malformed artifact JSON (adapter)",
            "calibration diagnostics artifact is not valid JSON",
            native_adapter.validate_measurement_receipt,
            request, planted, cell_dir, pinned,
        )

        def flip(planted: dict[str, Any]) -> None:
            value = planted["diagnostics_artifact"]["sha256"]
            replacement = "0" if value[0] != "0" else "1"
            planted["diagnostics_artifact"]["sha256"] = replacement + value[1:]

        plant(flip)
        rejects(
            "digest drift",
            "calibration diagnostics artifact digest does not match", observe,
        )
        rejects(
            "digest drift (adapter)",
            "calibration diagnostics artifact digest does not match",
            native_adapter.validate_measurement_receipt,
            request, plant(flip), cell_dir, pinned,
        )

        restore()

        def rebind(planted: dict[str, Any]) -> dict[str, Any]:
            payload = json.dumps(planted["diagnostics"]) + "\n"
            artifact.write_text(payload, encoding="utf-8")
            planted["diagnostics_artifact"]["sha256"] = hashlib.sha256(
                payload.encode("utf-8")
            ).hexdigest()
            return planted

        inconsistent_validity = json.loads(json.dumps(original_receipt))
        trial = inconsistent_validity["diagnostics"]["trials"][0]["baseline"]
        trial["valid"] = False
        if trial["value_ms"] is None:
            trial["value_ms"] = 1.0
        rejects(
            "inconsistent sample validity",
            "invalid calibration sample must have null value",
            native_adapter.validate_measurement_receipt,
            request, rebind(inconsistent_validity), cell_dir, pinned,
        )

        restore()
        inconsistent_threshold = json.loads(json.dumps(original_receipt))
        inconsistent_threshold["diagnostics"]["detection_threshold_ms"] = (
            float(original_receipt["diagnostics"]["detection_threshold_ms"]) + 1.0
        )
        rejects(
            "inconsistent detection threshold",
            "diagnostics threshold is inconsistent",
            native_adapter.validate_measurement_receipt,
            request, rebind(inconsistent_threshold), cell_dir, pinned,
        )

        # Second positive control: the restores above are real, so the same two
        # instruments still accept the untouched producer evidence.
        restore()
        assert observe()[2] == [CALIBRATION_DEPENDENCY]
        native_adapter.validate_measurement_receipt(
            request, json.loads(json.dumps(original_receipt)), cell_dir, pinned
        )
        print(
            "gpu_dpr_calibration_failure_proof=true "
            f"producer_sha256={original_receipt['producer_sha256']} "
            f"scenario={SCENARIO} failure_class="
            f"{original_receipt['diagnostics']['failure_class']} "
            "positive_control=pass path_escape=rejected symlink=rejected "
            "malformed_json=rejected digest_drift=rejected "
            "inconsistent_fields=rejected"
        )


def main() -> int:
    try:
        prove(measurement_producer())
    except Skip as reason:
        print(f"gpu_dpr_calibration_failure_proof=skipped reason={reason}")
        return SKIP_EXIT
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
