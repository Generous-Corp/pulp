#!/usr/bin/env python3
"""Generate Pulp's exact Vellum ownership projection from the initial cut.

The cut manifest is the only input for candidate/deferred source membership.
Every manifest blob belongs to exactly one semantic slice. Candidate slices
contain exact selected files and may contain explicit ``unresolved`` rows, but
never optional, excluded, or Pulp-specific rows.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import Iterable


DEFAULT_MANIFEST = Path("docs/contracts/vellum-initial-cut-manifest.json")
DEFAULT_OUTPUT = Path(".github/vellum-ownership.json")
FRAMEWORK_REPOSITORY = "Generous-Corp/vellum"
FREEZE_OWNER = "@danielraffel"

TRANSFERABLE = frozenset(
    {"framework-core", "authoring-only", "platform-adapter", "test-only"}
)
CANDIDATE_ALLOWED = TRANSFERABLE | {"unresolved"}
DEFERRED = frozenset({"optional", "excluded"})

CODE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".h", ".hpp", ".m", ".mm", ".js", ".jsx", ".ts", ".tsx"}
)
AUDIO_PLUGIN_COMPONENTS = frozenset(
    {"audio", "auv2", "auv3", "clap", "dsp", "midi", "plugin", "signalgraph", "vst"}
)
# A Figma plugin is an authoring source, not an audio-plugin runtime concept.
NEUTRAL_IDENTIFIERS = frozenset({"figma_plugin"})


class ProjectionError(RuntimeError):
    """The cut cannot be represented as an exact ownership projection."""


def load_manifest(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProjectionError(f"cannot read cut manifest {path}: {error}") from error
    if not isinstance(value, dict) or value.get("schema") != "pulp.vellum.initial-cut-manifest.v1":
        raise ProjectionError("cut manifest has an unsupported schema")
    entries = value.get("entries")
    if not isinstance(entries, list) or value.get("entry_count") != len(entries):
        raise ProjectionError("cut manifest entry_count does not match entries")
    return value


def _is_capture(path: str) -> bool:
    name = Path(path).name
    return name.startswith("screenshot") or "_capture." in name


def semantic_slice(path: str, classification: str, classification_source: str) -> str:
    """Return the one semantic slice that owns a manifest row."""

    if path in {"LICENSE.md", "NOTICE.md", "DEPENDENCIES.md"}:
        return "legal-provenance"
    if path.startswith("core/canvas/"):
        if classification in DEFERRED or classification_source in {
            "derived:pulp-build-graph",
            "derived:audio-plugin-neutrality-split",
        }:
            return "canvas-kernel-deferred"
        return "canvas-kernel"
    if path.startswith("core/render/"):
        if classification in DEFERRED or classification_source in {
            "derived:pulp-build-graph",
            "derived:audio-plugin-neutrality-split",
        }:
            return "render-skia-dawn-deferred"
        return "render-skia-dawn"
    if path.startswith("core/view/") and _is_capture(path):
        return "capture-primitives"
    if path.startswith("core/view/platform/mac/"):
        return "macos-shell"
    if (
        "/design_" in path
        or path.endswith("/anchor_strategy.hpp")
        or path.endswith("/anchor_strategy.cpp")
        or path.startswith("packages/pulp-import-ir/")
    ):
        return "design-schema-compiler"
    if path.startswith(("core/view/include/", "core/view/src/")):
        if classification_source == "derived:audio-plugin-neutrality-split":
            return "retained-ui-kernel-deferred"
        return "retained-ui-kernel"
    if path.startswith(("external/fonts/", "external/nanosvg/")):
        return "runtime-assets"
    if path == "tools/figma-plugin/schema/figma-plugin-export-v1.json":
        return "legacy-figma-schema"
    raise ProjectionError(f"manifest row has no semantic slice: {path}")


def _candidate_slice(slice_id: str) -> bool:
    return slice_id not in {
        "canvas-kernel-deferred",
        "render-skia-dawn-deferred",
        "retained-ui-kernel-deferred",
        "legacy-figma-schema",
    }


def build_projection(manifest: dict[str, object]) -> dict[str, object]:
    entries = manifest["entries"]
    assert isinstance(entries, list)
    grouped: dict[str, list[str]] = {}
    seen_paths: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise ProjectionError("cut manifest entries must be objects")
        path = entry.get("source_path")
        classification = entry.get("classification")
        source = entry.get("classification_source")
        if not all(isinstance(value, str) for value in (path, classification, source)):
            raise ProjectionError("cut manifest entry is missing string fields")
        assert isinstance(path, str) and isinstance(classification, str) and isinstance(source, str)
        if path in seen_paths:
            raise ProjectionError(f"manifest path occurs more than once: {path}")
        seen_paths.add(path)
        slice_id = semantic_slice(path, classification, source)
        if _candidate_slice(slice_id) and classification not in CANDIDATE_ALLOWED:
            raise ProjectionError(
                f"candidate slice {slice_id} contains forbidden {classification} row: {path}"
            )
        grouped.setdefault(slice_id, []).append(path)

    slices: list[dict[str, object]] = []
    for slice_id in sorted(grouped):
        if slice_id == "legacy-figma-schema":
            state = "pulp-only"
        elif _candidate_slice(slice_id):
            state = "pulp-authoritative-untransferred"
        else:
            state = "excluded"
        slices.append({"id": slice_id, "state": state, "paths": sorted(grouped[slice_id])})

    # These are intentionally broad Pulp-owned product/tool surfaces outside
    # the selected cut. They document the non-transfer boundary and therefore
    # are not part of the exact manifest-row coverage calculation.
    slices.extend(
        [
            {
                "id": "pulp-audio-plugin-product",
                "state": "pulp-only",
                "paths": [
                    "core/audio/",
                    "core/format/",
                    "core/gpu_audio/",
                    "core/graph/",
                    "core/host/",
                    "core/midi/",
                    "core/playback/",
                    "core/signal/",
                ],
            },
            {
                "id": "pulp-tooling-surface",
                "state": "pulp-only",
                "paths": [
                    ".agents/skills/",
                    ".claude-plugin/",
                    ".claude/commands/",
                    ".mcp.json",
                    "tools/cli/",
                    "tools/mcp/",
                ],
            },
        ]
    )
    slices.sort(key=lambda item: str(item["id"]))
    return {
        "schema_version": 1,
        "framework_repository": FRAMEWORK_REPOSITORY,
        "freeze_owner": FREEZE_OWNER,
        "activation": {
            "state": "prepared",
            "pulp_extraction_base": manifest["source_commit"],
            "vellum_authority_commit": None,
            "accepted_by": None,
            "accepted_at": None,
        },
        "slices": slices,
    }


def _source_without_comments_and_strings(source: str) -> str:
    # This is deliberately a conservative lexical scan, not a compiler. It
    # removes prose and literals so an ordinary word in a comment cannot turn a
    # generic file into an extraction blocker, while retaining declarations,
    # identifiers, and preprocessor structure.
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.DOTALL)
    source = re.sub(r"//[^\n]*", " ", source)
    source = re.sub(r'"(?:\\.|[^"\\])*"', " ", source)
    source = re.sub(r"'(?:\\.|[^'\\])*'", " ", source)
    return source


def _identifier_components(identifier: str) -> list[str]:
    separated = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", identifier)
    return [part.lower() for part in separated.split("_") if part]


def neutrality_findings(repo: Path, manifest: dict[str, object]) -> list[tuple[str, str]]:
    """Find audio/plugin identifiers still claiming a transferable class."""

    findings: set[tuple[str, str]] = set()
    entries = manifest["entries"]
    assert isinstance(entries, list)
    for entry in entries:
        assert isinstance(entry, dict)
        classification = entry.get("classification")
        path_value = entry.get("source_path")
        if classification not in TRANSFERABLE or not isinstance(path_value, str):
            continue
        path = Path(path_value)
        if path.suffix.lower() not in CODE_SUFFIXES:
            continue
        try:
            source = (repo / path).read_text(encoding="utf-8")
        except OSError as error:
            raise ProjectionError(f"cannot scan transferable source {path}: {error}") from error
        stripped = _source_without_comments_and_strings(source)
        for identifier in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", stripped):
            if identifier.lower() in NEUTRAL_IDENTIFIERS:
                continue
            if AUDIO_PLUGIN_COMPONENTS.intersection(_identifier_components(identifier)):
                findings.add((path.as_posix(), identifier))
    return sorted(findings)


def verify_projection(
    *, repo: Path, manifest: dict[str, object], projection: dict[str, object]
) -> None:
    expected = build_projection(manifest)
    if projection != expected:
        raise ProjectionError("ownership projection is stale; regenerate it from the cut manifest")

    manifest_paths = {
        entry["source_path"]
        for entry in manifest["entries"]  # type: ignore[index]
        if isinstance(entry, dict)
    }
    coverage: dict[str, list[str]] = {path: [] for path in manifest_paths}
    candidate_owner: dict[str, str] = {}
    for item in projection["slices"]:  # type: ignore[index]
        assert isinstance(item, dict)
        slice_id = item["id"]
        paths = item["paths"]
        assert isinstance(slice_id, str) and isinstance(paths, list)
        for path in paths:
            if path in coverage:
                coverage[path].append(slice_id)
            if item["state"] == "pulp-authoritative-untransferred":
                previous = candidate_owner.get(path)
                if previous is not None:
                    raise ProjectionError(
                        f"candidate path overlaps {previous} and {slice_id}: {path}"
                    )
                candidate_owner[path] = slice_id

    bad = {path: owners for path, owners in coverage.items() if len(owners) != 1}
    if bad:
        detail = ", ".join(f"{path}={owners}" for path, owners in sorted(bad.items()))
        raise ProjectionError(f"manifest rows must be classified exactly once: {detail}")
    findings = neutrality_findings(repo, manifest)
    if findings:
        detail = ", ".join(f"{path}:{identifier}" for path, identifier in findings)
        raise ProjectionError(
            "transferable classifications contain audio/plugin identifiers; "
            f"reclassify them as unresolved or pulp-specific: {detail}"
        )


def serialize(value: dict[str, object]) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def write_atomic(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
        temporary.write(content)
        temporary.flush()
        os.fsync(temporary.fileno())
        temporary_path = Path(temporary.name)
    os.replace(temporary_path, path)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--verify", action="store_true")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        repo = args.repo.resolve()
        manifest_path = args.manifest if args.manifest.is_absolute() else repo / args.manifest
        output_path = args.output if args.output.is_absolute() else repo / args.output
        manifest = load_manifest(manifest_path)
        expected = build_projection(manifest)
        if args.verify:
            if not output_path.is_file():
                raise ProjectionError(f"ownership projection does not exist: {output_path}")
            try:
                actual = json.loads(output_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                raise ProjectionError(f"cannot read ownership projection: {error}") from error
            verify_projection(repo=repo, manifest=manifest, projection=actual)
            print(f"verified {manifest['entry_count']} exact manifest rows in {output_path}")
            return 0
        findings = neutrality_findings(repo, manifest)
        if findings:
            detail = ", ".join(f"{path}:{identifier}" for path, identifier in findings)
            raise ProjectionError(f"neutrality scan failed: {detail}")
        write_atomic(output_path, serialize(expected))
        print(f"wrote {manifest['entry_count']} exact manifest rows to {output_path}")
        return 0
    except ProjectionError as error:
        print(f"vellum-ownership-projection: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
