#!/usr/bin/env python3
"""Run and validate the private authenticated shared-I/O P2 campaign.

This host-only/default-off driver exercises every provider capacity and lead
pair. It accepts a receipt only when the probe proves every terminal belongs to
an admitted generation/sequence and accounts for all admissions and drops.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import sys
import time
from typing import Any

SCHEMA = "pulp.gpu-audio.p2.authenticated-slots-lead.v1"
SLOTS = (2, 4, 8, 16)
LEADS = (1, 2, 4, 8)
RUNS_PER_KIND = 5
REQUIRED_MEASURED_BLOCKS = 100_000
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
REPO_ROOT = Path(__file__).resolve().parents[2]
DRIVER_PATH = Path(__file__).resolve()
DRIVER_RELATIVE_PATH = Path("tools/scripts/gpu_audio_p2_campaign.py")
NEGATIVE_CONTROL_FAILED_BLOCKS = 1

# Keep this in lockstep with detail::SharedIoTraceKind.  The raw P2
# lifecycle census retains every row, while only Terminal and Delivery rows
# participate in the admission/terminal/delivery identity multisets.
TRACE_KINDS = frozenset((0, 1, 2, 3))
LIFECYCLE_TRACE_KINDS = frozenset((0, 2))

# A P2 receipt is promotion input only when the provider and the artifact
# generation that produced it are bound to one immutable manifest.  Keep this
# list deliberately small: adding a field without including it in the digest
# would make a receipt look bound while allowing it to drift independently.
PROVENANCE_BINDING_FIELDS = (
    "provider_asset_sha256",
    "dawn_archive_sha256",
    "provider_asset_manifest_sha256",
    "dawn_archive_manifest_sha256",
)


def _canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False).encode("utf-8")


def provenance_manifest_sha256(provenance: dict[str, Any]) -> str:
    """Return the digest of the exact asset/archive bindings in a raw row."""

    bindings = provenance.get("manifest_bindings")
    if not isinstance(bindings, dict):
        raise RuntimeError("raw provenance manifest_bindings must be an object")
    if set(bindings) != set(PROVENANCE_BINDING_FIELDS):
        raise RuntimeError("raw provenance manifest_bindings must contain the complete asset/archive set")
    for field in PROVENANCE_BINDING_FIELDS:
        if not isinstance(bindings.get(field), str) or SHA256_RE.fullmatch(bindings[field]) is None:
            raise RuntimeError(f"raw provenance manifest binding {field} is not an exact SHA-256")
    return hashlib.sha256(_canonical_json(bindings)).hexdigest()


def _positive_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def _nonnegative_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _validate_provider_identity(provenance: dict[str, Any]) -> None:
    """Require observed native provider identity, never metadata-only labels."""

    if provenance.get("provider_identity_status") != "passed" or provenance.get(
            "provider_observed_identity") != "passed":
        raise RuntimeError("raw provenance lacks passed provider identity status")
    for field in ("provider_revision", "adapter_name", "adapter_backend"):
        value = provenance.get(field)
        if not isinstance(value, str) or not value.strip():
            raise RuntimeError(f"raw provenance {field} must be non-empty")
    adapter_backend = provenance["adapter_backend"].casefold()
    if adapter_backend != "metal":
        raise RuntimeError("raw provenance adapter_backend must be Metal")
    if not SHA256_RE.fullmatch(provenance.get("provider_revision", "")):
        # Provider revisions are normally git SHAs; accepting only a full
        # SHA-256 here would reject the existing 40-character Dawn revision.
        if not re.fullmatch(r"[0-9a-fA-F]{40}", provenance.get("provider_revision", "")):
            raise RuntimeError("raw provenance provider_revision is not an immutable revision")
    if not _positive_integer(provenance.get("adapter_vendor_id")):
        raise RuntimeError("raw provenance adapter_vendor_id must be a non-zero observed ID")
    if not _nonnegative_integer(provenance.get("adapter_device_id")):
        raise RuntimeError("raw provenance adapter_device_id must be a non-negative observed ID")

    # The native runtime identity is intentionally accepted in either the
    # structured form emitted by new probes or the flat compatibility form.
    # Both forms must carry an explicit passed status and a backend/name.
    native = provenance.get("native_runtime")
    if isinstance(native, dict):
        if native.get("identity_status") != "passed":
            raise RuntimeError("raw provenance native runtime identity is not passed")
        for field in ("name", "backend"):
            if not isinstance(native.get(field), str) or not native[field].strip():
                raise RuntimeError(f"raw provenance native_runtime.{field} must be non-empty")
        native_backend = native["backend"]
    else:
        if provenance.get("native_runtime_identity_status") != "passed":
            raise RuntimeError("raw provenance native runtime identity is missing")
        for field in ("native_runtime_name", "native_runtime_backend"):
            if not isinstance(provenance.get(field), str) or not provenance[field].strip():
                raise RuntimeError(f"raw provenance {field} must be non-empty")
        native_backend = provenance["native_runtime_backend"]
    if native_backend.casefold() != adapter_backend:
        raise RuntimeError("raw provenance native runtime backend disagrees with adapter backend")


def _validate_run_identity(provenance: dict[str, Any]) -> None:
    """Reject a relabeled cold process as a same-process steady run."""

    run_kind = provenance.get("run_kind")
    if run_kind not in {"cold", "steady"}:
        raise RuntimeError("raw provenance run_kind must be cold or steady")
    if run_kind != "steady":
        return
    if provenance.get("steady_semantics") != "same_process_resident":
        raise RuntimeError("steady provenance must declare same_process_resident")
    if provenance.get("same_process_resident") is not True:
        raise RuntimeError("steady provenance lacks same_process_resident=true")
    if not _positive_integer(provenance.get("process_id")):
        raise RuntimeError("steady provenance lacks a process identity")
    if not isinstance(provenance.get("residency_session_id"), str) or not provenance[
            "residency_session_id"]:
        raise RuntimeError("steady provenance lacks a residency session identity")
    if provenance.get("prepared_sessions") != 1 or provenance.get("reprepare_count") != 0:
        raise RuntimeError("steady provenance does not prove one prepared resident session")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def validate_host_preflight(path: Path, expected_source_revision: str,
                            max_age_seconds: int = 900) -> dict[str, Any]:
    """Require a fresh, quiet, source-bound host admission receipt.

    The host probe is deliberately an input owned by the campaign/operations
    lane.  This driver only verifies the stable contract; it does not infer
    quietness from a local process list or turn a missing field into a pass.
    """

    if not path.is_file():
        raise RuntimeError(f"host preflight is missing: {path}")
    try:
        receipt = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError("host preflight is not valid JSON") from exc
    if not isinstance(receipt, dict):
        raise RuntimeError("host preflight must be a JSON object")
    required = {
        "schema": receipt.get("schema") == "pulp.gpu-audio.p2.host-preflight.v1",
        "status": receipt.get("status") == "passed",
        "source_revision": receipt.get("source_revision") == expected_source_revision,
        "host_id": isinstance(receipt.get("host_id"), str) and bool(receipt["host_id"].strip()),
        "quiet_host": receipt.get("quiet_host") is True,
        "host_vitals": receipt.get("host_vitals_level") == "green",
        "contention": receipt.get("gpu_contention") is False
        and receipt.get("ui_contention") is False,
        "thermal": isinstance(receipt.get("thermal_state"), str)
        and bool(receipt["thermal_state"].strip()),
    }
    sampled_at = receipt.get("sampled_at")
    try:
        sampled_epoch = datetime.fromisoformat(sampled_at.replace("Z", "+00:00")).timestamp()
    except (AttributeError, TypeError, ValueError, OverflowError) as exc:
        raise RuntimeError("host preflight sampled_at must be an ISO-8601 timestamp") from exc
    now = time.time()
    required["fresh"] = 0 <= now - sampled_epoch <= max_age_seconds
    failed = [name for name, ok in required.items() if not ok]
    if failed:
        raise RuntimeError(f"host preflight failed: {', '.join(failed)}")
    return receipt


def require_unchanged_host_preflight(path: Path, expected_sha256: str) -> None:
    """Keep the admission artifact immutable for the whole campaign."""

    try:
        observed = sha256(path)
    except OSError as exc:
        raise RuntimeError("host preflight disappeared during campaign") from exc
    if observed != expected_sha256:
        raise RuntimeError("host preflight changed during campaign")


def _source_provenance() -> tuple[str, str]:
    """Return exact HEAD and driver hashes, refusing tracked source drift."""

    status = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "status", "--porcelain=v1", "--untracked-files=no"],
        capture_output=True, text=True, check=False, encoding="utf-8"
    )
    if status.returncode != 0:
        raise RuntimeError("unable to establish source tree status")
    if status.stdout:
        raise RuntimeError("source tree has tracked modifications; measurement requires a clean tree")

    # The campaign driver is part of the authenticated source, not an input
    # supplied by the host.  Requiring the canonical tracked path and the
    # exact HEAD blob prevents a copied/untracked driver from laundering a
    # receipt with the same source revision and a caller-supplied digest.
    try:
        expected_driver_path = (REPO_ROOT / DRIVER_RELATIVE_PATH).resolve()
    except OSError as exc:
        raise RuntimeError("campaign driver path cannot be resolved") from exc
    if DRIVER_PATH != expected_driver_path or not DRIVER_PATH.is_file():
        raise RuntimeError("campaign driver must be the canonical tracked file")
    tracked = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "ls-files", "--error-unmatch", "--stage", "--",
         str(DRIVER_RELATIVE_PATH)],
        capture_output=True, text=True, check=False, encoding="utf-8"
    )
    if tracked.returncode != 0 or not tracked.stdout.strip():
        raise RuntimeError("campaign driver is not tracked by the source repository")
    stage_fields = tracked.stdout.strip().split()
    if len(stage_fields) < 3 or stage_fields[0] != "100644":
        raise RuntimeError("campaign driver is not an owned regular source blob")
    try:
        head_blob = subprocess.check_output(
            ["git", "-C", str(REPO_ROOT), "rev-parse", f"HEAD:{DRIVER_RELATIVE_PATH}"],
            text=True, encoding="utf-8"
        ).strip()
        worktree_blob = subprocess.check_output(
            ["git", "-C", str(REPO_ROOT), "hash-object", "--", str(DRIVER_PATH)],
            text=True, encoding="utf-8"
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError("unable to establish tracked campaign driver blob") from exc
    if not re.fullmatch(r"[0-9a-fA-F]{40}", head_blob) or worktree_blob != head_blob:
        raise RuntimeError("campaign driver blob is not owned by the measured HEAD")

    try:
        source_revision = subprocess.check_output(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"], text=True, encoding="utf-8"
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError("unable to resolve source revision") from exc
    if not re.fullmatch(r"[0-9a-fA-F]{40}", source_revision):
        raise RuntimeError("source revision is not an immutable commit SHA")
    return source_revision, sha256(DRIVER_PATH)


def _require_unchanged_source(initial: tuple[str, str]) -> None:
    """Re-authenticate source and driver ownership after a complete campaign."""

    final = _source_provenance()
    if final != initial:
        raise RuntimeError("source or campaign driver provenance changed during campaign")


def validate_manifest_provenance(manifest: dict[str, Any], expected_source_revision: str,
                                 expected_driver_sha256: str) -> None:
    """Reject a campaign manifest whose driver/source binding drifted."""

    if not re.fullmatch(r"[0-9a-fA-F]{40}", expected_source_revision):
        raise RuntimeError("expected source revision is not an immutable commit SHA")
    if SHA256_RE.fullmatch(expected_driver_sha256) is None:
        raise RuntimeError("expected driver hash is not an exact SHA-256")
    if manifest.get("source_revision") != expected_source_revision:
        raise RuntimeError("campaign source revision does not match the measured source")
    if manifest.get("driver_sha256") != expected_driver_sha256:
        raise RuntimeError("campaign driver hash does not match the measured driver")
    if manifest.get("source_tree_clean") is not True:
        raise RuntimeError("campaign manifest does not attest to a clean tracked source tree")


def _parse_axis(value: str, allowed: tuple[int, ...], label: str) -> tuple[int, ...]:
    """Parse a comma-separated subset of one campaign matrix axis.

    The default still runs the complete acceptance matrix.  Selecting a subset
    is useful for a bounded single-cell smoke or a resumed campaign, but the
    probe and receipt validators remain unchanged and fail closed per cell.
    """

    values = [part.strip() for part in value.split(",")]
    if not values or any(not part for part in values):
        raise argparse.ArgumentTypeError(f"{label} must be a comma-separated non-empty list")
    try:
        parsed = tuple(int(part, 10) for part in values)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{label} must contain integers") from exc
    if any(item not in allowed for item in parsed):
        allowed_text = ",".join(str(item) for item in allowed)
        raise argparse.ArgumentTypeError(f"{label} values must be drawn from {{{allowed_text}}}")
    if len(set(parsed)) != len(parsed):
        raise argparse.ArgumentTypeError(f"{label} must not contain duplicates")
    return tuple(item for item in allowed if item in parsed)


class LosslessLifecycleObserver:
    """Retain every raw row and expose per-admission lifecycle identities.

    The observer is deliberately a small host-side hook.  It does not infer
    missing rows or synthesize terminal decisions; ``validate_identity_rows``
    remains the authority for exactly-once admission/terminal/delivery proof.
    """

    def __init__(self) -> None:
        self._rows: list[dict] = []

    def observe(self, row: dict) -> None:
        if not isinstance(row, dict):
            raise RuntimeError("raw observer row must be an object")
        self._rows.append(row)

    @property
    def rows(self) -> tuple[dict, ...]:
        return tuple(self._rows)

    def identities(self) -> dict[tuple[int, int, int], dict[str, dict]]:
        """Return retained terminal/delivery rows keyed by admission identity."""

        observed: dict[tuple[int, int, int], dict[str, dict]] = {}
        for row in self._rows:
            if row.get("kind") != "record" or row.get("trace_kind") not in LIFECYCLE_TRACE_KINDS:
                continue
            identity = (row.get("engine_id"), row.get("generation"), row.get("sequence"))
            label = "terminal" if row.get("trace_kind") == 0 else "delivery"
            observed.setdefault(identity, {})[label] = row
        return observed

    def validate(self, expected_probe_sha256: str | None = None,
                 expected_manifest_sha256: str | None = None,
                 receipt: dict[str, Any] | None = None) -> None:
        validate_identity_rows(list(self._rows), expected_probe_sha256,
                               expected_manifest_sha256, receipt)


def observe_jsonl(path: Path) -> LosslessLifecycleObserver:
    """Read a raw JSONL stream without dropping blank or malformed rows."""

    observer = LosslessLifecycleObserver()
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                raise RuntimeError(f"raw observer encountered blank line {line_number}")
            try:
                observer.observe(json.loads(line))
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"raw observer encountered invalid JSON at line {line_number}") from exc
    return observer


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--probe", type=Path)
    p.add_argument("--output-dir", type=Path)
    p.add_argument("--host-preflight", type=Path,
                   help="fresh source-bound quiet-host receipt (required for hardware runs)")
    p.add_argument("--frames", type=int, default=32, choices=(32, 64, 128))
    p.add_argument("--blocks", type=int, default=REQUIRED_MEASURED_BLOCKS)
    p.add_argument("--warmup", type=int, default=16)
    p.add_argument("--slots", type=lambda value: _parse_axis(value, SLOTS, "--slots"),
                   default=SLOTS,
                   help="comma-separated provider-slot cells (default: 2,4,8,16)")
    p.add_argument("--leads", type=lambda value: _parse_axis(value, LEADS, "--leads"),
                   default=LEADS,
                   help="comma-separated lead-block cells (default: 1,2,4,8)")
    p.add_argument("--wake-on-write", action="store_true")
    p.add_argument("--expected-source-revision")
    p.add_argument("--expected-driver-sha256")
    p.add_argument(
        "--plan-only",
        action="store_true",
        help="validate and print the complete authenticated matrix without running hardware",
    )
    return p.parse_args(argv)


def logical_trial_repetitions(run_kind: str, process_repetition: int) -> tuple[int, ...]:
    """Map one process receipt to its logical matrix repetitions."""

    if run_kind == "cold":
        return (process_repetition,)
    if run_kind == "steady":
        return tuple(range(1, RUNS_PER_KIND + 1))
    raise ValueError(f"unknown run kind: {run_kind}")


def validate_receipt(receipt: dict, slots: int, lead: int, expected_run_kind: str) -> None:
    if receipt.get("schema") != "pulp.gpu-audio-paced-convolution.v1":
        raise RuntimeError(f"{slots=} {lead=}: unexpected probe schema")
    required = {
        "completed": receipt.get("status") == "completed",
        "authenticated": receipt.get("gpu_receipt_authenticated") is True,
        "declared_slots": receipt.get("declared_slots") == slots,
        "declared_lead": receipt.get("declared_lead_blocks") == lead,
        "run_kind": receipt.get("run_kind") == expected_run_kind,
        # A steady run is useful only when one process owns one prepared
        # provider session across the measured blocks.  Fresh subprocesses are
        # cold starts, even if the command line calls them "steady".
        "steady_semantics": expected_run_kind != "steady" or (
            receipt.get("steady_semantics") == "same_process_resident"
            and receipt.get("same_process_resident") is True
            and _positive_integer(receipt.get("process_id"))
            and isinstance(receipt.get("residency_session_id"), str)
            and bool(receipt.get("residency_session_id"))
            and receipt.get("prepared_sessions") == 1
            and receipt.get("reprepare_count") == 0
        ),
        "terminal_identity": receipt.get("authenticated_terminal_records")
        == receipt.get("terminal_record_count"),
        "terminal_accounting": receipt.get("terminal_record_count")
        == receipt.get("retired_success", -1) + receipt.get("retired_failure", -2),
        "admission_accounting": receipt.get("admissions_attempted")
        == receipt.get("admissions_enqueued", -1) + receipt.get("admissions_dropped", -2),
        "trace_accounting": receipt.get("trace_attempted")
        == receipt.get("trace_enqueued", -1)
        + receipt.get("trace_dropped", -2)
        + receipt.get("trace_sampled_out", -3)
        + receipt.get("trace_invalid", -4),
        "no_admission_drop": receipt.get("admissions_dropped") == 0,
        "no_trace_drop": receipt.get("trace_dropped") == 0,
        "positive_high_water": receipt.get("high_water_in_flight", 0) > 0,
        "positive_success": receipt.get("retired_success", 0) > 0,
        "measured_blocks": receipt.get("measured_blocks") == REQUIRED_MEASURED_BLOCKS * (
            RUNS_PER_KIND if expected_run_kind == "steady" else 1),
        # The probe's accounting unit is a callback, not a measured block:
        # warmup and lead callbacks are real deliveries and must be present in
        # the raw census.  Keep this relationship explicit so a future batched
        # producer must publish a new admission granularity contract rather
        # than silently weakening the current one.
        "callback_geometry": (
            _nonnegative_integer(receipt.get("warmup_blocks"))
            and _positive_integer(receipt.get("measured_blocks"))
            and _positive_integer(receipt.get("total_callbacks"))
            and receipt.get("total_callbacks") == receipt.get("warmup_blocks")
            + receipt.get("measured_blocks") + lead
            and receipt.get("measured_callback_count") == receipt.get("measured_blocks")
            and receipt.get("callback_count") == receipt.get("total_callbacks")
            and receipt.get("admission_granularity") == "one_per_callback"
        ),
        "census_counter_fields": (
            _nonnegative_integer(receipt.get("admission_record_count"))
            and _nonnegative_integer(receipt.get("terminal_record_count"))
            and _nonnegative_integer(receipt.get("delivery_record_count"))
            and receipt.get("admission_record_count") == receipt.get("admissions_enqueued")
            and receipt.get("admission_record_count") == receipt.get("callback_count")
            and receipt.get("terminal_record_count") == receipt.get("admission_record_count")
            and receipt.get("delivery_record_count") == receipt.get("callback_count")
        ),
        "steady_repetitions": expected_run_kind != "steady" or (
            receipt.get("steady_repetitions") == RUNS_PER_KIND
            and receipt.get("measured_blocks_per_repetition") == REQUIRED_MEASURED_BLOCKS
        ),
        "provider_identity": receipt.get("provider_identity_status") == "passed",
        "native_identity": receipt.get("native_runtime_identity_status") == "passed"
        or (isinstance(receipt.get("native_runtime"), dict)
            and receipt["native_runtime"].get("identity_status") == "passed"),
        "provider_ids": _positive_integer(receipt.get("adapter_vendor_id"))
        and _nonnegative_integer(receipt.get("adapter_device_id")),
    }
    failed = [name for name, ok in required.items() if not ok]
    if failed:
        raise RuntimeError(f"{slots=} {lead=} receipt failed: {', '.join(failed)}")


def validate_negative_control_receipt(receipt: dict[str, Any]) -> None:
    """Require a planted control to fail for the observed oracle reason."""

    if receipt.get("status") == "completed" or receipt.get("negative_control") is not True:
        raise RuntimeError("negative control unexpectedly passed or was not marked")
    if receipt.get("oracle_failed_blocks") != NEGATIVE_CONTROL_FAILED_BLOCKS:
        raise RuntimeError("negative control did not prove the expected oracle failure")


def validate_identity_rows(rows: list[dict], expected_probe_sha256: str | None = None,
                          expected_manifest_sha256: str | None = None,
                          receipt: dict[str, Any] | None = None) -> None:
    provenance_rows = [r for r in rows if r.get("kind") == "provenance"]
    if len(provenance_rows) != 1 or provenance_rows[0].get("schema") != "pulp.gpu-audio.p2.raw.v1":
        raise RuntimeError("raw provenance row missing or has unexpected schema")
    provenance = provenance_rows[0]
    provenance_engine_id = provenance.get("engine_id")
    if not _positive_integer(provenance_engine_id):
        raise RuntimeError("raw provenance engine identity must be a positive integer")
    executable_sha256 = provenance.get("executable_observed_sha256")
    if not isinstance(executable_sha256, str) or SHA256_RE.fullmatch(executable_sha256) is None:
        raise RuntimeError("raw provenance lacks an exact observed executable hash")
    _validate_provider_identity(provenance)
    _validate_run_identity(provenance)
    if expected_probe_sha256 is not None and executable_sha256 != expected_probe_sha256:
        raise RuntimeError("raw provenance executable hash does not match the campaign probe")
    for field in ("provider_asset_sha256", "dawn_archive_sha256"):
        value = provenance.get(field)
        if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
            raise RuntimeError(f"raw provenance {field} is not an exact SHA-256")
    bindings = provenance.get("manifest_bindings")
    if not isinstance(bindings, dict):
        raise RuntimeError("raw provenance is missing manifest-bound asset/archive hashes")
    for field in ("provider_asset_sha256", "dawn_archive_sha256"):
        if bindings.get(field) != provenance.get(field):
            raise RuntimeError(f"raw provenance {field} disagrees with manifest binding")
    manifest_digest = provenance_manifest_sha256(provenance)
    if provenance.get("provenance_manifest_sha256") != manifest_digest:
        raise RuntimeError("raw provenance manifest digest does not bind asset/archive hashes")
    if expected_manifest_sha256 is not None and provenance.get("provenance_manifest_sha256") != expected_manifest_sha256:
        raise RuntimeError("raw provenance manifest digest does not match campaign manifest")
    records = [r for r in rows if r.get("kind") != "provenance"]
    if any(r.get("kind") not in ("admission", "record") for r in records):
        raise RuntimeError("raw census contains unknown row kind")
    if not records:
        raise RuntimeError("raw record census is empty")
    record_rows = [r for r in records if r.get("kind") == "record"]
    for record in record_rows:
        trace_kind = record.get("trace_kind")
        if (isinstance(trace_kind, bool) or not isinstance(trace_kind, int) or
                trace_kind not in TRACE_KINDS):
            raise RuntimeError("raw census contains unknown trace kind")
        if trace_kind in (1, 3):
            # Eligible and Recovery rows are valid lifecycle evidence but are
            # excluded from the Terminal/Delivery identity multisets. Mirror
            # SharedIoTraceRecord::valid() for the fields serialized by the
            # host-only probe so malformed known rows fail closed.
            if (not _positive_integer(record.get("generation")) or
                    record.get("gpu_work_admitted") is True or
                    record.get("output_eligible") is True or
                    record.get("gpu_terminal", 0) != 0 or
                    record.get("delivery", 0) != 0 or
                    not _nonnegative_integer(record.get("valid_stages"))):
                raise RuntimeError("raw census contains malformed known trace kind")
            if trace_kind == 3 and record.get("next_generation", 0) <= record["generation"]:
                raise RuntimeError("raw census contains malformed Recovery trace row")
    admissions = [(r.get("engine_id"), r.get("generation"), r.get("sequence"))
                  for r in rows if r.get("kind") == "admission"]
    terminals = [(r.get("engine_id"), r.get("generation"), r.get("sequence")) for r in record_rows
                 if r.get("trace_kind") == 0]
    deliveries = [(r.get("engine_id"), r.get("generation"), r.get("sequence")) for r in record_rows
                  if r.get("trace_kind") == 2]
    if not admissions:
        raise RuntimeError("raw admission census is empty")
    if len(admissions) != len(set(admissions)):
        raise RuntimeError("raw admission census contains duplicate identities")
    if len(terminals) != len(set(terminals)):
        raise RuntimeError("raw terminal census contains duplicate identities")
    if len(deliveries) != len(set(deliveries)):
        raise RuntimeError("raw delivery census contains duplicate identities")
    engine_ids = {r.get("engine_id") for r in rows if r.get("kind") in ("admission", "record")}
    if engine_ids != {provenance_engine_id}:
        raise RuntimeError("raw rows have inconsistent engine identity")
    if any(r.get("trace_kind") == 0 and (r.get("generation", 0) == 0 or
           r.get("valid_stages", 0) == 0 or r.get("gpu_terminal", 0) == 0 or
           r.get("admission_identity_matched") is not True) for r in record_rows):
        raise RuntimeError("raw terminal row lacks authenticated fields")
    admission_set = set(admissions)
    admitted_deliveries = [identity for identity in deliveries if identity in admission_set]
    if len(admitted_deliveries) != len(admissions) or sorted(admissions) != sorted(admitted_deliveries):
        raise RuntimeError("admission/admitted-delivery identity multisets differ")
    if sorted(admissions) != sorted(terminals):
        raise RuntimeError("admission/terminal identity multisets differ")
    if receipt is not None:
        expected_warmup = receipt.get("warmup_blocks")
        expected_measured = receipt.get("measured_blocks")
        expected_lead = receipt.get("declared_lead_blocks")
        expected_callbacks = receipt.get("callback_count", receipt.get("total_callbacks"))
        expected_measured_callbacks = receipt.get("measured_callback_count")
        expected_admission_rows = receipt.get("admission_record_count")
        expected_admissions = receipt.get("admissions_enqueued")
        expected_terminals = receipt.get("terminal_record_count")
        expected_authenticated = receipt.get("authenticated_terminal_records")
        expected_deliveries = receipt.get("delivery_record_count")
        if not all(_nonnegative_integer(value) for value in
                   (expected_warmup, expected_measured, expected_lead, expected_callbacks,
                    expected_measured_callbacks, expected_admission_rows,
                    expected_admissions, expected_terminals, expected_authenticated,
                    expected_deliveries)):
            raise RuntimeError("receipt lacks non-negative raw census counters")
        if receipt.get("admission_granularity") != "one_per_callback":
            raise RuntimeError("receipt admission granularity is not authenticated")
        if expected_callbacks != expected_warmup + expected_measured + expected_lead:
            raise RuntimeError("receipt callback geometry is inconsistent")
        if expected_measured_callbacks != expected_measured:
            raise RuntimeError("receipt measured callback count disagrees with measured blocks")
        if receipt.get("total_callbacks") != expected_callbacks:
            raise RuntimeError("receipt callback count disagrees with total callbacks")
        if expected_admission_rows != expected_admissions:
            raise RuntimeError("receipt admission row count disagrees with admission counter")
        if expected_admission_rows != expected_callbacks:
            raise RuntimeError("receipt admission row count disagrees with callback count")
        if len(admissions) != expected_admissions:
            raise RuntimeError("raw admission census disagrees with receipt")
        if len(terminals) != expected_terminals or len(terminals) != expected_authenticated:
            raise RuntimeError("raw terminal census disagrees with receipt")
        if len(admitted_deliveries) != expected_admissions:
            raise RuntimeError("raw admitted-delivery census disagrees with receipt")
        if len(deliveries) != expected_deliveries or len(deliveries) != expected_callbacks:
            raise RuntimeError("raw delivery census disagrees with callback count")
    terminal_by_identity = {
        (r.get("engine_id"), r.get("generation"), r.get("sequence")): r
        for r in record_rows if r.get("trace_kind") == 0
    }
    for delivery in (r for r in record_rows if r.get("trace_kind") == 2):
        identity = (delivery.get("engine_id"), delivery.get("generation"), delivery.get("sequence"))
        if delivery.get("delivery", 0) == 0:
            raise RuntimeError("raw delivery row lacks an audio disposition")
        if delivery.get("callback_timing_available") is not True:
            raise RuntimeError("raw delivery row lacks callback timing")
        if not _positive_integer(delivery.get("callback_end_ns")) or not _positive_integer(
                delivery.get("result_visible_ns")):
            raise RuntimeError("raw delivery row lacks publication timestamps")
        expected_admitted = identity in admission_set
        if delivery.get("admitted") is not expected_admitted:
            raise RuntimeError("raw delivery admitted classification disagrees with admissions")
        if delivery.get("callback_only") is not (not expected_admitted):
            raise RuntimeError("raw delivery callback-only classification disagrees with admissions")
        if not expected_admitted:
            continue
        terminal = terminal_by_identity[identity]
        # GPU delivery is legal only after an accepted terminal.  A fallback,
        # silence, or priming decision may pair with any typed terminal cause.
        if delivery.get("delivery") == 1 and terminal.get("gpu_terminal") != 1:
            raise RuntimeError("GPU delivery is not backed by a completed terminal")


def run(args: argparse.Namespace) -> int:
    selected_slots = tuple(args.slots)
    selected_leads = tuple(args.leads)
    if not selected_slots or not selected_leads:
        raise RuntimeError("campaign matrix selection must contain at least one slot and lead")
    matrix_complete = selected_slots == SLOTS and selected_leads == LEADS
    cells = [{"slots": slots, "lead": lead}
             for slots in selected_slots for lead in selected_leads]
    if args.plan_only:
        print(json.dumps({
            "schema": SCHEMA,
            "status": "contract_only",
            "performance_verdict": "unassigned",
            "cells": cells,
            "cell_count": len(cells),
            "run_kinds": ["cold", "steady"],
            "runs_per_kind": RUNS_PER_KIND,
            "required_measured_blocks": REQUIRED_MEASURED_BLOCKS,
            "trial_count": len(cells) * 2 * RUNS_PER_KIND,
        }, sort_keys=True))
        return 0
    if args.probe is None or args.output_dir is None:
        raise RuntimeError("--probe and --output-dir are required for a hardware campaign")
    if not args.probe.is_file() or not args.probe.stat().st_mode & 0o111:
        raise RuntimeError(f"probe is not executable: {args.probe}")
    if args.output_dir.exists():
        raise RuntimeError(f"output directory must be new: {args.output_dir}")
    if args.blocks != REQUIRED_MEASURED_BLOCKS:
        raise RuntimeError(f"acceptance campaign requires exactly {REQUIRED_MEASURED_BLOCKS} measured blocks")
    source_revision, driver_sha256 = _source_provenance()
    initial_source_provenance = (source_revision, driver_sha256)
    if args.host_preflight is None:
        raise RuntimeError("--host-preflight is required for a hardware campaign")
    host_preflight = validate_host_preflight(args.host_preflight, source_revision)
    host_preflight_sha256 = sha256(args.host_preflight)
    if args.expected_source_revision is not None and args.expected_source_revision != source_revision:
        raise RuntimeError("expected source revision does not match the measured source")
    if args.expected_driver_sha256 is not None and args.expected_driver_sha256 != driver_sha256:
        raise RuntimeError("expected driver hash does not match the measured driver")
    probe_sha256 = sha256(args.probe)
    args.output_dir.mkdir(parents=True)
    trials = []
    negative_control = None
    campaign_manifest_digest: str | None = None
    try:
        control_dir = args.output_dir / "negative-control"
        control = [str(args.probe), f"--frames={args.frames}", f"--slots={selected_slots[0]}",
                   f"--lead={selected_leads[0]}",
                   f"--blocks={args.blocks}", f"--warmup={args.warmup}",
                   "--run-kind=cold",
                   f"--output-dir={control_dir}", f"--raw-jsonl={control_dir / 'raw.jsonl'}",
                   "--negative-control", "--expect-failure"]
        control_proc = subprocess.run(control, capture_output=True, text=True, timeout=900, encoding="utf-8")
        control_receipt_path = control_dir / "receipt.json"
        if control_proc.returncode != 0 or not control_receipt_path.is_file():
            raise RuntimeError("negative control did not return expected-failure status")
        control_receipt = json.loads(control_receipt_path.read_text(encoding="utf-8"))
        control_raw = control_dir / "raw.jsonl"
        validate_negative_control_receipt(control_receipt)
        if not control_raw.is_file():
            raise RuntimeError("negative control did not produce a raw census")
        control_observer = observe_jsonl(control_raw)
        control_observer.validate(probe_sha256, receipt=control_receipt)
        control_rows = control_observer.rows
        campaign_manifest_digest = control_rows[0].get("provenance_manifest_sha256")
        negative_control = {"argv": control, "returncode": control_proc.returncode,
                            "receipt_sha256": sha256(control_receipt_path),
                            "stdout_sha256": hashlib.sha256(control_proc.stdout.encode()).hexdigest(),
                            "stderr_sha256": hashlib.sha256(control_proc.stderr.encode()).hexdigest()}
        for slots in selected_slots:
            for lead in selected_leads:
                for run_kind in ("cold", "steady"):
                    logical_repetitions = range(1, RUNS_PER_KIND + 1)
                    # Cold repetitions are independent processes.  Steady
                    # repetitions are one persistent probe process containing
                    # five contiguous measured segments; this is the only
                    # way to make same-process residency an actual property.
                    process_repetitions = (logical_repetitions if run_kind == "cold" else (1,))
                    for repetition in process_repetitions:
                        trial_dir = args.output_dir / f"slots-{slots}-lead-{lead}-{run_kind}-{repetition}"
                        command = [str(args.probe), f"--frames={args.frames}", f"--slots={slots}",
                                   f"--lead={lead}", f"--blocks={args.blocks}",
                                   f"--warmup={(0 if run_kind == 'cold' else args.warmup)}",
                                   f"--run-kind={run_kind}", f"--output-dir={trial_dir}",
                                   f"--raw-jsonl={trial_dir / 'raw.jsonl'}"]
                        if run_kind == "steady":
                            command.append(f"--steady-repetitions={RUNS_PER_KIND}")
                        if args.wake_on_write:
                            command.append("--wake-on-write")
                        proc = subprocess.run(command, capture_output=True, text=True, timeout=900, encoding="utf-8")
                        receipt_path = trial_dir / "receipt.json"
                        if proc.returncode != 0 or not receipt_path.is_file():
                            raise RuntimeError(f"{slots=} {lead=} {run_kind=} {repetition=} probe failed ({proc.returncode})")
                        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
                        validate_receipt(receipt, slots, lead, run_kind)
                        raw_path = trial_dir / "raw.jsonl"
                        if not raw_path.is_file():
                            raise RuntimeError(f"{slots=} {lead=} missing streamed provenance/rows")
                        observer = observe_jsonl(raw_path)
                        if len(observer.rows) < 2 or observer.rows[0].get("kind") != "provenance":
                            raise RuntimeError(f"{slots=} {lead=} missing streamed provenance/rows")
                        observer.validate(probe_sha256, campaign_manifest_digest, receipt)
                        parsed_rows = observer.rows
                        provenance = parsed_rows[0]
                        if provenance.get("run_kind") != run_kind:
                            raise RuntimeError(
                                f"{slots=} {lead=} {run_kind=} provenance run kind disagrees"
                            )
                        (trial_dir / "command.json").write_text(json.dumps({"argv": command, "returncode": proc.returncode}, indent=2) + "\n", encoding="utf-8")
                        (trial_dir / "probe.stdout").write_text(proc.stdout, encoding="utf-8")
                        (trial_dir / "probe.stderr").write_text(proc.stderr, encoding="utf-8")
                        # Each cold process is one logical repetition. A
                        # steady process contains all five logical repetitions
                        # in one resident session. Expanding cold receipts
                        # five times makes a complete 96-process matrix look
                        # like 480 logical trials and fails at finalization.
                        trial_repetitions = logical_trial_repetitions(run_kind, repetition)
                        for logical_repetition in trial_repetitions:
                            trials.append({"slots": slots, "lead": lead, "run_kind": run_kind,
                                           "repetition": logical_repetition, "receipt_sha256": sha256(receipt_path),
                                           "raw_jsonl_sha256": sha256(raw_path),
                                           "blocks_sha256": sha256(trial_dir / "blocks.csv") if (trial_dir / "blocks.csv").is_file() else None,
                                           "process_id": receipt.get("process_id"),
                                           "steady_repetitions": receipt.get("steady_repetitions", 1),
                                           "receipt": receipt})
    except Exception:
        # Preserve no partial campaign receipt: incomplete matrices are not evidence.
        for child in args.output_dir.iterdir():
            if child.is_dir():
                for path in child.iterdir():
                    path.unlink()
                child.rmdir()
        args.output_dir.rmdir()
        raise
    _require_unchanged_source(initial_source_provenance)
    require_unchanged_host_preflight(args.host_preflight, host_preflight_sha256)
    if len(trials) != len(selected_slots) * len(selected_leads) * 2 * RUNS_PER_KIND:
        raise RuntimeError("campaign completed with an incomplete cold/steady matrix")
    if not campaign_manifest_digest:
        raise RuntimeError("campaign has no manifest-bound provider provenance")
    manifest = {"schema": SCHEMA, "performance_verdict": "unassigned",
                "acceptance_status": ("authenticated_screening" if matrix_complete
                                       else "authenticated_screening_subset"),
                "generated_utc": datetime.now(timezone.utc).isoformat(),
                "source_revision": source_revision,
                "driver_sha256": driver_sha256,
                "source_tree_clean": True,
                "probe_sha256": probe_sha256, "probe_path": str(args.probe.resolve()),
                "host_preflight_sha256": host_preflight_sha256,
                "host_preflight_path": str(args.host_preflight.resolve()),
                "host_id": host_preflight["host_id"],
                "machine_id": platform.node() or "unavailable", "host_platform": platform.platform(),
                "negative_control": negative_control,
                "provenance_manifest_sha256": campaign_manifest_digest,
                "slots": list(selected_slots), "leads": list(selected_leads),
                "runs_per_kind": RUNS_PER_KIND,
                "run_kinds": ["cold", "steady"], "required_measured_blocks": REQUIRED_MEASURED_BLOCKS,
                "paced": True, "trials": trials}
    validate_manifest_provenance(manifest, source_revision, driver_sha256)
    (args.output_dir / "campaign.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"schema": SCHEMA, "status": "completed", "trials": len(trials),
                      "acceptance_status": manifest["acceptance_status"],
                      "performance_verdict": "unassigned"}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run(parse_args(sys.argv[1:])))
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"gpu_audio_p2_campaign: {exc}", file=sys.stderr)
        raise SystemExit(2)
