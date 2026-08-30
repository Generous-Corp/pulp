#!/usr/bin/env python3
"""Adjudicate an immutable twelve-scene Forge Modular qualification campaign.

Generation and adjudication have different evidence boundaries.  The provider
driver owns ``result.json`` permanently; this tool rehashes those receipts and
writes separate ``adjudication.json`` files after the local Rack and idiom
checks finish.  No provider command is reachable from this program.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
from types import ModuleType
from typing import Callable

import qualify_scenes as Q
import file_lock


ADJUDICATION_SCHEMA = "forge.modular.scene_adjudication.v2"
SUMMARY_SCHEMA = "forge.modular.qualification_summary.v1"
CHECK_NAMES = (
    "module_availability", "rack_load", "idiom_contract",
    "causal_witness", "negative_control", "dsp_and_audio",
)
STATUSES = {"PASS", "FAIL", "WITHHOLD"}


class AdjudicationError(RuntimeError):
    """A fail-closed provenance, execution, or receipt error."""


@dataclass(frozen=True)
class CampaignContext:
    catalogue_path: Path
    run_root: Path
    campaign: dict
    identity: dict
    inventory: dict
    scenes: tuple[dict, ...]
    toolchain: Path
    bindings: dict


def _read_object(path: Path, label: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AdjudicationError(f"cannot read {label} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise AdjudicationError(f"{label} must contain one JSON object: {path}")
    return value


def _status(checks: dict) -> str:
    statuses = [checks[name]["status"] for name in CHECK_NAMES]
    if any(status == "WITHHOLD" for status in statuses):
        return "WITHHOLD"
    if any(status == "FAIL" for status in statuses):
        return "FAIL"
    return "PASS"


def _check_evidence_digest(checks: dict) -> str:
    evidence = {
        name: {key: value for key, value in checks[name].items() if key != "status"}
        for name in CHECK_NAMES
    }
    return Q._digest(evidence)


def _command_result(command: list[str], completed: subprocess.CompletedProcess) -> dict:
    stdout = completed.stdout if isinstance(completed.stdout, str) else ""
    stderr = completed.stderr if isinstance(completed.stderr, str) else ""
    return {
        "argv": command,
        "exit_code": completed.returncode,
        "stdout_sha256": hashlib.sha256(stdout.encode()).hexdigest(),
        "stderr_sha256": hashlib.sha256(stderr.encode()).hexdigest(),
        "stdout": stdout,
        "stderr": stderr,
    }


def _run(command: list[str], runner: Callable[..., subprocess.CompletedProcess],
         expected: dict[int, str], env_overrides: dict[str, str] | None = None) -> dict:
    run_options = {
        "capture_output": True, "text": True, "timeout": 180, "check": False,
    }
    if env_overrides:
        environment = dict(os.environ)
        environment.update(env_overrides)
        run_options["env"] = environment
    try:
        completed = runner(command, **run_options)
        result = _command_result(command, completed)
    except Exception as exc:
        result = {
            "status": "WITHHOLD", "argv": command,
            "error": f"checker could not run: {exc}",
        }
        if env_overrides:
            result["environment"] = env_overrides
        return result
    crashed = (completed.returncode < 0 or
               "Traceback (most recent call last)" in result["stderr"])
    result["status"] = (
        "WITHHOLD" if crashed else expected.get(completed.returncode, "WITHHOLD"))
    if crashed:
        result["error"] = "checker crashed before producing a verdict"
    elif completed.returncode not in expected:
        result["error"] = f"checker returned unexpected exit {completed.returncode}"
    if env_overrides:
        result["environment"] = env_overrides
    return result


def _load_idiom_checker(toolchain: Path) -> ModuleType:
    path = toolchain / "idiom_check.py"
    spec = importlib.util.spec_from_file_location(
        f"forge_scene_idiom_check_{os.getpid()}", path)
    if spec is None or spec.loader is None:
        raise AdjudicationError(f"cannot load idiom checker: {path}")
    module = importlib.util.module_from_spec(spec)
    old_path = list(sys.path)
    try:
        sys.path.insert(0, str(toolchain))
        spec.loader.exec_module(module)
    except Exception as exc:
        raise AdjudicationError(f"cannot load idiom checker {path}: {exc}") from exc
    finally:
        sys.path[:] = old_path
    return module


def _tree_identity(path: Path, active: set[Path] | None = None) -> dict | None:
    if not path.exists():
        return None
    resolved = path.resolve()
    active = set() if active is None else active
    if resolved in active:
        return {"root": str(resolved), "cycle": True}
    if resolved.is_file():
        return {"root": str(resolved), "files": {".": Q._file_digest(resolved)}}
    active.add(resolved)
    files: dict[str, str] = {}
    links: dict[str, dict] = {}
    try:
        for root, directories, names in os.walk(resolved, followlinks=False):
            root_path = Path(root)
            for name in sorted([*directories, *names]):
                entry = root_path / name
                relative = str(entry.relative_to(resolved))
                if entry.is_symlink():
                    links[relative] = {
                        "target": os.readlink(entry),
                        "identity": _tree_identity(entry, active),
                    }
                elif entry.is_file():
                    files[relative] = Q._file_digest(entry)
        return {"root": str(resolved), "files": files, "links": links}
    finally:
        active.remove(resolved)


def _rack_user_root() -> Path:
    return Path.home() / "Library/Application Support/Rack2"


def _rack_plugin_roots() -> list[Path]:
    rack_root = _rack_user_root()
    try:
        return sorted(
            (path.resolve() for path in rack_root.iterdir()
             if path.is_dir() and path.name.startswith("plugins-")),
            key=str)
    except OSError:
        return []


def _plugin_identities(artifact: Path) -> dict:
    document = _read_object(artifact, "generated Rack patch")
    modules = document.get("modules")
    if not isinstance(modules, list):
        modules = []
    plugins = sorted({
        module.get("plugin") for module in modules
        if isinstance(module, dict) and isinstance(module.get("plugin"), str)
        and module.get("plugin") != "Core"
    })
    roots = _rack_plugin_roots()
    identities = {}
    for plugin in plugins:
        candidates = []
        for root in roots:
            try:
                entries = sorted(root.iterdir(), key=lambda path: path.name)
            except OSError:
                continue
            for entry in entries:
                if (entry.name == plugin or
                        (entry.name.startswith(plugin + "-") and
                         entry.name.endswith(".vcvplugin"))):
                    candidates.append(_tree_identity(entry))
        identities[plugin] = candidates
    return {
        "architecture": platform.machine().lower(),
        "roots": [str(root) for root in roots],
        "plugins": identities,
    }


def _load_context(catalogue_path: Path, run_root: Path) -> CampaignContext:
    catalogue_path = catalogue_path.resolve()
    run_root = run_root.resolve()
    scenes = tuple(Q.load_catalogue(catalogue_path))
    campaign_path = run_root / "campaign.json"
    identity_path = run_root / "identity.json"
    inventory_path = run_root / "inventory.json"
    settings_path = run_root / "settings.input.json"
    campaign = _read_object(campaign_path, "campaign receipt")
    identity = Q.load_receipt(
        identity_path, Q.IDENTITY_SCHEMA, "qualification identity receipt", "identity")
    inventory = Q.load_receipt(
        inventory_path, Q.INVENTORY_SCHEMA, "inventory receipt", "inventory")
    if campaign.get("schema") != Q.CAMPAIGN_SCHEMA:
        raise AdjudicationError(f"campaign schema must be {Q.CAMPAIGN_SCHEMA}")
    if campaign.get("scene_ids") != list(Q.SCENE_IDS):
        raise AdjudicationError("campaign does not bind the exact twelve scene ids")
    catalogue_document = {"schema": Q.CATALOGUE_SCHEMA, "scenes": list(scenes)}
    if campaign.get("catalogue_sha256") != Q._digest(catalogue_document):
        raise AdjudicationError("catalogue does not match the campaign receipt")
    if campaign.get("identity_sha256") != Q._digest(identity):
        raise AdjudicationError("identity receipt does not match the campaign")
    if campaign.get("inventory_sha256") != Q._digest(inventory):
        raise AdjudicationError("inventory receipt does not match the campaign")
    if not settings_path.is_file() or campaign.get("settings_sha256") != Q._file_digest(settings_path):
        raise AdjudicationError("frozen settings receipt is missing or changed")

    recorded_toolchain = campaign.get("toolchain")
    if not isinstance(recorded_toolchain, dict):
        raise AdjudicationError("campaign has no toolchain identity")
    root = recorded_toolchain.get("root")
    if not isinstance(root, str) or not Path(root).is_absolute():
        raise AdjudicationError("campaign toolchain root is not absolute")
    toolchain = Path(root).resolve()
    current_toolchain = Q._toolchain_identity(toolchain)
    if current_toolchain != recorded_toolchain:
        raise AdjudicationError("executing toolchain changed after generation")
    identity_toolchain = identity["identity"].get("toolchain")
    if identity_toolchain != {
            "stamp": current_toolchain["stamp"],
            "files": current_toolchain["files"]}:
        raise AdjudicationError("toolchain does not match the qualification identity")

    bindings = {
        "campaign_sha256": Q._file_digest(campaign_path),
        "campaign_document_sha256": Q._digest(campaign),
        "catalogue_sha256": Q._file_digest(catalogue_path),
        "catalogue_document_sha256": Q._digest(catalogue_document),
        "identity_sha256": Q._file_digest(identity_path),
        "identity_document_sha256": Q._digest(identity),
        "inventory_sha256": Q._file_digest(inventory_path),
        "inventory_document_sha256": Q._digest(inventory),
        "settings_sha256": Q._file_digest(settings_path),
        "toolchain_sha256": Q._digest(current_toolchain),
        "toolchain_root": str(toolchain),
        "toolchain_stamp": current_toolchain["stamp"],
    }
    return CampaignContext(
        catalogue_path, run_root, campaign, identity, inventory, scenes,
        toolchain, bindings)


def _scene_bindings(context: CampaignContext, scene: dict) -> tuple[dict, dict, Path]:
    scene_dir = context.run_root / scene["id"]
    try:
        current_files = {
            "campaign_sha256": Q._file_digest(context.run_root / "campaign.json"),
            "catalogue_sha256": Q._file_digest(context.catalogue_path),
            "identity_sha256": Q._file_digest(context.run_root / "identity.json"),
            "inventory_sha256": Q._file_digest(context.run_root / "inventory.json"),
            "settings_sha256": Q._file_digest(context.run_root / "settings.input.json"),
        }
    except OSError as exc:
        raise AdjudicationError(f"campaign evidence disappeared: {exc}") from exc
    for name, current in current_files.items():
        if current != context.bindings[name]:
            raise AdjudicationError(
                f"{name.removesuffix('_sha256')} changed during adjudication")
    try:
        current_toolchain_sha256 = Q._digest(Q._toolchain_identity(context.toolchain))
    except (OSError, Q.QualificationError) as exc:
        raise AdjudicationError(f"cannot rehash executing toolchain: {exc}") from exc
    if current_toolchain_sha256 != context.bindings["toolchain_sha256"]:
        raise AdjudicationError("toolchain changed during adjudication")
    submission_path = scene_dir / "submission.json"
    result_path = scene_dir / "result.json"
    submission = _read_object(submission_path, "scene submission claim")
    expected_submission = {
        "schema": "forge.modular.scene_submission_claim.v1",
        "campaign_sha256": Q._digest(context.campaign),
        "scene": scene,
        "maximum_provider_submissions": 1,
        "retries": 0,
    }
    if submission != expected_submission:
        raise AdjudicationError(f"{scene['id']} submission claim does not match campaign")
    try:
        result = Q._existing_result(result_path, scene_dir, scene["id"])
    except Q.QualificationError as exc:
        raise AdjudicationError(str(exc)) from exc
    if result.get("status") != "generated":
        raise AdjudicationError(f"{scene['id']} has no generated artifact to adjudicate")
    generation = result.get("generation")
    artifact = (scene_dir / "artifact.vcv").resolve()
    if not isinstance(generation, dict) or generation.get("status") != "generated":
        raise AdjudicationError(f"{scene['id']} generation receipt is not generated")
    if generation.get("idiom") != scene["idiom"]:
        raise AdjudicationError(f"{scene['id']} idiom differs from the catalogue")
    try:
        recorded_artifact = Path(generation.get("artifact", "")).resolve()
    except (OSError, TypeError) as exc:
        raise AdjudicationError(f"{scene['id']} has a malformed artifact path") from exc
    if recorded_artifact != artifact:
        raise AdjudicationError(f"{scene['id']} result points at a different artifact")
    bindings = dict(context.bindings)
    bindings.update({
        "submission_sha256": Q._file_digest(submission_path),
        "result_sha256": Q._file_digest(result_path),
        "result_document_sha256": Q._digest(result),
        "artifact_sha256": Q._file_digest(artifact),
        "idiom": scene["idiom"],
    })
    return result, bindings, artifact


def _rack_bindings(rack_app: Path, rack_log: Path | None, artifact: Path) -> dict:
    app = rack_app.resolve()
    binary = app / "Contents/MacOS/Rack"
    binding = {
        "app": str(app),
        "binary": str(binary),
        "binary_sha256": Q._file_digest(binary) if binary.is_file() else None,
        "app_bundle": _tree_identity(app),
        "rack_log": str(rack_log.resolve()) if rack_log is not None else None,
        "plugin_installation": _plugin_identities(artifact),
        "fidelity_platform": (
            os.environ.get("FORGE_RACK_PLATFORM")
            if os.environ.get("FORGE_RACK_PLATFORM") in {"mac-arm64", "mac-x64"}
            else None),
    }
    return binding


def _rack_user_inputs() -> dict:
    return {
        name: _tree_identity(_rack_user_root() / name)
        for name in ("settings.json", "licenses", "forge-portmap.json")
    }


def _adjudication_bindings(context: CampaignContext, scene: dict,
                           rack_app: Path, rack_log: Path | None
                           ) -> tuple[dict, Path]:
    _, bindings, artifact = _scene_bindings(context, scene)
    bindings["rack"] = _rack_bindings(rack_app, rack_log, artifact)
    return bindings, artifact


def _strict_idiom_and_control(checker: ModuleType, patch: dict, inventory: dict,
                              slug: str) -> dict[str, dict]:
    try:
        idioms = checker.load_idioms()
        roles = checker.load_roles()
        idiom = idioms.get(slug)
        if idiom is None:
            raise AdjudicationError(f"no such idiom in executing toolchain: {slug}")
        unchecked: list[str] = []
        findings = checker.check(patch, inventory, idiom, roles, unchecked)
    except AdjudicationError:
        raise
    except Exception as exc:
        withheld = {"status": "WITHHOLD", "error": f"idiom checker crashed: {exc}"}
        return {
            "idiom_contract": withheld,
            "causal_witness": dict(withheld),
            "negative_control": dict(withheld),
        }

    contract = {
        "status": "PASS" if not findings and not unchecked else
                  ("FAIL" if findings else "WITHHOLD"),
        "findings": findings,
        "unchecked": unchecked,
    }
    if contract["status"] != "PASS":
        blocked = {
            "status": "WITHHOLD",
            "error": "positive idiom contract is not fully proven",
        }
        return {
            "idiom_contract": contract,
            "causal_witness": dict(blocked),
            "negative_control": dict(blocked),
        }

    mistakes = idiom.get("common_mistakes") or []
    for source_mistake in mistakes:
        mistake = dict(source_mistake)
        mistake["_topology"] = idiom.get("topology", [])
        mistake["_forbidden_topology"] = idiom.get("forbidden_topology", [])
        try:
            broken = checker.apply_mistake(patch, mistake, inventory, roles)
        except Exception as exc:
            withheld = {
                "status": "WITHHOLD", "mistake_id": mistake.get("id"),
                "error": f"negative-control mutation crashed: {exc}",
            }
            return {
                "idiom_contract": contract,
                "causal_witness": withheld,
                "negative_control": dict(withheld),
            }
        if broken is None:
            continue
        witness = {
            "mistake_id": mistake.get("id"),
            "operation": mistake.get("do"),
            "original_sha256": Q._digest(patch),
            "mutated_sha256": Q._digest(broken),
        }
        if witness["original_sha256"] == witness["mutated_sha256"]:
            witness.update({
                "status": "WITHHOLD",
                "error": "first applicable mutation did not change the patch",
            })
            return {
                "idiom_contract": contract,
                "causal_witness": witness,
                "negative_control": {
                    "status": "WITHHOLD", "mistake_id": mistake.get("id"),
                    "error": "mutation did not materialize",
                },
            }
        witness["status"] = "PASS"
        try:
            mutated_unchecked: list[str] = []
            mutated_findings = checker.check(
                broken, inventory, idiom, roles, mutated_unchecked)
        except Exception as exc:
            negative = {
                "status": "WITHHOLD", "mistake_id": mistake.get("id"),
                "error": f"mutated idiom check crashed: {exc}",
            }
        else:
            negative_status = (
                "PASS" if mutated_findings else
                "WITHHOLD" if mutated_unchecked else "FAIL")
            negative = {
                "status": negative_status,
                "mistake_id": mistake.get("id"),
                "findings": mutated_findings,
                "unchecked": mutated_unchecked,
            }
            if mutated_unchecked:
                negative["error"] = "negative control has unchecked evidence"
            elif not mutated_findings:
                negative["error"] = "idiom still holds after its negative control"
        return {
            "idiom_contract": contract,
            "causal_witness": witness,
            "negative_control": negative,
        }

    withheld = {
        "status": "WITHHOLD",
        "error": "no common mistake applies to the generated patch",
    }
    return {
        "idiom_contract": contract,
        "causal_witness": withheld,
        "negative_control": dict(withheld),
    }


def _recorded_command_status(check: dict, expected: dict[int, str]) -> str:
    exit_code = check.get("exit_code")
    if isinstance(exit_code, int) and not isinstance(exit_code, bool):
        stdout = check.get("stdout")
        stderr = check.get("stderr")
        if not isinstance(stdout, str) or not isinstance(stderr, str) or \
                check.get("stdout_sha256") != hashlib.sha256(stdout.encode()).hexdigest() or \
                check.get("stderr_sha256") != hashlib.sha256(stderr.encode()).hexdigest():
            raise AdjudicationError("recorded command output evidence is malformed")
        crashed = exit_code < 0 or "Traceback (most recent call last)" in stderr
        return "WITHHOLD" if crashed else expected.get(exit_code, "WITHHOLD")
    if isinstance(check.get("error"), str):
        return "WITHHOLD"
    raise AdjudicationError("recorded command verdict has no execution evidence")


def _recorded_check_status(name: str, check: dict, has_audio: bool) -> str:
    if name == "module_availability":
        return _recorded_command_status(check, {0: "PASS", 1: "FAIL"})
    if name == "rack_load":
        status = _recorded_command_status(check, {0: "PASS"})
        if (status == "PASS" and check.get("error") ==
                "Rack user inputs changed while the load check was executing"):
            return "WITHHOLD"
        return status
    if name == "dsp_and_audio":
        status = _recorded_command_status(
            check, {0: "PASS", 1: "FAIL", 2: "WITHHOLD"})
        if (status in {"PASS", "FAIL"} and check.get("probe") is None and
                check.get("error") ==
                "fidelity returned a verdict without preserved probe evidence"):
            return "WITHHOLD"
        if (status in {"PASS", "FAIL"} and check.get("error") ==
                "Rack user inputs changed while fidelity was executing"):
            return "WITHHOLD"
        if (status in {"PASS", "FAIL"} and not has_audio and
                check.get("error") ==
                "fidelity returned a verdict without a non-empty WAV artifact"):
            return "WITHHOLD"
        return status
    if name == "idiom_contract":
        findings = check.get("findings")
        unchecked = check.get("unchecked")
        if isinstance(findings, list) and isinstance(unchecked, list):
            return "FAIL" if findings else "WITHHOLD" if unchecked else "PASS"
    elif name == "causal_witness":
        original = check.get("original_sha256")
        mutated = check.get("mutated_sha256")
        if isinstance(original, str) and isinstance(mutated, str):
            return "PASS" if original != mutated else "WITHHOLD"
    elif name == "negative_control":
        findings = check.get("findings")
        unchecked = check.get("unchecked")
        if isinstance(findings, list) and isinstance(unchecked, list):
            return "PASS" if findings else "WITHHOLD" if unchecked else "FAIL"
    if isinstance(check.get("error"), str):
        return "WITHHOLD"
    raise AdjudicationError(f"recorded {name} verdict evidence is malformed")


def _validate_existing(path: Path, scene_id: str, bindings: dict) -> dict:
    receipt = _read_object(path, "scene adjudication")
    if receipt.get("schema") != ADJUDICATION_SCHEMA or receipt.get("scene_id") != scene_id:
        raise AdjudicationError(f"{scene_id} adjudication has wrong schema or identity")
    if receipt.get("bindings") != bindings:
        raise AdjudicationError(f"{scene_id} adjudication evidence changed")
    checks = receipt.get("checks")
    if not isinstance(checks, dict) or set(checks) != set(CHECK_NAMES):
        raise AdjudicationError(f"{scene_id} adjudication has the wrong checks")
    if any(not isinstance(checks[name], dict) or
           checks[name].get("status") not in STATUSES for name in CHECK_NAMES):
        raise AdjudicationError(f"{scene_id} adjudication has an invalid check status")
    # This detects corruption and uncoordinated edits inside one local trust
    # principal; it is not authentication against that principal.  Durable
    # authenticity begins when CI archives the receipt outside this run root.
    if receipt.get("check_evidence_sha256") != _check_evidence_digest(checks):
        raise AdjudicationError(f"{scene_id} recorded check evidence changed")
    has_audio = isinstance(receipt.get("audio"), dict)
    for name in CHECK_NAMES:
        if checks[name]["status"] != _recorded_check_status(
                name, checks[name], has_audio):
            raise AdjudicationError(
                f"{scene_id} {name} classification does not match its evidence")
    if receipt.get("status") != _status(checks):
        raise AdjudicationError(f"{scene_id} adjudication aggregate status is invalid")
    audio = receipt.get("audio")
    if checks["dsp_and_audio"]["status"] in {"PASS", "FAIL"} and audio is None:
        raise AdjudicationError(
            f"{scene_id} DSP/audio verdict has no preserved WAV evidence")
    if (checks["dsp_and_audio"]["status"] in {"PASS", "FAIL"} and
            checks["dsp_and_audio"].get("probe") is None):
        raise AdjudicationError(
            f"{scene_id} DSP/audio verdict has no preserved probe evidence")
    probe = checks["dsp_and_audio"].get("probe")
    probe_path = path.parent / "fidelity-probe/ForgeProbe"
    if probe is not None and probe != _tree_identity(probe_path):
        raise AdjudicationError(
            f"{scene_id} preserved fidelity probe is missing or changed")
    if audio is not None:
        relative = audio.get("path") if isinstance(audio, dict) else None
        expected_name = (f"audio-{audio.get('sha256')}.wav"
                         if isinstance(audio, dict) else None)
        audio_path = path.parent / relative if isinstance(relative, str) else path
        if not isinstance(audio, dict) or relative != expected_name or \
                audio_path.parent != path.parent or not audio_path.is_file() or \
                audio.get("bytes") != audio_path.stat().st_size or \
                audio.get("sha256") != Q._file_digest(audio_path):
            raise AdjudicationError(f"{scene_id} audio artifact is missing or changed")
    return receipt


def adjudicate_scene(context: CampaignContext, scene: dict, rack_app: Path,
                     *, rack_log: Path | None = None,
                     runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
                     idiom_loader: Callable[[Path], ModuleType] = _load_idiom_checker,
                     ) -> dict:
    scene_dir = context.run_root / scene["id"]
    bindings, artifact = _adjudication_bindings(
        context, scene, rack_app, rack_log)
    receipt_path = scene_dir / "adjudication.json"
    lock_path = scene_dir / ".adjudication.lock"
    lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        try:
            file_lock.exclusive_nonblocking(lock_fd)
        except BlockingIOError as exc:
            raise AdjudicationError(f"{scene['id']} has an active adjudication owner") from exc
        if receipt_path.is_file():
            return _validate_existing(receipt_path, scene["id"], bindings)

        python = sys.executable
        checks: dict[str, dict] = {}
        checks["module_availability"] = _run(
            [python, str(context.toolchain / "patch.py"), "verify", str(artifact)],
            runner, {0: "PASS", 1: "FAIL"})

        rack_app = rack_app.resolve()
        rack_command = [
            python, str(context.toolchain / "rack_open.py"),
            "--app", str(rack_app), "--patch", str(artifact),
        ]
        if rack_log is not None:
            rack_command.extend(["--log", str(rack_log.resolve())])
        if bindings["rack"]["binary_sha256"] is None:
            checks["rack_load"] = {
                "status": "WITHHOLD", "argv": rack_command,
                "error": f"Rack executable is unavailable: {bindings['rack']['binary']}",
            }
        else:
            rack_user_inputs = _rack_user_inputs()
            checks["rack_load"] = _run(rack_command, runner, {0: "PASS"})
            rack_user_inputs_after = _rack_user_inputs()
            checks["rack_load"]["user_inputs_before"] = rack_user_inputs
            checks["rack_load"]["user_inputs_after"] = rack_user_inputs_after
            if rack_user_inputs_after != rack_user_inputs:
                checks["rack_load"]["status"] = "WITHHOLD"
                checks["rack_load"]["error"] = (
                    "Rack user inputs changed while the load check was executing")

        try:
            patch = _read_object(artifact, "generated Rack patch")
            idiom_checks = _strict_idiom_and_control(
                idiom_loader(context.toolchain), patch,
                context.inventory["inventory"], scene["idiom"])
        except AdjudicationError as exc:
            withheld = {"status": "WITHHOLD", "error": str(exc)}
            idiom_checks = {
                "idiom_contract": withheld,
                "causal_witness": dict(withheld),
                "negative_control": dict(withheld),
            }
        checks.update(idiom_checks)

        with tempfile.TemporaryDirectory(prefix="scene-audio-", dir=scene_dir) as tmp:
            temporary_audio = Path(tmp) / "audio.wav"
            probe_root = scene_dir / "fidelity-probe"
            fidelity_command = [
                python, str(context.toolchain / "fidelity.py"), str(artifact),
                "--wav", str(temporary_audio),
                "--preserve-probe", str(probe_root),
            ]
            fidelity_environment = {
                "FORGE_RACK_BIN": bindings["rack"]["binary"],
                "FORGE_RACK_PLATFORM": bindings["rack"]["fidelity_platform"] or "",
            }
            fidelity_user_inputs = _rack_user_inputs()
            checks["dsp_and_audio"] = _run(
                fidelity_command, runner,
                {0: "PASS", 1: "FAIL", 2: "WITHHOLD"},
                fidelity_environment)
            fidelity_user_inputs_after = _rack_user_inputs()
            checks["dsp_and_audio"]["user_inputs_before"] = fidelity_user_inputs
            checks["dsp_and_audio"]["user_inputs_after"] = fidelity_user_inputs_after
            if fidelity_user_inputs_after != fidelity_user_inputs:
                checks["dsp_and_audio"]["status"] = "WITHHOLD"
                checks["dsp_and_audio"]["error"] = (
                    "Rack user inputs changed while fidelity was executing")
            checks["dsp_and_audio"]["probe"] = _tree_identity(
                probe_root / "ForgeProbe")
            if (checks["dsp_and_audio"]["status"] in {"PASS", "FAIL"} and
                    checks["dsp_and_audio"]["probe"] is None):
                checks["dsp_and_audio"]["status"] = "WITHHOLD"
                checks["dsp_and_audio"]["error"] = (
                    "fidelity returned a verdict without preserved probe evidence")
            audio = None
            if (checks["dsp_and_audio"]["status"] in {"PASS", "FAIL"} and
                    (not temporary_audio.is_file() or
                     temporary_audio.stat().st_size == 0)):
                checks["dsp_and_audio"]["status"] = "WITHHOLD"
                checks["dsp_and_audio"]["error"] = (
                    "fidelity returned a verdict without a non-empty WAV artifact")
            if temporary_audio.is_file() and temporary_audio.stat().st_size > 0:
                payload = temporary_audio.read_bytes()
                digest = hashlib.sha256(payload).hexdigest()
                audio_path = scene_dir / f"audio-{digest}.wav"
                try:
                    Q._write_bytes_exclusive(audio_path, payload)
                except FileExistsError:
                    if Q._file_digest(audio_path) != digest:
                        checks["dsp_and_audio"] = {
                            "status": "WITHHOLD", "argv": fidelity_command,
                            "error": "content-addressed audio evidence changed",
                        }
                except OSError as exc:
                    checks["dsp_and_audio"] = {
                        "status": "WITHHOLD", "argv": fidelity_command,
                        "error": f"cannot preserve audio evidence: {exc}",
                    }
                if (checks["dsp_and_audio"]["status"] != "WITHHOLD" and
                        audio_path.is_file() and Q._file_digest(audio_path) == digest):
                    audio = {
                        "path": audio_path.name,
                        "sha256": digest,
                        "bytes": len(payload),
                    }

        receipt = {
            "schema": ADJUDICATION_SCHEMA,
            "scene_id": scene["id"],
            "status": _status(checks),
            "bindings": bindings,
            "checks": checks,
            "check_evidence_sha256": _check_evidence_digest(checks),
        }
        if audio is not None:
            receipt["audio"] = audio
        final_bindings, _ = _adjudication_bindings(
            context, scene, rack_app, rack_log)
        if final_bindings != bindings:
            raise AdjudicationError(
                f"{scene['id']} generation evidence changed during adjudication")
        try:
            Q._write_exclusive(receipt_path, receipt)
        except FileExistsError:
            return _validate_existing(receipt_path, scene["id"], bindings)
        return receipt
    finally:
        os.close(lock_fd)


def write_summary(context: CampaignContext, rack_app: Path,
                  rack_log: Path | None = None) -> dict:
    scene_receipts = []
    statuses = []
    for scene in context.scenes:
        scene_id = scene["id"]
        path = context.run_root / scene_id / "adjudication.json"
        if not path.is_file():
            raise AdjudicationError(
                f"cannot summarize without all 12 adjudications: missing {scene_id}")
        bindings, _ = _adjudication_bindings(
            context, scene, rack_app, rack_log)
        receipt = _validate_existing(path, scene_id, bindings)
        statuses.append(receipt["status"])
        scene_receipts.append({
            "scene_id": scene_id,
            "status": receipt["status"],
            "adjudication_sha256": Q._file_digest(path),
        })
    if len(scene_receipts) != len(Q.SCENE_IDS):
        raise AdjudicationError("summary must bind exactly 12 scene adjudications")
    status = ("WITHHOLD" if any(item == "WITHHOLD" for item in statuses) else
              "FAIL" if any(item == "FAIL" for item in statuses) else "PASS")
    summary = {
        "schema": SUMMARY_SCHEMA,
        "status": status,
        "campaign_sha256": context.bindings["campaign_sha256"],
        "catalogue_sha256": context.bindings["catalogue_sha256"],
        "scene_ids": list(Q.SCENE_IDS),
        "scenes": scene_receipts,
    }
    try:
        Q._write_exclusive(context.run_root / "qualification-summary.json", summary)
    except FileExistsError:
        existing = _read_object(
            context.run_root / "qualification-summary.json", "qualification summary")
        if existing != summary:
            raise AdjudicationError("qualification summary is owned by different evidence")
    return summary


def adjudicate_campaign(*, catalogue: Path, run_root: Path, rack_app: Path,
                        rack_log: Path | None = None,
                        runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
                        idiom_loader: Callable[[Path], ModuleType] = _load_idiom_checker,
                        ) -> int:
    context = _load_context(catalogue, run_root)
    for scene in context.scenes:
        adjudicate_scene(
            context, scene, rack_app, rack_log=rack_log,
            runner=runner, idiom_loader=idiom_loader)
    status = write_summary(context, rack_app, rack_log)["status"]
    return {"PASS": 0, "FAIL": 1, "WITHHOLD": 2}[status]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalogue", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--rack-app", type=Path, required=True)
    parser.add_argument("--rack-log", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return adjudicate_campaign(
            catalogue=args.catalogue, run_root=args.run_root,
            rack_app=args.rack_app, rack_log=args.rack_log)
    except (AdjudicationError, Q.QualificationError) as exc:
        print(f"adjudicate-scenes: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
