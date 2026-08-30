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
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Callable, Iterable
import uuid


CATALOGUE_SCHEMA = "forge.modular.shipped_scene_catalog.v1"
IDENTITY_SCHEMA = "forge.modular.qualification_identity.v1"
INVENTORY_SCHEMA = "forge.modular.inventory_receipt.v1"
CAMPAIGN_SCHEMA = "forge.modular.qualification_campaign.v1"
RESULT_SCHEMA = "forge.modular.scene_qualification_result.v1"
SCENE_IDS = tuple(f"P05-S{index:02d}" for index in range(1, 13))


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
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
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


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
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
