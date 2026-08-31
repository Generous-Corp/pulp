#!/usr/bin/env python3
"""Run the shipped Forge Modular twelve-scene qualification exactly once.

The shipped scene catalogue belongs to Forge.  This driver deliberately does
not copy its prompts: ``--catalogue`` consumes the versioned export produced by
the candidate Forge build.  A run claims each scene on disk before starting the
provider-backed patch command.  A claimed scene is never submitted again,
including after failure or an interrupted process; reconciliation uses the raw
response and run receipts already owned by that scene.
"""

from __future__ import annotations

import argparse
import ctypes
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
from typing import Callable, Iterable
import uuid

import file_lock


CATALOGUE_SCHEMA = "forge.modular.shipped_scene_catalog.v1"
IDENTITY_SCHEMA = "forge.modular.qualification_identity.v1"
INVENTORY_SCHEMA = "forge.modular.inventory_receipt.v1"
CAMPAIGN_SCHEMA = "forge.modular.qualification_campaign.v1"
RESULT_SCHEMA = "forge.modular.scene_qualification_result.v1"
SCENE_IDS = tuple(f"P05-S{index:02d}" for index in range(1, 13))
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")


class QualificationError(RuntimeError):
    """A fail-closed campaign precondition or receipt error."""


def _read_object(path: Path, label: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise QualificationError(f"cannot read {label} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise QualificationError(f"{label} must contain one JSON object: {path}")
    return value


def _canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _digest(value: object) -> str:
    return hashlib.sha256(_canonical(value)).hexdigest()


def _file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _binary_identity(_resources: Path, binary: Path) -> str:
    """Hash executable content without executing candidate-owned code.

    A Developer ID signature is replaceable publication metadata. Zero its
    load-command fields and exclude its blob, matching Forge's release
    identity contract while keeping this verifier entirely reviewer-owned.
    """
    try:
        data = bytearray(binary.read_bytes())
        if len(data) < 28:
            return hashlib.sha256(data).hexdigest()
        magic = bytes(data[:4])
        if magic == b"\xcf\xfa\xed\xfe":
            endian, header_size = "<", 32
        elif magic == b"\xfe\xed\xfa\xcf":
            endian, header_size = ">", 32
        elif magic == b"\xce\xfa\xed\xfe":
            endian, header_size = "<", 28
        elif magic == b"\xfe\xed\xfa\xce":
            endian, header_size = ">", 28
        else:
            return hashlib.sha256(data).hexdigest()

        command_count = struct.unpack_from(endian + "I", data, 16)[0]
        offset = header_size
        signature_range = None
        signature_command_range = None
        non_linkedit_end = 0
        linkedit_ranges = []
        linkedit_size_fields = []
        for _ in range(command_count):
            if offset + 8 > len(data):
                raise ValueError("truncated Mach-O load commands")
            command, size = struct.unpack_from(endian + "II", data, offset)
            if size < 8 or offset + size > len(data):
                raise ValueError("invalid Mach-O load command")
            if command == 0x1D:  # LC_CODE_SIGNATURE
                if size < 16:
                    raise ValueError("truncated LC_CODE_SIGNATURE")
                if signature_range is not None:
                    raise ValueError("multiple LC_CODE_SIGNATURE commands")
                signature_range = struct.unpack_from(
                    endian + "II", data, offset + 8)
                signature_command_range = (offset + 8, offset + 16)
            elif command in (0x1, 0x19):
                if size < 40:
                    raise ValueError("truncated Mach-O segment command")
                segment_name = bytes(
                    data[offset + 8:offset + 24]).rstrip(b"\0")
                if command == 0x19:  # LC_SEGMENT_64
                    if size < 56:
                        raise ValueError("truncated LC_SEGMENT_64")
                    file_offset, file_size = struct.unpack_from(
                        endian + "QQ", data, offset + 40)
                    size_fields = ((offset + 32, offset + 40),
                                   (offset + 48, offset + 56))
                else:  # LC_SEGMENT
                    file_offset, file_size = struct.unpack_from(
                        endian + "II", data, offset + 32)
                    size_fields = ((offset + 28, offset + 32),
                                   (offset + 36, offset + 40))
                file_end = file_offset + file_size
                if file_end > len(data):
                    raise ValueError(
                        "Mach-O segment extends beyond end of file")
                if segment_name == b"__LINKEDIT":
                    linkedit_ranges.append((file_offset, file_end))
                    linkedit_size_fields.extend(size_fields)
                else:
                    non_linkedit_end = max(non_linkedit_end, file_end)
            offset += size
        if signature_range is not None:
            signature_offset, signature_size = signature_range
            signature_end = signature_offset + signature_size
            if not signature_size or signature_offset < offset:
                raise ValueError(
                    "code signature overlaps Mach-O load commands")
            if not non_linkedit_end:
                raise ValueError(
                    "signed Mach-O has no file-backed executable segment")
            if signature_end != len(data):
                raise ValueError(
                    "code signature is not the terminal Mach-O blob")
            if signature_offset < non_linkedit_end:
                raise ValueError(
                    "code signature overlaps file-backed executable data")
            if linkedit_ranges and not any(
                    start <= signature_offset and signature_end <= end
                    for start, end in linkedit_ranges):
                raise ValueError("code signature is outside __LINKEDIT")
            assert signature_command_range is not None
            data[slice(*signature_command_range)] = b"\0" * 8
            for start, end in linkedit_size_fields:
                data[start:end] = b"\0" * (end - start)
            data = data[:signature_offset]
        identity = hashlib.sha256(data).hexdigest()
    except (OSError, ValueError, struct.error) as exc:
        raise QualificationError(
            f"cannot identify Forge binary {binary}: {exc}") from exc
    if not HEX64.fullmatch(identity):
        raise QualificationError(
            f"Forge binary identity is not one exact SHA-256: {binary}")
    return identity


def _is_packaged_resources_layout(build_info: Path, toolchain: Path) -> bool:
    """Recognize only the signed app bundle's non-duplicated helper layout."""
    resources = build_info.parent
    contents = resources.parent
    app_bundle = contents.parent
    return (
        build_info.name == "FORGE_BUILD_INFO" and
        resources.name == "Resources" and
        contents.name == "Contents" and
        app_bundle.suffix == ".app" and
        app_bundle.is_dir() and
        toolchain == (resources / "tools" / "rack").resolve()
    )


def _completed_response(path: Path) -> bool:
    """A reservation alone is not evidence that the provider returned."""
    try:
        return path.is_file() and path.stat().st_size > 0
    except OSError:
        return False


def _json_object_receipt(path: Path) -> bool:
    if not _completed_response(path):
        return False
    try:
        return isinstance(json.loads(path.read_text(encoding="utf-8")), dict)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return False


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _mkdir_durable(path: Path) -> None:
    if path.is_dir():
        return
    path.mkdir(parents=True)
    _fsync_directory(path.parent)


def _write_bytes_exclusive(path: Path, payload: bytes) -> None:
    _mkdir_durable(path.parent)
    temporary = path.with_name(
        f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    descriptor = os.open(
        temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.link(temporary, path)
        _fsync_directory(path.parent)
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        raise
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _write_exclusive(path: Path, value: object) -> None:
    _write_bytes_exclusive(path, _canonical(value))


def _write_or_match(path: Path, value: object, label: str) -> None:
    try:
        _write_exclusive(path, value)
    except FileExistsError:
        existing = _read_object(path, label)
        if existing != value:
            raise QualificationError(
                f"{label} is owned by different inputs; refusing to reuse {path}")


def _freeze_file(source: Path, destination: Path, label: str) -> Path:
    """Create one campaign-owned byte snapshot, or reuse its exact bytes."""
    if destination.is_file():
        if source.is_file() and source.read_bytes() != destination.read_bytes():
            raise QualificationError(
                f"{label} is owned by different input bytes; refusing to reuse "
                f"{destination}")
        return destination
    try:
        payload = source.read_bytes()
    except OSError as exc:
        raise QualificationError(f"cannot read {label} {source}: {exc}") from exc
    _write_bytes_exclusive(destination, payload)
    return destination


def _acquire_scene_lock(scene_dir: Path) -> int:
    """Hold exclusive ownership across claim, provider call, and receipt."""
    _mkdir_durable(scene_dir)
    descriptor = os.open(scene_dir / ".qualification.lock", os.O_RDWR | os.O_CREAT,
                         0o600)
    try:
        file_lock.exclusive_nonblocking(descriptor)
    except BlockingIOError as exc:
        os.close(descriptor)
        raise QualificationError(
            f"{scene_dir.name} has an active qualification owner") from exc
    return descriptor


def load_catalogue(path: Path) -> list[dict]:
    document = _read_object(path, "shipped scene catalogue")
    if document.get("schema") != CATALOGUE_SCHEMA:
        raise QualificationError(
            f"shipped scene catalogue schema must be {CATALOGUE_SCHEMA}")
    scenes = document.get("scenes")
    if not isinstance(scenes, list) or len(scenes) != len(SCENE_IDS):
        raise QualificationError("shipped scene catalogue must contain exactly 12 scenes")
    got = []
    for index, scene in enumerate(scenes):
        if not isinstance(scene, dict):
            raise QualificationError(f"scene {index + 1} is not an object")
        expected = SCENE_IDS[index]
        if scene.get("id") != expected:
            raise QualificationError(
                f"scene {index + 1} must be {expected}, got {scene.get('id')!r}")
        prompt = scene.get("prompt")
        idiom = scene.get("idiom")
        if not isinstance(prompt, str) or not prompt.strip():
            raise QualificationError(f"{expected} has no canonical prompt")
        if not isinstance(idiom, str) or not idiom.strip():
            raise QualificationError(f"{expected} has no idiom identity")
        got.append({"id": expected, "prompt": prompt, "idiom": idiom})
    return got


def load_receipt(path: Path, schema: str, label: str, payload: str) -> dict:
    document = _read_object(path, label)
    if document.get("schema") != schema:
        raise QualificationError(f"{label} schema must be {schema}")
    if not isinstance(document.get(payload), dict) or not document[payload]:
        raise QualificationError(f"{label} has no non-empty {payload} object")
    return document


def _toolchain_identity(root: Path) -> dict:
    root = root.resolve()
    required = ("patch.py", "with_app_model_selection.py", "idiom_check.py",
                "fidelity.py", "rack_open.py", "FORGE_TOOLCHAIN_STAMP")
    files = {}
    for name in required:
        path = root / name
        if not path.is_file():
            raise QualificationError(f"toolchain identity is incomplete: missing {path}")
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if "__pycache__" in relative.parts or path.suffix in {".pyc", ".pyo"}:
            continue
        if path.is_symlink():
            raise QualificationError(
                f"toolchain identity contains a symbolic link: {path}")
        if path.is_file():
            files[str(relative)] = _file_digest(path)
    stamp = (root / "FORGE_TOOLCHAIN_STAMP").read_text(
        encoding="utf-8").strip()
    if not stamp:
        raise QualificationError("FORGE_TOOLCHAIN_STAMP is empty")
    return {"root": str(root), "stamp": stamp, "files": files}


def load_model_selection(toolchain: Path, settings: Path) -> dict:
    """Use the executing wrapper's parser to snapshot the saved UI route."""
    wrapper = toolchain / "with_app_model_selection.py"
    spec = importlib.util.spec_from_file_location(
        "forge_qualification_model_selection", wrapper)
    if spec is None or spec.loader is None:
        raise QualificationError(f"cannot load model-selection parser: {wrapper}")
    module = importlib.util.module_from_spec(spec)
    try:
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        selected = module.load_selection(settings, "ui")
    except Exception as exc:
        raise QualificationError(f"saved Forge UI model selection is invalid: {exc}") from exc
    finally:
        sys.modules.pop(spec.name, None)
    return {
        "provider": selected.provider,
        "model": selected.model,
        "reasoning_effort": selected.reasoning_effort,
        "codex_executable": selected.codex_executable,
        "codex_home": selected.codex_home,
    }


def snapshot_inventory(toolchain: Path) -> dict:
    """Read the exact local inventory through the executing patch.py."""
    completed = subprocess.run(
        [sys.executable, "-c",
         "import json,patch; print(json.dumps(patch.inventory(),sort_keys=True))"],
        cwd=str(toolchain), capture_output=True, text=True, timeout=120,
        check=False)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise QualificationError(
            "cannot snapshot the executing Rack inventory: " +
            (detail or f"exit {completed.returncode}"))
    try:
        inventory = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise QualificationError(
            f"executing Rack inventory is not JSON: {exc}") from exc
    if not isinstance(inventory, dict) or not inventory:
        raise QualificationError("executing Rack inventory is empty")
    return inventory


def _load_build_info(path: Path) -> dict[str, str]:
    """Read the candidate bundle's exact, duplicate-free build identity."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise QualificationError(
            f"cannot read Forge build identity {path}: {exc}") from exc
    fields: dict[str, str] = {}
    for number, line in enumerate(lines, 1):
        if not line or "=" not in line:
            raise QualificationError(
                f"Forge build identity line {number} is not key=value")
        key, value = line.split("=", 1)
        if not key or not value:
            raise QualificationError(
                f"Forge build identity line {number} has an empty key or value")
        if key in fields:
            raise QualificationError(
                f"Forge build identity repeats field {key!r}")
        fields[key] = value
    required = {
        "schema": "1",
        "product": "Forge Modular",
        "product_id": "com.generous.forge.modular",
        "role": "Rack module and patch generator",
        "format": "Standalone application",
        "source_git_dirty": "false",
    }
    for key, expected in required.items():
        if fields.get(key) != expected:
            raise QualificationError(
                f"Forge build identity {key} must be {expected!r}")
    if fields.get("build") not in {
            "Release · darwin-arm64", "Release · darwin-x64"}:
        raise QualificationError(
            "Forge build identity must name a supported Release build")
    if not fields.get("version"):
        raise QualificationError("Forge build identity has no version")
    if not HEX40.fullmatch(fields.get("source_git_head", "")):
        raise QualificationError(
            "Forge build identity must name one exact source_git_head")
    if not HEX64.fullmatch(fields.get("source_snapshot_sha256", "")):
        raise QualificationError(
            "Forge build identity must name one exact source_snapshot_sha256")
    pulp_parts = fields.get("pulp_sdk", "").rsplit(" · ", 1)
    if (len(pulp_parts) != 2 or not pulp_parts[0] or
            not HEX40.fullmatch(pulp_parts[1])):
        raise QualificationError(
            "Forge build identity pulp_sdk must name a version and exact revision")
    if ("expected_pulp_sdk_ref" in fields and
            fields["expected_pulp_sdk_ref"] != pulp_parts[1]):
        raise QualificationError(
            "Forge build identity expected_pulp_sdk_ref does not match pulp_sdk")
    return fields


def _stamp_identity(stamp: str) -> tuple[str, str, str | None]:
    lines = stamp.splitlines()
    if (len(lines) not in {3, 4} or not lines[0] or not lines[1] or
            not lines[2].startswith("pulp ")):
        raise QualificationError(
            "FORGE_TOOLCHAIN_STAMP must contain version, date, Pulp revision, "
            "and at most one decoder identity")
    revision = lines[2].removeprefix("pulp ")
    if not HEX40.fullmatch(revision):
        raise QualificationError(
            "FORGE_TOOLCHAIN_STAMP must name one exact Pulp revision")
    decoder_identity = None
    if len(lines) == 4:
        decoder = lines[3].split()
        if (len(decoder) != 2 or decoder[0] != "rack_patch_decode" or
                not HEX64.fullmatch(decoder[1])):
            raise QualificationError(
                "FORGE_TOOLCHAIN_STAMP must name one exact decoder identity")
        decoder_identity = decoder[1]
    return lines[0], revision, decoder_identity


def _rename_directory_exclusive(source: Path, destination: Path) -> None:
    """Atomically publish one directory without replacing an existing name."""
    if sys.platform == "darwin":
        function = ctypes.CDLL(None, use_errno=True).renameatx_np
        function.argtypes = (
            ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
            ctypes.c_uint)
        function.restype = ctypes.c_int
        result = function(-2, os.fsencode(source), -2, os.fsencode(destination), 4)
    elif sys.platform.startswith("linux"):
        library = ctypes.CDLL(None, use_errno=True)
        function = getattr(library, "renameat2", None)
        if function is None:
            raise QualificationError(
                "this platform cannot publish qualification inputs without replace")
        function.argtypes = (
            ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p,
            ctypes.c_uint)
        function.restype = ctypes.c_int
        result = function(
            -100, os.fsencode(source), -100, os.fsencode(destination), 1)
    elif os.name == "nt":
        # Windows rename fails when the destination already exists.
        os.rename(source, destination)
        return
    else:
        raise QualificationError(
            "this platform cannot publish qualification inputs without replace")
    if result != 0:
        error = ctypes.get_errno()
        if error == errno.EEXIST:
            raise FileExistsError(error, os.strerror(error), destination)
        raise OSError(error, os.strerror(error), destination)


def prepare_inputs(*, toolchain: Path, settings: Path, build_info: Path,
                   expected_pulp_ref: str, output_dir: Path,
                   inventory_loader: Callable[[Path], dict] = snapshot_inventory,
                   selection_loader: Callable[[Path, Path], dict] = load_model_selection,
                   ) -> tuple[Path, Path]:
    """Atomically author the two provider-free campaign input receipts."""
    toolchain = toolchain.resolve()
    settings = settings.resolve()
    build_info = build_info.resolve()
    output_dir = output_dir.expanduser().absolute()
    if not HEX40.fullmatch(expected_pulp_ref):
        raise QualificationError(
            "expected Pulp SDK authority must be one exact revision")
    fields = _load_build_info(build_info)
    toolchain_snapshot = _toolchain_identity(toolchain)
    pulp_revision = fields["pulp_sdk"].rsplit(" · ", 1)[1]
    if pulp_revision != expected_pulp_ref:
        raise QualificationError(
            "Forge build identity does not match the expected Pulp SDK authority")
    stamp_version, stamp_pulp_revision, stamped_decoder = _stamp_identity(
        toolchain_snapshot["stamp"])
    if stamp_version != fields["version"]:
        raise QualificationError(
            "Forge build identity and FORGE_TOOLCHAIN_STAMP name different "
            "versions")
    if stamp_pulp_revision != pulp_revision:
        raise QualificationError(
            "Forge build identity and FORGE_TOOLCHAIN_STAMP name different "
            "Pulp revisions")
    decoder = build_info.parent / "build" / "rack_patch_decode"
    if not decoder.is_file() or not os.access(decoder, os.X_OK):
        raise QualificationError(
            f"Forge Modular Rack saved-patch decoder is not executable: {decoder}")
    decoder_identity = _binary_identity(build_info.parent, decoder)
    executing_decoder = toolchain / "rack_patch_decode"
    packaged_resources = _is_packaged_resources_layout(build_info, toolchain)
    if not packaged_resources or executing_decoder.exists():
        if (not executing_decoder.is_file() or
                not os.access(executing_decoder, os.X_OK)):
            raise QualificationError(
                "executing Rack saved-patch decoder is missing or not executable: "
                f"{executing_decoder}")
        if _binary_identity(build_info.parent, executing_decoder) != decoder_identity:
            raise QualificationError(
                "executing Rack saved-patch decoder does not match the candidate "
                "executable")
    if stamped_decoder is not None and stamped_decoder != decoder_identity:
        raise QualificationError(
            "FORGE_TOOLCHAIN_STAMP decoder identity does not match the "
            "candidate executable")
    selected_model = selection_loader(toolchain, settings)
    inventory = inventory_loader(toolchain)
    if not isinstance(inventory, dict) or not inventory:
        raise QualificationError("executing Rack inventory is empty")
    identity_document = {
        "schema": IDENTITY_SCHEMA,
        "identity": {
            "forge_revision": fields["source_git_head"],
            "pulp_revision": pulp_revision,
            "source_snapshot_sha256": fields["source_snapshot_sha256"],
            "rack_patch_decode_sha256": decoder_identity,
            **selected_model,
            "toolchain": {
                "stamp": toolchain_snapshot["stamp"],
                "files": toolchain_snapshot["files"],
            },
        },
    }
    inventory_document = {"schema": INVENTORY_SCHEMA, "inventory": inventory}
    expected = {
        "identity.json": identity_document,
        "inventory.json": inventory_document,
    }
    def existing_pair() -> tuple[Path, Path]:
        if not output_dir.is_dir() or output_dir.is_symlink():
            raise QualificationError(
                f"qualification input destination is not a directory: {output_dir}")
        for name, document in expected.items():
            path = output_dir / name
            if not path.is_file() or _read_object(path, name) != document:
                raise QualificationError(
                    f"qualification inputs are incomplete or owned by different "
                    f"inputs: {output_dir}")
        return output_dir / "identity.json", output_dir / "inventory.json"

    if output_dir.exists() or output_dir.is_symlink():
        return existing_pair()
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_dir.with_name(
        f".{output_dir.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    temporary.mkdir(mode=0o700)
    try:
        for name, document in expected.items():
            _write_exclusive(temporary / name, document)
        try:
            _rename_directory_exclusive(temporary, output_dir)
        except FileExistsError:
            shutil.rmtree(temporary)
            return existing_pair()
        _fsync_directory(output_dir.parent)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return output_dir / "identity.json", output_dir / "inventory.json"


def _selected(scenes: list[dict], requested: Iterable[str]) -> list[dict]:
    wanted = list(requested)
    if not wanted:
        return scenes
    unknown = sorted(set(wanted) - set(SCENE_IDS))
    if unknown:
        raise QualificationError("unknown scene id(s): " + ", ".join(unknown))
    wanted_set = set(wanted)
    return [scene for scene in scenes if scene["id"] in wanted_set]


def _existing_result(path: Path, scene_dir: Path, scene_id: str) -> dict:
    """Validate terminal ownership and rehash retained evidence on resume."""
    result = _read_object(path, "scene result")
    if result.get("schema") != RESULT_SCHEMA or result.get("scene_id") != scene_id:
        raise QualificationError(
            f"{scene_id} terminal receipt has the wrong schema or scene identity")
    if result.get("status") not in {"generated", "failed"}:
        raise QualificationError(f"{scene_id} terminal receipt has no valid status")
    if result.get("retries") != 0:
        raise QualificationError(f"{scene_id} terminal receipt does not record zero retries")
    if result.get("submission_processes") not in {0, 1}:
        raise QualificationError(
            f"{scene_id} terminal receipt has an invalid process count")
    if result.get("provider_submissions") not in {0, 1}:
        raise QualificationError(
            f"{scene_id} terminal receipt has an invalid provider submission count")
    receipts = result.get("receipts")
    if result["status"] == "generated" and not isinstance(receipts, dict):
        raise QualificationError(f"{scene_id} generated without artifact receipts")
    receipts = receipts if isinstance(receipts, dict) else {}
    required = {
        "attempts/attempt01-model-response.txt", "model-prompt.txt",
        "artifact.vcv", "artifact.why.json", "run.log",
    }
    if result["status"] == "generated" and set(receipts) != required:
        raise QualificationError(
            f"{scene_id} generated without the exact required receipt set")
    if result["status"] == "generated" and result.get("provider_submissions") != 1:
        raise QualificationError(f"{scene_id} generated without one provider submission")
    if result["status"] == "generated" and result.get("submission_processes") != 1:
        raise QualificationError(f"{scene_id} generated without one process")
    response = scene_dir / "attempts/attempt01-model-response.txt"
    if result.get("provider_submissions") == 1 and not _completed_response(response):
        raise QualificationError(
            f"{scene_id} records a provider submission without a completed response")
    if result["status"] == "failed":
        if (not result.get("reconciled_interruption") and
                (result.get("submission_processes") != 1 or
                 "run.log" not in receipts)):
            raise QualificationError(
                f"{scene_id} failed without its submission/run-log receipt")
        if (result.get("provider_submissions") == 1 and
                "attempts/attempt01-model-response.txt" not in receipts):
            raise QualificationError(
                f"{scene_id} provider failure lost its raw response")
    root = scene_dir.resolve()
    for relative, expected in receipts.items():
        if not isinstance(relative, str) or not isinstance(expected, str):
            raise QualificationError(f"{scene_id} has a malformed artifact receipt")
        candidate = (scene_dir / relative).resolve()
        try:
            candidate.relative_to(root)
        except ValueError as exc:
            raise QualificationError(
                f"{scene_id} artifact receipt escapes its scene directory") from exc
        if not candidate.is_file() or _file_digest(candidate) != expected:
            raise QualificationError(
                f"{scene_id} retained artifact is missing or changed: {relative}")
    return result


def _follow_on(toolchain: Path, artifact: Path, idiom: str,
               scene_dir: Path) -> dict:
    python = sys.executable
    return {
        "rack_load": {"status": "pending", "argv": [
            python, str(toolchain / "patch.py"), "verify", str(artifact)]},
        "contract": {"status": "pending", "argv": [
            python, str(toolchain / "idiom_check.py"), str(artifact), idiom]},
        "causal": {
            "status": "pending",
            "inputs": [str(artifact), str(scene_dir / "artifact.why.json"),
                       str(scene_dir / "run.log")],
        },
        "dsp_and_audio": {"status": "pending", "argv": [
            python, str(toolchain / "fidelity.py"), str(artifact), "--wav",
            str(scene_dir / "audio.wav")]},
        "open_in_rack": {"status": "pending", "argv": [
            python, str(toolchain / "rack_open.py"), str(artifact)]},
    }


def _retained_receipts(scene_dir: Path) -> dict[str, str]:
    receipts = {}
    for relative in (
            "attempts/attempt01-model-response.txt", "model-prompt.txt",
            "artifact.vcv", "artifact.why.json", "run.log"):
        path = scene_dir / relative
        if path.is_file():
            receipts[relative] = _file_digest(path)
    return receipts


def _reconcile_interrupted_claim(scene: dict, scene_dir: Path,
                                 toolchain: Path) -> dict:
    """Terminalize retained evidence without risking another provider call."""
    response = scene_dir / "attempts/attempt01-model-response.txt"
    provider_submissions = 1 if _completed_response(response) else 0
    result = {
        "schema": RESULT_SCHEMA,
        "scene_id": scene["id"],
        "status": "failed",
        "exit_code": None,
        "error": (
            "interrupted after a completed provider response before the terminal receipt"
            if provider_submissions else
            "interrupted after claim; provider submission boundary is indeterminate"),
        "reconciled_interruption": True,
        "submission_processes": 1 if (scene_dir / "run.log").is_file() else 0,
        "provider_submissions": provider_submissions,
        "maximum_provider_submissions": 1,
        "retries": 0,
        "receipts": _retained_receipts(scene_dir),
        "generation": {
            "status": "failed", "artifact": str(scene_dir / "artifact.vcv"),
            "idiom": scene["idiom"],
        },
        "follow_on": _follow_on(
            toolchain, scene_dir / "artifact.vcv", scene["idiom"], scene_dir),
    }
    _write_exclusive(scene_dir / "result.json", result)
    return result


def run_campaign(*, catalogue: Path, identity: Path, inventory: Path,
                 toolchain: Path, settings: Path, run_root: Path,
                 requested: Iterable[str] = (),
                 runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
                 inventory_loader: Callable[[Path], dict] = snapshot_inventory,
                 selection_loader: Callable[[Path, Path], dict] = load_model_selection,
                 ) -> int:
    catalogue = catalogue.resolve()
    identity = identity.resolve()
    inventory = inventory.resolve()
    toolchain = toolchain.resolve()
    settings = settings.resolve()
    run_root = run_root.resolve()
    scenes = load_catalogue(catalogue)
    identity_receipt = load_receipt(
        identity, IDENTITY_SCHEMA, "qualification identity receipt", "identity")
    inventory_receipt = load_receipt(
        inventory, INVENTORY_SCHEMA, "inventory receipt", "inventory")
    toolchain_snapshot = _toolchain_identity(toolchain)
    recorded_toolchain = identity_receipt["identity"].get("toolchain")
    expected_toolchain = {
        "stamp": toolchain_snapshot["stamp"],
        "files": toolchain_snapshot["files"],
    }
    if recorded_toolchain != expected_toolchain:
        raise QualificationError(
            "qualification identity receipt does not match the executing "
            "toolchain stamp and file hashes")
    if not settings.is_file() and not (run_root / "settings.input.json").is_file():
        raise QualificationError(f"saved Forge settings are missing: {settings}")
    _mkdir_durable(run_root)
    frozen_settings = _freeze_file(
        settings, run_root / "settings.input.json", "saved Forge settings")
    selected_model = selection_loader(toolchain, frozen_settings)
    recorded_model = {
        key: identity_receipt["identity"].get(key) for key in (
            "provider", "model", "reasoning_effort", "codex_executable",
            "codex_home")
    }
    if recorded_model != selected_model:
        raise QualificationError(
            "qualification identity receipt does not match the saved Forge "
            "UI model selection")
    if inventory_loader(toolchain) != inventory_receipt["inventory"]:
        raise QualificationError(
            "inventory receipt does not match the executing Rack inventory")

    campaign = {
        "schema": CAMPAIGN_SCHEMA,
        "catalogue_sha256": _digest({"schema": CATALOGUE_SCHEMA, "scenes": scenes}),
        "identity_sha256": _digest(identity_receipt),
        "inventory_sha256": _digest(inventory_receipt),
        "toolchain": toolchain_snapshot,
        "settings_sha256": _file_digest(frozen_settings),
        "model_selection": selected_model,
        "scene_ids": list(SCENE_IDS),
    }
    _write_or_match(run_root / "campaign.json", campaign, "campaign receipt")
    _write_or_match(run_root / "identity.json", identity_receipt, "identity receipt")
    _write_or_match(run_root / "inventory.json", inventory_receipt, "inventory receipt")
    fingerprint = _digest(campaign)
    any_failed = False

    for scene in _selected(scenes, requested):
        if _toolchain_identity(toolchain) != toolchain_snapshot:
            raise QualificationError(
                "executing toolchain changed before the next scene submission")
        scene_dir = run_root / scene["id"]
        lock_fd = _acquire_scene_lock(scene_dir)
        artifact = scene_dir / "artifact.vcv"
        claim = {
            "schema": "forge.modular.scene_submission_claim.v1",
            "campaign_sha256": fingerprint,
            "scene": scene,
            "maximum_provider_submissions": 1,
            "retries": 0,
        }
        claim_path = scene_dir / "submission.json"
        try:
            _write_exclusive(claim_path, claim)
        except FileExistsError:
            try:
                existing = _read_object(claim_path, "scene submission claim")
                if existing != claim:
                    raise QualificationError(
                        f"{scene['id']} is claimed by different inputs")
                result_path = scene_dir / "result.json"
                if not result_path.is_file():
                    result = _reconcile_interrupted_claim(
                        scene, scene_dir, toolchain)
                else:
                    result = _existing_result(result_path, scene_dir, scene["id"])
                if (result.get("post_submission_validation_error") or
                        result.get("inventory_matches_receipt") is False or
                        result.get("toolchain_matches_identity") is False):
                    os.close(lock_fd)
                    return 1
                if result.get("status") != "generated":
                    any_failed = True
                os.close(lock_fd)
                continue
            except BaseException:
                os.close(lock_fd)
                raise
        except BaseException:
            os.close(lock_fd)
            raise

        try:
            attempts = scene_dir / "attempts"
            attempts.mkdir(mode=0o700)
            log_path = scene_dir / "run.log"
            log_fd = os.open(
                log_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except BaseException:
            os.close(lock_fd)
            raise
        command = [
            sys.executable, str(toolchain / "with_app_model_selection.py"),
            "--settings", str(frozen_settings), "--role", "ui", "--",
            sys.executable, str(toolchain / "patch.py"), "build", scene["prompt"],
            "--retries", "0", "--out", str(artifact),
        ]
        environment = dict(os.environ)
        environment["FORGE_ATTEMPT_DIR"] = str(attempts)
        environment["FORGE_MODEL_PROMPT_RECORD"] = str(scene_dir / "model-prompt.txt")
        try:
            with os.fdopen(log_fd, "w", encoding="utf-8") as log:
                completed = runner(
                    command, cwd=str(toolchain), env=environment,
                    stdout=log, stderr=subprocess.STDOUT, check=False)
                log.flush()
                os.fsync(log.fileno())
        except Exception as exc:
            raw_response = attempts / "attempt01-model-response.txt"
            receipts = _retained_receipts(scene_dir)
            result = {
                "schema": RESULT_SCHEMA, "scene_id": scene["id"],
                "status": "failed", "exit_code": None,
                "error": f"submission launcher failed: {exc}",
                "submission_processes": 1,
                "provider_submissions": 1 if _completed_response(raw_response) else 0,
                "maximum_provider_submissions": 1, "retries": 0,
                "run_log": str(log_path), "receipts": receipts,
            }
            try:
                _write_exclusive(scene_dir / "result.json", result)
            finally:
                os.close(lock_fd)
            any_failed = True
            continue
        except BaseException:
            os.close(lock_fd)
            raise

        raw_response = attempts / "attempt01-model-response.txt"
        prompt_record = scene_dir / "model-prompt.txt"
        why = scene_dir / "artifact.why.json"
        required = (raw_response, prompt_record, artifact, why)
        valid = {
            raw_response: _completed_response(raw_response),
            prompt_record: _completed_response(prompt_record),
            artifact: _json_object_receipt(artifact),
            why: _json_object_receipt(why),
        }
        missing = [str(path) for path in required if not valid[path]]
        receipts = {str(path.relative_to(scene_dir)): _file_digest(path)
                    for path in required if path.is_file()}
        receipts[log_path.name] = _file_digest(log_path)
        try:
            inventory_matches = (
                inventory_loader(toolchain) == inventory_receipt["inventory"])
            toolchain_matches = _toolchain_identity(toolchain) == toolchain_snapshot
        except Exception as exc:
            result = {
                "schema": RESULT_SCHEMA,
                "scene_id": scene["id"],
                "status": "failed",
                "exit_code": completed.returncode,
                "submission_processes": 1,
                "provider_submissions": 1 if _completed_response(raw_response) else 0,
                "maximum_provider_submissions": 1,
                "retries": 0,
                "receipts": receipts,
                "missing_receipts": missing,
                "post_submission_validation_error": str(exc),
                "generation": {
                    "status": "failed", "artifact": str(artifact),
                    "idiom": scene["idiom"],
                },
                "follow_on": _follow_on(
                    toolchain, artifact, scene["idiom"], scene_dir),
            }
            try:
                _write_exclusive(scene_dir / "result.json", result)
            finally:
                os.close(lock_fd)
            return 1
        except BaseException:
            os.close(lock_fd)
            raise
        generation_succeeded = (
            completed.returncode == 0 and _completed_response(raw_response) and
            not missing and inventory_matches and toolchain_matches)
        result = {
            "schema": RESULT_SCHEMA,
            "scene_id": scene["id"],
            # "generated" is deliberately narrower than "passed": Rack load,
            # contract, causal, DSP/audio, and human Open-in-Rack proof remain
            # explicit pending follow-ons below.
            "status": "generated" if generation_succeeded else "failed",
            "exit_code": completed.returncode,
            # The immutable raw response is the evidence that the provider
            # boundary was crossed. A local preflight refusal has one launched
            # process but zero provider submissions and is still never retried.
            "submission_processes": 1,
            "provider_submissions": 1 if _completed_response(raw_response) else 0,
            "maximum_provider_submissions": 1,
            "retries": 0,
            "receipts": receipts,
            "missing_receipts": missing,
            "inventory_matches_receipt": inventory_matches,
            "toolchain_matches_identity": toolchain_matches,
            "generation": {
                "status": "generated" if generation_succeeded else "failed",
                "artifact": str(artifact),
                "idiom": scene["idiom"],
            },
            "follow_on": _follow_on(toolchain, artifact, scene["idiom"], scene_dir),
        }
        try:
            _write_exclusive(scene_dir / "result.json", result)
        finally:
            os.close(lock_fd)
        if not inventory_matches or not toolchain_matches:
            # The campaign identity is no longer true. Unlike an ordinary
            # scene failure this invalidates every later provider submission,
            # so stop with the already-retained response and artifact.
            return 1
        if not generation_succeeded:
            any_failed = True
    return 1 if any_failed else 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalogue", type=Path, required=True)
    parser.add_argument("--list", action="store_true",
                        help="print the validated shipped scenes without running them")
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--inventory", type=Path)
    parser.add_argument("--toolchain-root", type=Path)
    parser.add_argument("--settings", type=Path)
    parser.add_argument("--run-root", type=Path)
    parser.add_argument("--scene", action="append", default=[])
    args = parser.parse_args(argv)
    if not args.list:
        missing = [name for name in (
            "identity", "inventory", "toolchain_root", "settings", "run_root")
            if getattr(args, name) is None]
        if missing:
            parser.error("a run requires --" + ", --".join(
                name.replace("_", "-") for name in missing))
    return args


def parse_prepare_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="qualify_scenes.py prepare",
        description="Author provider-free Forge Modular qualification inputs.")
    parser.add_argument("--toolchain-root", type=Path, required=True)
    parser.add_argument("--settings", type=Path, required=True)
    parser.add_argument("--build-info", type=Path, required=True)
    parser.add_argument("--expected-pulp-ref", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    raw = sys.argv[1:] if argv is None else argv
    try:
        if raw and raw[0] == "prepare":
            args = parse_prepare_args(raw[1:])
            identity, inventory = prepare_inputs(
                toolchain=args.toolchain_root, settings=args.settings,
                build_info=args.build_info,
                expected_pulp_ref=args.expected_pulp_ref,
                output_dir=args.output_dir)
            print(identity)
            print(inventory)
            return 0
        args = parse_args(raw)
        if args.list:
            for scene in load_catalogue(args.catalogue):
                print(json.dumps(scene, sort_keys=True))
            return 0
        return run_campaign(
            catalogue=args.catalogue, identity=args.identity,
            inventory=args.inventory, toolchain=args.toolchain_root,
            settings=args.settings, run_root=args.run_root,
            requested=args.scene)
    except QualificationError as exc:
        print(f"qualify-scenes: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
