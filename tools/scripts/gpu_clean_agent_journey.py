#!/usr/bin/env python3
"""Prove the A5 symptom-to-repair journey through a real Pulp GPU recipe.

This driver intentionally knows no recipe id.  It starts with one exact symptom,
uses the selected CLI's embedded catalog, scaffolds the selected workspace, runs
the seeded negative control, diagnoses its typed evidence, applies the
scaffold-documented fix, and reruns the same native recipe.  The resulting
receipt is small, machine-readable, and binds every bounded artifact by hash.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import stat
import subprocess
import sys
from dataclasses import dataclass
from typing import Any


RESULT_SCHEMA = "pulp.gpu-probe-result.v1"
DISCOVERY_SCHEMA = "pulp.gpu-recipes-discovery.v1"
CATALOG_SCHEMA = "pulp.gpu-recipes.v1"
SCAFFOLD_SCHEMA = "pulp.gpu-recipe-scaffold-result.v1"
SELECTION_SCHEMA = "pulp.gpu-recipe-selection.v1"
JOURNEY_SCHEMA = "pulp.gpu-clean-agent-journey.v1"
FIX_MARKER = "Fix the seeded failure by removing only `--negative-control`"
MAX_ARTIFACTS = 16
MAX_TOTAL_ARTIFACT_BYTES = 16 * 1024 * 1024
MAX_WORKSPACE_BYTES = 64 * 1024 * 1024
ARTIFACT_KINDS = {"json", "image", "numeric-samples", "trace"}
RESULT_FIELDS = {
    "schema", "version", "gpu_evidence_id", "recipe_id", "source_digest",
    "signature_digest", "dimensions", "seed", "clock", "input_format",
    "output_format", "encoding", "tolerance", "adapter_policy", "adapter",
    "numeric_sample_count", "mutation", "verdict", "passes", "artifacts",
    "recommendations",
}


class JourneyError(RuntimeError):
    """A contract violation that means the journey is not proven."""


class JourneyUnavailable(JourneyError):
    """The requested recipe evidence is unavailable or unverified."""


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


def _run(argv: list[str], timeout_seconds: float) -> CommandResult:
    try:
        result = subprocess.run(
            argv,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            env={**os.environ, "NO_COLOR": "1"},
        )
    except subprocess.TimeoutExpired as exc:
        raise JourneyError(
            f"command exceeded the {timeout_seconds:g}s bound: {argv[0]}"
        ) from exc
    return CommandResult(result.returncode, result.stdout, result.stderr)


def _json_stdout(result: CommandResult, label: str) -> dict[str, Any]:
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        detail = result.stderr.strip() or result.stdout.strip() or "no output"
        raise JourneyError(f"{label} did not emit one JSON object: {detail}") from exc
    if not isinstance(value, dict):
        raise JourneyError(f"{label} JSON root must be an object")
    return value


def _require_absolute_new_workspace(workspace: pathlib.Path) -> None:
    if not workspace.is_absolute():
        raise JourneyError("workspace must be an absolute path")
    if workspace.exists() or workspace.is_symlink():
        raise JourneyError("workspace must not exist")
    if not workspace.parent.is_dir() or workspace.parent.is_symlink():
        raise JourneyError("workspace parent must be an existing real directory")


def _select_recipe(
    pulp: pathlib.Path, symptom: str, timeout_seconds: float
) -> tuple[dict[str, Any], dict[str, Any]]:
    result = _run(
        [str(pulp), "gpu", "recipes", "list", "--symptom", symptom, "--json"],
        timeout_seconds,
    )
    if result.returncode == 2:
        raise JourneyUnavailable(
            f"symptom discovery is unavailable: {result.stderr.strip()}"
        )
    if result.returncode != 0:
        raise JourneyError(
            f"symptom discovery failed with exit {result.returncode}: {result.stderr.strip()}"
        )
    discovery = _json_stdout(result, "symptom discovery")
    if discovery.get("schema") != DISCOVERY_SCHEMA:
        raise JourneyError("symptom discovery schema is not the supported v1 contract")
    revision = discovery.get("catalog_revision")
    if type(revision) is not int or revision < 1:
        raise JourneyError("symptom discovery has no valid catalog revision")
    matches = discovery.get("recipes")
    if not isinstance(matches, list) or len(matches) != 1:
        raise JourneyError("symptom must select exactly one recipe")
    selected = matches[0]
    if not isinstance(selected, dict) or selected.get("callable") is not True:
        raise JourneyUnavailable("the uniquely selected recipe is not callable in this CLI")
    recipe = selected.get("recipe")
    if not isinstance(recipe, dict) or not isinstance(recipe.get("id"), str):
        raise JourneyError("selected recipe has no stable id")
    symptoms = recipe.get("symptoms")
    if not isinstance(symptoms, list) or symptom not in symptoms:
        raise JourneyError("selected recipe does not declare the requested symptom")
    return discovery, recipe


def _scaffold(
    pulp: pathlib.Path,
    recipe: dict[str, Any],
    catalog_revision: int,
    workspace: pathlib.Path,
    timeout_seconds: float,
) -> dict[str, Any]:
    recipe_id = recipe["id"]
    result = _run(
        [
            str(pulp),
            "gpu",
            "recipes",
            "scaffold",
            recipe_id,
            "--output",
            str(workspace),
            "--json",
        ],
        timeout_seconds,
    )
    if result.returncode != 0:
        raise JourneyError(
            f"recipe scaffold failed with exit {result.returncode}: {result.stderr.strip()}"
        )
    scaffold = _json_stdout(result, "recipe scaffold")
    if (
        scaffold.get("schema") != SCAFFOLD_SCHEMA
        or scaffold.get("recipe_id") != recipe_id
        or scaffold.get("callable") is not True
        or scaffold.get("output") != str(workspace)
    ):
        raise JourneyError("recipe scaffold result does not bind the callable selection")

    try:
        selection = json.loads((workspace / "gpu-recipe.json").read_text(encoding="utf-8"))
        readme = (workspace / "README.md").read_text(encoding="utf-8")
    except (OSError, json.JSONDecodeError) as exc:
        raise JourneyError(f"recipe scaffold is incomplete: {exc}") from exc
    _validate_selection_binding(selection, recipe, catalog_revision)
    if FIX_MARKER not in readme:
        raise JourneyError("scaffold does not document the seeded-failure fix")
    documented_commands = [
        line for line in readme.splitlines() if line.startswith("pulp gpu probe ")
    ]
    expected_commands = [
        f'pulp gpu probe --recipe {recipe_id} --artifacts "$PWD/artifacts/baseline-1" --json',
        f'pulp gpu probe --recipe {recipe_id} --artifacts "$PWD/artifacts/baseline-2" --json',
        f'pulp gpu probe --recipe {recipe_id} --artifacts "$PWD/artifacts/negative" '
        "--negative-control --json",
    ]
    if documented_commands != expected_commands:
        raise JourneyError("scaffold does not document the selected recipe's exact commands")
    return {
        "selection_sha256": _sha256(workspace / "gpu-recipe.json"),
        "readme_sha256": _sha256(workspace / "README.md"),
        "documented_commands": documented_commands,
    }


def _validate_selection_binding(
    selection: Any, recipe: dict[str, Any], catalog_revision: int
) -> None:
    if (
        not isinstance(selection, dict)
        or set(selection) != {"schema", "catalog_schema", "catalog_revision", "recipe"}
        or selection.get("schema") != SELECTION_SCHEMA
        or selection.get("catalog_schema") != CATALOG_SCHEMA
        or type(selection.get("catalog_revision")) is not int
        or selection.get("catalog_revision") != catalog_revision
        or selection.get("recipe") != recipe
    ):
        raise JourneyError("selection receipt does not bind the discovered catalog row")


def _probe_command(
    pulp: pathlib.Path,
    recipe: dict[str, Any],
    artifact_dir: pathlib.Path,
    *,
    negative_control: bool,
) -> list[str]:
    try:
        command = list(recipe["entrypoints"]["cli"]["command"])
    except (KeyError, TypeError) as exc:
        raise JourneyError("selected recipe has no CLI command") from exc
    if not command or command[0] != "pulp" or "<absolute-artifact-dir>" not in command:
        raise JourneyError("selected recipe command is not executable by this journey")
    command[0] = str(pulp)
    command[command.index("<absolute-artifact-dir>")] = str(artifact_dir)
    if negative_control:
        json_index = command.index("--json") if "--json" in command else len(command)
        command.insert(json_index, "--negative-control")
    return command


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _verified_regular_file(
    path: pathlib.Path, artifact_dir: pathlib.Path
) -> tuple[int, str]:
    try:
        if artifact_dir.is_symlink() or not artifact_dir.is_dir():
            raise JourneyError("artifact directory must be a real directory")
        resolved_dir = artifact_dir.resolve(strict=True)
        if path.parent.resolve(strict=True) != resolved_dir:
            raise JourneyError("declared artifact escapes its evidence directory")
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError as exc:
        raise JourneyError(f"declared artifact is unavailable: {path.name}") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise JourneyError(f"declared artifact is not a regular file: {path.name}")
        digest = hashlib.sha256()
        with os.fdopen(descriptor, "rb") as stream:
            descriptor = -1
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return metadata.st_size, digest.hexdigest()
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _verified_artifacts(
    evidence: dict[str, Any], artifact_dir: pathlib.Path, workspace: pathlib.Path
) -> dict[str, str]:
    artifacts = evidence.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts or len(artifacts) > MAX_ARTIFACTS:
        raise JourneyError("probe artifact declaration is empty or exceeds its count bound")
    hashes: dict[str, str] = {}
    total_bytes = 0
    try:
        resolved_workspace = workspace.resolve(strict=True)
        resolved_artifact_dir = artifact_dir.resolve(strict=True)
        if not resolved_artifact_dir.is_relative_to(resolved_workspace):
            raise JourneyError("artifact directory escapes the evidence workspace")
    except OSError as exc:
        raise JourneyError("artifact directory is unavailable") from exc
    for artifact in artifacts:
        if not isinstance(artifact, dict) or set(artifact) != {
            "name",
            "kind",
            "mime",
            "bytes",
            "sha256",
        }:
            raise JourneyError("probe artifact declaration is malformed")
        name = artifact.get("name")
        kind = artifact.get("kind")
        mime = artifact.get("mime")
        declared_bytes = artifact.get("bytes")
        declared_hash = artifact.get("sha256")
        if (
            not isinstance(name, str)
            or not name
            or len(name) > 240
            or "\\" in name
            or name in {".", ".."}
            or pathlib.PurePosixPath(name).name != name
            or name in hashes
            or kind not in ARTIFACT_KINDS
            or not isinstance(mime, str)
            or not mime
            or len(mime) > 128
            or type(declared_bytes) is not int
            or declared_bytes < 0
            or declared_bytes > MAX_TOTAL_ARTIFACT_BYTES
            or not isinstance(declared_hash, str)
            or re.fullmatch(r"[0-9a-f]{64}", declared_hash) is None
        ):
            raise JourneyError("probe artifact declaration is unsafe or incomplete")
        path = artifact_dir / name
        actual_bytes, actual_hash = _verified_regular_file(path, artifact_dir)
        if actual_bytes != declared_bytes or actual_hash != declared_hash:
            raise JourneyError(f"declared artifact bytes or digest do not match: {name}")
        total_bytes += actual_bytes
        if total_bytes > MAX_TOTAL_ARTIFACT_BYTES:
            raise JourneyError("probe artifacts exceed the 16 MiB journey bound")
        hashes[name] = actual_hash
    try:
        actual_names = {entry.name for entry in artifact_dir.iterdir()}
    except OSError as exc:
        raise JourneyError("artifact directory cannot be enumerated") from exc
    if actual_names != set(hashes):
        raise JourneyError("artifact directory contains undeclared or missing entries")
    return dict(sorted(hashes.items()))


def _validate_evidence(
    evidence: dict[str, Any], recipe_id: str, artifact_dir: pathlib.Path,
    workspace: pathlib.Path,
) -> dict[str, str]:
    _validate_result_schema_shape(evidence)
    if (
        evidence.get("schema") != RESULT_SCHEMA
        or type(evidence.get("version")) is not int
        or evidence.get("version") != 1
        or evidence.get("recipe_id") != recipe_id
    ):
        raise JourneyError("probe evidence does not bind the selected recipe and schema")
    evidence_id = evidence.get("gpu_evidence_id")
    if not isinstance(evidence_id, str) or re.fullmatch(r"[0-9a-f]{32}", evidence_id) is None:
        raise JourneyError("probe evidence has no correlation id")
    for field in ("source_digest", "signature_digest"):
        value = evidence.get(field)
        if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
            raise JourneyError(f"probe evidence has no valid {field}")
    passes = _typed_passes(evidence)
    if not any(item["work_completed"] is True for item in passes):
        raise JourneyError("probe evidence does not prove work completed")
    aggregate = "pass"
    for item in passes:
        if item["verdict"] == "fail":
            aggregate = "fail"
        elif aggregate != "fail" and item["verdict"] == "unavailable":
            aggregate = "unavailable"
        elif aggregate == "pass" and item["verdict"] == "unverified":
            aggregate = "unverified"
    if evidence.get("verdict") != aggregate:
        raise JourneyError("top-level verdict does not aggregate the typed passes")
    return _verified_artifacts(evidence, artifact_dir, workspace)


def _validate_result_schema_shape(evidence: Any) -> None:
    """Validate the closed v1 JSON shape without a source-tree dependency."""

    if not isinstance(evidence, dict) or set(evidence) != RESULT_FIELDS:
        raise JourneyError("probe result does not have the closed v1 field set")
    dimensions = evidence["dimensions"]
    if (
        not isinstance(dimensions, dict)
        or set(dimensions) != {"width", "height", "work_items"}
        or type(dimensions["width"]) is not int
        or not 1 <= dimensions["width"] <= 4096
        or type(dimensions["height"]) is not int
        or not 1 <= dimensions["height"] <= 4096
        or type(dimensions["work_items"]) is not int
        or not 1 <= dimensions["work_items"] <= 1_048_576
    ):
        raise JourneyError("probe result dimensions violate the v1 contract")
    tolerance = evidence["tolerance"]
    if (
        not isinstance(tolerance, dict)
        or set(tolerance) != {"absolute", "relative"}
        or any(
            isinstance(tolerance[field], bool)
            or not isinstance(tolerance[field], (int, float))
            or not math.isfinite(tolerance[field])
            or tolerance[field] < 0
            for field in ("absolute", "relative")
        )
    ):
        raise JourneyError("probe result tolerance violates the v1 contract")
    adapter = evidence["adapter"]
    adapter_fields = {"status", "class", "backend", "name", "vendor", "architecture", "device"}
    if (
        not isinstance(adapter, dict)
        or set(adapter) != adapter_fields
        or adapter["status"] not in {"authentic", "unverified", "unavailable"}
        or adapter["class"] not in {"hardware", "software", "null", "unknown"}
        or any(
            value is not None and (not isinstance(value, str) or len(value) > 256)
            for value in (adapter[field] for field in adapter_fields - {"status", "class"})
        )
    ):
        raise JourneyError("probe result adapter violates the v1 contract")
    bounded_strings = ("clock", "input_format", "output_format", "encoding")
    if any(
        not isinstance(evidence[field], str) or not 1 <= len(evidence[field]) <= 64
        for field in bounded_strings
    ):
        raise JourneyError("probe result execution strings violate the v1 contract")
    if (
        type(evidence["seed"]) is not int
        or evidence["seed"] < 0
        or evidence["adapter_policy"]
        not in {"hardware-required", "hardware-preferred", "any-supported"}
        or type(evidence["numeric_sample_count"]) is not int
        or not 0 <= evidence["numeric_sample_count"] <= 4096
        or (
            evidence["mutation"] is not None
            and (
                not isinstance(evidence["mutation"], str)
                or len(evidence["mutation"]) > 128
            )
        )
        or evidence["verdict"] not in {"pass", "fail", "unavailable", "unverified"}
    ):
        raise JourneyError("probe result execution identity violates the v1 contract")
    recommendations = evidence["recommendations"]
    if (
        not isinstance(recommendations, list)
        or len(recommendations) > 16
        or any(
            not isinstance(item, str) or not 1 <= len(item) <= 512
            for item in recommendations
        )
    ):
        raise JourneyError("probe result recommendations violate the v1 contract")


def _typed_passes(evidence: dict[str, Any]) -> list[dict[str, Any]]:
    passes = evidence.get("passes")
    if not isinstance(passes, list) or not passes or len(passes) > 16:
        raise JourneyError("probe evidence has no pass-level work evidence")
    required = {
        "sequence",
        "name",
        "verdict",
        "work_completed",
        "expected",
        "observed",
        "absolute_error",
        "code",
    }
    for index, item in enumerate(passes):
        if not isinstance(item, dict) or set(item) != required:
            raise JourneyError("every probe pass must be a complete typed object")
        if (
            type(item["sequence"]) is not int
            or item["sequence"] != index
            or not isinstance(item["name"], str)
            or not item["name"]
            or len(item["name"]) > 64
            or item["verdict"] not in {"pass", "fail", "unavailable", "unverified"}
            or type(item["work_completed"]) is not bool
            or not isinstance(item["code"], str)
            or not item["code"]
            or len(item["code"]) > 128
            or any(
                value is not None
                and (
                    isinstance(value, bool)
                    or not isinstance(value, (int, float))
                    or not math.isfinite(value)
                )
                for value in (item["expected"], item["observed"], item["absolute_error"])
            )
        ):
            raise JourneyError("probe pass fields are not a valid ordered typed sequence")
        expected = item["expected"]
        observed = item["observed"]
        absolute_error = item["absolute_error"]
        if item["verdict"] == "pass" and item["work_completed"] is not True:
            raise JourneyError("a passing probe pass must prove work completed")
        if (expected is None) != (observed is None):
            raise JourneyError("numeric expected and observed values must appear together")
        if (expected is None) != (absolute_error is None):
            raise JourneyError("numeric evidence requires an absolute error")
        if absolute_error is not None:
            if absolute_error < 0.0:
                raise JourneyError("absolute error must be nonnegative")
            calculated = abs(observed - expected)
            epsilon = sys.float_info.epsilon * max(1.0, calculated, absolute_error) * 8.0
            if abs(calculated - absolute_error) > epsilon:
                raise JourneyError("absolute error does not match observed minus expected")
    return passes


def _partition_artifact_changes(
    reference_hashes: dict[str, str], seeded_hashes: dict[str, str]
) -> tuple[list[str], list[str]]:
    if set(reference_hashes) != set(seeded_hashes):
        raise JourneyError("seeded run changed the selected recipe's artifact identities")
    changed = sorted(
        name
        for name in reference_hashes
        if reference_hashes[name] != seeded_hashes[name]
    )
    stable = sorted(set(reference_hashes) - set(changed))
    if not changed:
        raise JourneyError("seeded mutation did not change a bounded output artifact")
    if not stable:
        raise JourneyError("seeded mutation changed every reference artifact")
    return stable, changed


def _pass_contract(evidence: dict[str, Any]) -> list[str]:
    return [item["name"] for item in _typed_passes(evidence)]


def _artifact_contract(evidence: dict[str, Any]) -> dict[str, dict[str, Any]]:
    artifacts = evidence.get("artifacts")
    if not isinstance(artifacts, list):
        raise JourneyError("probe evidence has no artifact contract")
    return {
        item["name"]: {
            "kind": item["kind"],
            "mime": item["mime"],
            "bytes": item["bytes"],
        }
        for item in artifacts
    }


def _require_recipe_contract(
    evidence: dict[str, Any],
    pass_contract: list[str],
    artifact_contract: dict[str, dict[str, Any]],
) -> None:
    if _pass_contract(evidence) != pass_contract:
        raise JourneyError("run changed the selected recipe's semantic pass contract")
    if _artifact_contract(evidence) != artifact_contract:
        raise JourneyError("run changed the selected recipe's artifact contract")


def _execution_contract(evidence: dict[str, Any]) -> dict[str, Any]:
    return {
        field: evidence[field]
        for field in (
            "dimensions",
            "seed",
            "clock",
            "input_format",
            "output_format",
            "encoding",
            "tolerance",
            "adapter_policy",
            "numeric_sample_count",
        )
    }


def _require_execution_contract(
    evidence: dict[str, Any], reference_contract: dict[str, Any]
) -> None:
    if _execution_contract(evidence) != reference_contract:
        raise JourneyError("run changed the selected recipe's typed execution identity")


def _require_signature_isolation(
    reference: dict[str, Any], seeded: dict[str, Any], repaired: dict[str, Any]
) -> None:
    reference_signature = reference.get("signature_digest")
    if (
        reference_signature != repaired.get("signature_digest")
        or seeded.get("signature_digest") == reference_signature
    ):
        raise JourneyError("signatures do not isolate only the seeded mutation")


def _seeded_failing_passes(evidence: dict[str, Any]) -> list[dict[str, Any]]:
    passes = _typed_passes(evidence)
    if any(item["verdict"] not in {"pass", "fail"} for item in passes):
        raise JourneyUnavailable("seeded run contains unavailable or unverified pass evidence")
    return [item for item in passes if item["verdict"] == "fail"]


def _authentic_adapter(evidence: dict[str, Any]) -> dict[str, Any]:
    adapter = evidence.get("adapter")
    identity_fields = (
        "status",
        "class",
        "backend",
        "name",
        "vendor",
        "architecture",
        "device",
    )
    placeholder_values = {"unknown", "generic", "n/a", "none", "null", "unavailable"}
    if (
        evidence.get("adapter_policy") != "hardware-required"
        or not isinstance(adapter, dict)
        or adapter.get("status") != "authentic"
        or adapter.get("class") != "hardware"
        or any(
            not isinstance(adapter.get(field), str) or not adapter[field]
            for field in identity_fields
        )
        or any(
            adapter[field].strip().casefold() in placeholder_values
            for field in identity_fields
        )
    ):
        raise JourneyError("clean-agent gate requires authentic hardware adapter evidence")
    if adapter["backend"] != "Metal":
        raise JourneyUnavailable(
            "clean-agent identity validation currently supports Metal evidence only"
        )
    # Dawn can report device_id == 0 for a real Apple adapter. Do not invent
    # an instance id; bind the concrete model through its authentic name,
    # vendor, Metal architecture, and nonzero Apple vendor id instead.
    device_match = re.fullmatch(
        r"vendor=0x([0-9a-f]+),device=0x([0-9a-f]+)", adapter["device"]
    )
    if (
        adapter["vendor"].casefold() != "apple"
        or re.fullmatch(
            r"Apple (?:M|A)[1-9][0-9]*(?: Pro| Max| Ultra)?", adapter["name"]
        ) is None
        or re.fullmatch(r"metal-[1-9][0-9]*", adapter["architecture"]) is None
        or device_match is None
        or int(device_match.group(1), 16) == 0
    ):
        raise JourneyError("Metal adapter evidence has no concrete Apple identity")
    return {key: adapter[key] for key in identity_fields}


def _write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    try:
        with temporary.open("xb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _validate_workspace_inventory(
    workspace: pathlib.Path,
    reference_hashes: dict[str, str],
    seeded_hashes: dict[str, str],
    repaired_hashes: dict[str, str],
) -> None:
    expected_directories = {
        "artifacts",
        "artifacts/reference",
        "artifacts/seeded-failure",
        "artifacts/repaired",
    }
    expected_files = {
        "README.md",
        "gpu-recipe.json",
        "reference-result.json",
        "seeded-failure-result.json",
        "repaired-result.json",
        "clean-agent-journey.json",
    }
    for directory, hashes in (
        ("reference", reference_hashes),
        ("seeded-failure", seeded_hashes),
        ("repaired", repaired_hashes),
    ):
        expected_files.update(f"artifacts/{directory}/{name}" for name in hashes)
    if workspace.is_symlink() or not workspace.is_dir():
        raise JourneyError("evidence workspace must be a real directory")

    actual_directories: set[str] = set()
    actual_files: set[str] = set()
    total_bytes = 0

    def inspect(directory: pathlib.Path) -> None:
        nonlocal total_bytes
        try:
            entries = list(os.scandir(directory))
        except OSError as exc:
            raise JourneyError("evidence workspace cannot be enumerated") from exc
        for entry in entries:
            relative = pathlib.Path(entry.path).relative_to(workspace).as_posix()
            try:
                metadata = entry.stat(follow_symlinks=False)
            except OSError as exc:
                raise JourneyError(f"workspace entry is unavailable: {relative}") from exc
            if stat.S_ISDIR(metadata.st_mode):
                actual_directories.add(relative)
                inspect(pathlib.Path(entry.path))
            elif stat.S_ISREG(metadata.st_mode):
                actual_files.add(relative)
                total_bytes += metadata.st_size
            else:
                raise JourneyError(f"workspace entry is not a regular file: {relative}")

    inspect(workspace)
    if actual_directories != expected_directories or actual_files != expected_files:
        raise JourneyError("evidence workspace contains undeclared or missing entries")
    if total_bytes > MAX_WORKSPACE_BYTES:
        raise JourneyError("evidence workspace exceeds the 64 MiB aggregate bound")


def execute_journey(
    pulp: pathlib.Path,
    symptom: str,
    workspace: pathlib.Path,
    timeout_seconds: float,
) -> dict[str, Any]:
    """Execute and return one complete A5 receipt, or raise JourneyError."""

    _require_absolute_new_workspace(workspace)
    discovery, recipe = _select_recipe(pulp, symptom, timeout_seconds)
    recipe_id = recipe["id"]
    catalog_revision = discovery["catalog_revision"]
    scaffold_proof = _scaffold(
        pulp, recipe, catalog_revision, workspace, timeout_seconds
    )

    reference_dir = workspace / "artifacts" / "reference"
    reference_result = _run(
        _probe_command(pulp, recipe, reference_dir, negative_control=False), timeout_seconds
    )
    if reference_result.returncode == 2:
        raise JourneyUnavailable("reference recipe evidence is unavailable or unverified")
    if reference_result.returncode != 0:
        raise JourneyError(
            f"reference recipe did not pass (exit {reference_result.returncode})"
        )
    reference = _json_stdout(reference_result, "reference probe")
    reference_hashes = _validate_evidence(reference, recipe_id, reference_dir, workspace)
    adapter = _authentic_adapter(reference)
    if reference.get("verdict") != "pass" or reference.get("mutation") is not None:
        raise JourneyError("reference probe is not an unmutated pass")
    if any(item["verdict"] != "pass" for item in _typed_passes(reference)):
        raise JourneyError("reference probe contains a non-passing semantic pass")
    reference_pass_contract = _pass_contract(reference)
    reference_artifact_contract = _artifact_contract(reference)
    reference_execution_contract = _execution_contract(reference)
    reference_passes = _typed_passes(reference)

    negative_dir = workspace / "artifacts" / "seeded-failure"
    negative_result = _run(
        _probe_command(pulp, recipe, negative_dir, negative_control=True), timeout_seconds
    )
    if negative_result.returncode == 2:
        raise JourneyUnavailable("seeded recipe evidence is unavailable or unverified")
    if negative_result.returncode != 1:
        raise JourneyError(
            f"seeded negative control must exit 1, got {negative_result.returncode}"
        )
    negative = _json_stdout(negative_result, "seeded negative control")
    negative_hashes = _validate_evidence(negative, recipe_id, negative_dir, workspace)
    if _authentic_adapter(negative) != adapter:
        raise JourneyError("reference and seeded runs used different adapter identities")
    _require_recipe_contract(negative, reference_pass_contract, reference_artifact_contract)
    _require_execution_contract(negative, reference_execution_contract)
    failing_passes = _seeded_failing_passes(negative)
    if (
        negative.get("verdict") != "fail"
        or not isinstance(negative.get("mutation"), str)
        or not negative["mutation"]
        or len(failing_passes) != 1
        or failing_passes[0].get("work_completed") is not True
        or not isinstance(failing_passes[0].get("code"), str)
    ):
        raise JourneyError("seeded failure is not a single bounded typed diagnosis")
    diagnosis = {
        "mutation": negative["mutation"],
        "pass": failing_passes[0].get("name"),
        "code": failing_passes[0]["code"],
        "expected": failing_passes[0].get("expected"),
        "observed": failing_passes[0].get("observed"),
        "absolute_error": failing_passes[0].get("absolute_error"),
    }

    # This is the only corrective edit documented by the scaffold for its
    # deliberately seeded failure: rerun the catalog command without the seed.
    repaired_dir = workspace / "artifacts" / "repaired"
    repaired_result = _run(
        _probe_command(pulp, recipe, repaired_dir, negative_control=False), timeout_seconds
    )
    if repaired_result.returncode == 2:
        raise JourneyUnavailable("repaired recipe evidence is unavailable or unverified")
    if repaired_result.returncode != 0:
        raise JourneyError(
            f"documented repair did not pass (exit {repaired_result.returncode})"
        )
    repaired = _json_stdout(repaired_result, "repaired probe")
    repaired_hashes = _validate_evidence(repaired, recipe_id, repaired_dir, workspace)
    if _authentic_adapter(repaired) != adapter:
        raise JourneyError("seeded and repaired runs used different adapter identities")
    if repaired.get("verdict") != "pass" or repaired.get("mutation") is not None:
        raise JourneyError("repaired probe did not remove the mutation and pass")
    if any(item["verdict"] != "pass" for item in _typed_passes(repaired)):
        raise JourneyError("repaired probe still contains a failing pass")
    _require_recipe_contract(repaired, reference_pass_contract, reference_artifact_contract)
    _require_execution_contract(repaired, reference_execution_contract)
    if _typed_passes(repaired) != reference_passes:
        raise JourneyError("repaired proof does not reproduce the reference typed passes")
    evidence_ids = {
        reference.get("gpu_evidence_id"),
        negative.get("gpu_evidence_id"),
        repaired.get("gpu_evidence_id"),
    }
    if len(evidence_ids) != 3:
        raise JourneyError("reference, seeded, and repaired runs need distinct evidence ids")
    negative_source = negative.get("source_digest")
    repaired_source = repaired.get("source_digest")
    reference_source = reference.get("source_digest")
    if (
        not isinstance(negative_source, str)
        or not isinstance(repaired_source, str)
        or not isinstance(reference_source, str)
        or negative_source == repaired_source
        or reference_source != repaired_source
    ):
        raise JourneyError("source digests do not isolate only the seeded mutation")
    _require_signature_isolation(reference, negative, repaired)
    if reference_hashes != repaired_hashes:
        raise JourneyError("repaired artifacts do not reproduce the reference proof")

    # Partition every artifact against the unmutated reference. This binds all
    # stable evidence without inferring semantic roles from filenames, while
    # explicitly naming every artifact changed by the seeded mutation.
    stable_names, changed_artifacts = _partition_artifact_changes(
        reference_hashes, negative_hashes
    )

    reference_json_path = workspace / "reference-result.json"
    negative_json_path = workspace / "seeded-failure-result.json"
    repaired_json_path = workspace / "repaired-result.json"
    _write_json(reference_json_path, reference)
    _write_json(negative_json_path, negative)
    _write_json(repaired_json_path, repaired)

    receipt = {
        "schema": JOURNEY_SCHEMA,
        "symptom": symptom,
        "catalog_revision": catalog_revision,
        "selection": {
            "recipe_id": recipe_id,
            "catalog_schema": CATALOG_SCHEMA,
            "callable": True,
        },
        "scaffold": scaffold_proof,
        "adapter": adapter,
        "reference_proof": {
            "exit_code": reference_result.returncode,
            "gpu_evidence_id": reference["gpu_evidence_id"],
            "source_digest": reference_source,
            "signature_digest": reference.get("signature_digest"),
            "verdict": reference["verdict"],
            "pass_contract": reference_pass_contract,
            "artifact_contract": reference_artifact_contract,
            "artifacts_sha256": reference_hashes,
            "result_json_sha256": _sha256(reference_json_path),
        },
        "seeded_failure": {
            "exit_code": negative_result.returncode,
            "gpu_evidence_id": negative["gpu_evidence_id"],
            "source_digest": negative_source,
            "signature_digest": negative.get("signature_digest"),
            "diagnosis": diagnosis,
            "artifacts_sha256": negative_hashes,
            "result_json_sha256": _sha256(negative_json_path),
        },
        "applied_fix": {
            "kind": "remove-seeded-negative-control-argument",
            "removed_argument": "--negative-control",
            "source": "scaffold/README.md",
        },
        "repaired_proof": {
            "exit_code": repaired_result.returncode,
            "gpu_evidence_id": repaired["gpu_evidence_id"],
            "source_digest": repaired_source,
            "signature_digest": repaired.get("signature_digest"),
            "verdict": repaired["verdict"],
            "artifacts_sha256": repaired_hashes,
            "result_json_sha256": _sha256(repaired_json_path),
        },
        "stable_reference_artifacts": stable_names,
        "changed_output_artifacts": changed_artifacts,
        "pulp_binary_sha256": _sha256(pulp),
    }
    _write_json(workspace / "clean-agent-journey.json", receipt)
    _validate_workspace_inventory(
        workspace, reference_hashes, negative_hashes, repaired_hashes
    )
    return receipt


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pulp", required=True, type=pathlib.Path)
    parser.add_argument("--symptom", required=True)
    parser.add_argument("--workspace", required=True, type=pathlib.Path)
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        pulp = args.pulp.resolve(strict=True)
        if not pulp.is_file() or not os.access(pulp, os.X_OK):
            raise JourneyError("--pulp must name an executable file")
        if not args.symptom or args.symptom.strip() != args.symptom:
            raise JourneyError("--symptom must be a non-empty exact token")
        if args.timeout_seconds <= 0:
            raise JourneyError("--timeout-seconds must be positive")
        receipt = execute_journey(
            pulp, args.symptom, args.workspace, args.timeout_seconds
        )
    except JourneyUnavailable as exc:
        print(f"gpu-clean-agent-journey: UNAVAILABLE: {exc}", file=sys.stderr)
        return 2
    except (JourneyError, OSError) as exc:
        print(f"gpu-clean-agent-journey: FAIL: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(receipt, sort_keys=True))
    else:
        print(
            "gpu-clean-agent-journey: PASS: "
            f"{receipt['symptom']} -> {receipt['selection']['recipe_id']} -> "
            f"{receipt['seeded_failure']['diagnosis']['code']} -> repaired"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
