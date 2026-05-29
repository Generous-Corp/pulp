#!/usr/bin/env python3
"""Build a resource-session manifest from a PulpFrontendIR report."""

from __future__ import annotations

import argparse
import hashlib
import json
import mimetypes
import pathlib
from typing import Any


def load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return data


def write_json(path: pathlib.Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def as_dict(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def repo_relative(path: pathlib.Path, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def resolve_local_path(value: Any, repo_root: pathlib.Path) -> pathlib.Path | None:
    if not isinstance(value, str) or not value:
        return None
    if "://" in value or value.startswith("data:"):
        return None
    path = pathlib.Path(value)
    if not path.is_absolute():
        path = repo_root / path
    return path


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def local_metadata(path: pathlib.Path, repo_root: pathlib.Path) -> dict[str, Any]:
    entry: dict[str, Any] = {
        "path": repo_relative(path, repo_root),
        "exists": path.exists(),
    }
    if path.exists() and path.is_file():
        entry["byte_size"] = path.stat().st_size
        entry["sha256"] = sha256_file(path)
        mime, _ = mimetypes.guess_type(path.name)
        if mime:
            entry["mime"] = mime
    return entry


def artifact_entry(
    artifact_id: str,
    kind: str,
    path_value: Any,
    repo_root: pathlib.Path,
    *,
    sha256: str | None = None,
    watch: bool = False,
    watch_reason: str = "",
    schema: str = "",
) -> dict[str, Any] | None:
    path = resolve_local_path(path_value, repo_root)
    if path is None:
        return None
    entry = {
        "id": artifact_id,
        "kind": kind,
        "watch": watch,
    }
    entry.update(local_metadata(path, repo_root))
    if sha256 and "sha256" not in entry:
        entry["sha256"] = sha256
    if watch_reason:
        entry["watch_reason"] = watch_reason
    if schema:
        entry["schema"] = schema
    return entry


def add_artifact(artifacts: list[dict[str, Any]], entry: dict[str, Any] | None) -> None:
    if entry is None:
        return
    path = entry.get("path")
    for existing in artifacts:
        if path and existing.get("path") == path:
            return
    artifacts.append(entry)


def artifact_from_ref(
    artifact_id: str,
    ref: dict[str, Any],
    repo_root: pathlib.Path,
    *,
    default_kind: str,
    watch: bool = False,
    watch_reason: str = "",
) -> dict[str, Any] | None:
    return artifact_entry(
        artifact_id,
        str(ref.get("kind") or default_kind),
        ref.get("path"),
        repo_root,
        sha256=ref.get("sha256") if isinstance(ref.get("sha256"), str) else None,
        watch=watch,
        watch_reason=watch_reason,
        schema=str(ref.get("schema") or ""),
    )


def resource_entries(report: dict[str, Any], repo_root: pathlib.Path) -> list[dict[str, Any]]:
    entries = []
    for resource in as_list(report.get("resources")):
        if not isinstance(resource, dict):
            continue
        entry: dict[str, Any] = {
            "id": resource.get("id", ""),
            "kind": "resource",
            "origin": resource.get("original_uri", ""),
            "resolved_uri": resource.get("resolved_uri", ""),
            "mime": resource.get("mime", ""),
            "sha256": resource.get("sha256", ""),
            "byte_size": resource.get("byte_size", 0),
            "watch": resource.get("watch") is True,
            "watch_reason": "resource",
            "route_usage": as_list(resource.get("route_usage")),
            "requested_by": as_list(resource.get("requested_by")),
            "transforms": as_list(resource.get("transforms")),
        }
        if "bundle_destination" in resource:
            entry["bundle_destination"] = resource["bundle_destination"]
        path = resolve_local_path(resource.get("resolved_uri"), repo_root)
        if path is not None:
            metadata = local_metadata(path, repo_root)
            entry["path"] = metadata["path"]
            entry["exists"] = metadata["exists"]
            for key in ("sha256", "byte_size", "mime"):
                if metadata.get(key):
                    entry[key] = metadata[key]
        entries.append(entry)
    return sorted(entries, key=lambda item: str(item.get("id", "")))


def proof_artifact_refs(report: dict[str, Any], repo_root: pathlib.Path) -> list[dict[str, Any]]:
    artifacts = []
    validation = as_dict(report.get("validation"))
    for index, proof in enumerate(as_list(validation.get("proofs"))):
        if not isinstance(proof, dict):
            continue
        add_artifact(
            artifacts,
            artifact_entry(
                f"proof.{index}",
                str(proof.get("kind") or "proof"),
                proof.get("path"),
                repo_root,
            ),
        )
    for key in ("compile", "binary_dependencies"):
        proof_ref = as_dict(as_dict(validation.get(key)).get("proof_artifact"))
        add_artifact(
            artifacts,
            artifact_from_ref(f"{key}.proof", proof_ref, repo_root, default_kind="native_proof"),
        )
    for index, screenshot in enumerate(as_list(validation.get("screenshots"))):
        if not isinstance(screenshot, dict):
            continue
        add_artifact(
            artifacts,
            artifact_entry(
                f"screenshot.{index}",
                str(screenshot.get("kind") or "screenshot"),
                screenshot.get("path"),
                repo_root,
                sha256=screenshot.get("sha256") if isinstance(screenshot.get("sha256"), str) else None,
            ),
        )
    return artifacts


def route_summary(report: dict[str, Any]) -> dict[str, Any]:
    counts: dict[str, int] = {}
    js_required = 0
    fallback = 0
    for route in as_list(report.get("routes")):
        if not isinstance(route, dict):
            continue
        chosen = route.get("chosen_route")
        if isinstance(chosen, str) and chosen:
            counts[chosen] = counts.get(chosen, 0) + 1
        if route.get("requires_js_engine") is True:
            js_required += 1
        reason = route.get("fallback_reason")
        if isinstance(reason, str) and reason:
            fallback += 1
    return {
        "total": sum(counts.values()),
        "by_route": dict(sorted(counts.items())),
        "js_required": js_required,
        "fallback": fallback,
    }


def token_summary(report: dict[str, Any]) -> dict[str, int]:
    tokens = as_dict(report.get("tokens"))
    resolved = sum(1 for token in tokens.values() if isinstance(token, dict) and "resolved_value" in token)
    return {
        "total": len(tokens),
        "resolved": resolved,
        "unresolved": len(tokens) - resolved,
    }


def tweak_summary(report: dict[str, Any]) -> dict[str, Any]:
    tweaks = as_list(report.get("tweaks"))
    invalidations: dict[str, int] = {}
    for tweak in tweaks:
        if not isinstance(tweak, dict):
            continue
        for scope in as_list(tweak.get("invalidates")):
            if isinstance(scope, str) and scope:
                invalidations[scope] = invalidations.get(scope, 0) + 1
    return {
        "total": len(tweaks),
        "invalidations": dict(sorted(invalidations.items())),
    }


def gate_summary(gates: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        {
            "mode": gate.get("mode", ""),
            "verdict": gate.get("verdict", ""),
            "summary": gate.get("summary", {}),
        }
        for gate in gates
    ]


def watch_summary(artifacts: list[dict[str, Any]], resources: list[dict[str, Any]]) -> dict[str, Any]:
    paths = []
    missing_paths = []
    resource_ids = []
    for entry in artifacts:
        if entry.get("watch") is True and isinstance(entry.get("path"), str) and entry["path"]:
            paths.append(entry["path"])
            if entry.get("exists") is False:
                missing_paths.append(entry["path"])
    for resource in resources:
        if resource.get("watch") is True:
            if isinstance(resource.get("path"), str) and resource["path"]:
                paths.append(resource["path"])
                if resource.get("exists") is False:
                    missing_paths.append(resource["path"])
            if isinstance(resource.get("id"), str) and resource["id"]:
                resource_ids.append(resource["id"])
    return {
        "paths": sorted(set(paths)),
        "missing_paths": sorted(set(missing_paths)),
        "resource_ids": sorted(set(resource_ids)),
    }


def build_session_manifest(
    report: dict[str, Any],
    frontend_ir_path: pathlib.Path,
    repo_root: pathlib.Path,
    *,
    gates: list[tuple[pathlib.Path, dict[str, Any]]] | None = None,
    inspector: tuple[pathlib.Path, dict[str, Any]] | None = None,
    session_diff: tuple[pathlib.Path, dict[str, Any]] | None = None,
    tweak_sidecars: list[pathlib.Path] | None = None,
) -> dict[str, Any]:
    gates = gates or []
    tweak_sidecars = tweak_sidecars or []
    artifacts: list[dict[str, Any]] = []
    add_artifact(
        artifacts,
        artifact_entry("frontend_ir", "frontend_ir", frontend_ir_path.as_posix(), repo_root),
    )

    source = as_dict(report.get("source"))
    source_kind = str(source.get("kind") or "source")
    add_artifact(
        artifacts,
        artifact_entry(
            "source",
            f"source_{source_kind}",
            source.get("path"),
            repo_root,
            sha256=source.get("sha256") if isinstance(source.get("sha256"), str) else None,
            watch=True,
            watch_reason="source_contract",
        ),
    )
    add_artifact(
        artifacts,
        artifact_from_ref(
            "route_manifest",
            as_dict(report.get("route_manifest")),
            repo_root,
            default_kind="route_manifest",
            watch=True,
            watch_reason="route_contract",
        ),
    )
    add_artifact(
        artifacts,
        artifact_from_ref("design_ir", as_dict(report.get("design_ir")), repo_root, default_kind="design_ir"),
    )
    for index, (path, gate) in enumerate(gates):
        add_artifact(
            artifacts,
            artifact_entry(f"gate.{index}", "frontend_ir_gate", path.as_posix(), repo_root, schema=str(gate.get("schema") or "")),
        )
    if inspector is not None:
        path, data = inspector
        add_artifact(
            artifacts,
            artifact_entry("inspector", "frontend_ir_inspector", path.as_posix(), repo_root, schema=str(data.get("schema") or "")),
        )
    if session_diff is not None:
        path, data = session_diff
        add_artifact(
            artifacts,
            artifact_entry("session_diff", "frontend_ir_session_diff", path.as_posix(), repo_root, schema=str(data.get("schema") or "")),
        )
    for index, path in enumerate(tweak_sidecars):
        add_artifact(
            artifacts,
            artifact_entry(
                f"tweak_sidecar.{index}",
                "tweak_sidecar",
                path.as_posix(),
                repo_root,
                watch=True,
                watch_reason="tweak_sidecar",
            ),
        )
    for artifact in proof_artifact_refs(report, repo_root):
        add_artifact(artifacts, artifact)

    resources = resource_entries(report, repo_root)
    watch = watch_summary(artifacts, resources)
    validation = as_dict(report.get("validation"))
    session_diff_summary = as_dict(as_dict(session_diff[1]).get("summary")) if session_diff else {}
    inspector_summary = as_dict(as_dict(inspector[1]).get("summary")) if inspector else {}
    return {
        "schema": "pulp-frontend-ir-session-v0",
        "fixture_id": str(report.get("fixture_id", "")),
        "summary": {
            "artifacts": len(artifacts),
            "resources": len(resources),
            "watchable_paths": len(watch["paths"]),
            "routes": route_summary(report),
            "tokens": token_summary(report),
            "tweaks": tweak_summary(report),
        },
        "reload_policy": {
            "source_contract": "full_reimport",
            "route_contract": "full_reimport",
            "style_resource": "style_resource_reload",
            "resource": "resource_reload",
            "token_or_tweak": "token_tweak_reload",
            "current_recommendation": session_diff_summary.get("recommended_reload", ""),
            "narrow_reload_safe": session_diff_summary.get("narrow_reload_safe"),
        },
        "artifacts": sorted(artifacts, key=lambda item: str(item.get("id", ""))),
        "resources": resources,
        "watch": watch,
        "validation": {
            "compile": validation.get("compile", {}),
            "binary_dependencies": validation.get("binary_dependencies", {}),
            "gates": gate_summary([gate for _, gate in gates]),
            "inspector": inspector_summary,
            "session_diff": session_diff_summary,
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend-ir", required=True, type=pathlib.Path)
    parser.add_argument("--gate", action="append", type=pathlib.Path, default=[])
    parser.add_argument("--inspector", type=pathlib.Path)
    parser.add_argument("--session-diff", type=pathlib.Path)
    parser.add_argument("--tweak-sidecar", action="append", type=pathlib.Path, default=[])
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    args = parser.parse_args(argv)

    report = load_json(args.frontend_ir)
    gates = [(path, load_json(path)) for path in args.gate]
    inspector = (args.inspector, load_json(args.inspector)) if args.inspector else None
    session_diff = (args.session_diff, load_json(args.session_diff)) if args.session_diff else None
    write_json(
        args.output,
        build_session_manifest(
            report,
            args.frontend_ir,
            args.repo_root,
            gates=gates,
            inspector=inspector,
            session_diff=session_diff,
            tweak_sidecars=args.tweak_sidecar,
        ),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
