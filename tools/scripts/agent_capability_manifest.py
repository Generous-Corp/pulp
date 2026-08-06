#!/usr/bin/env python3
"""Build and validate Pulp's installed agent capability contract.

The consumer manifest describes design-time public API facts. Runtime grants,
instances, activation, policy, risk decisions, and receipts belong to the
unified control platform and are rejected here.
"""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
from typing import Any

import agent_capability_surface as surface
import json_schema_lite
from agent_capability_evolution import (
    contract_payload,
    manifest_evolution_problems as evolution_problems,
    surface_evolution_problems,
)


SCHEMA = "pulp.agent-capabilities.v1"
SCHEMA_MINOR = 0
MANIFEST_REVISION = 1
SURFACE_INVENTORY_VERSION = 1
HISTORY_SCHEMA = "pulp.agent-capability-history.v1"
HISTORY_FILE = pathlib.Path("tools/agent-capabilities/contract-history.json")
SNAPSHOT = pathlib.Path("docs/status/agent-capabilities.json")
MANIFEST_SCHEMA_FILE = pathlib.Path(
    "docs/status/agent-capabilities.schema.json"
)
COMPILE_FIXTURE = pathlib.Path("test/test_agent_capability_compile.cpp")
DOMAINS = {"signal", "music", "midi", "audio", "timebase", "sequence", "offline"}
ROOT_DOMAINS = {item["domain"] for item in surface.PUBLIC_ROOTS}
RT_CLASSES = {"audio", "control", "any", "offline", "mixed"}
STATUSES = {
    "stable",
    "usable",
    "experimental",
    "partial",
    "unsupported",
    "deprecated",
}
BINDING_KINDS = {"cpp_type", "cpp_function"}
EVOLUTION_STATES = {"active", "deprecated"}
REQUIRED_FEATURES = [
    "capability-contract-version-v1",
    "coverage-state-v1",
    "design-runtime-separation-v1",
    "tombstones-v1",
    "typed-bindings-v1",
]
FORBIDDEN_NUMERIC_CONTRACT_FIELDS = {"min", "max", "default", "range", "choices"}
FORBIDDEN_RUNTIME_CONTROL_FIELDS = {
    "activation",
    "authorization",
    "authorizations",
    "consent",
    "grant",
    "grants",
    "instance",
    "instance_id",
    "operation",
    "operation_id",
    "operations",
    "policies",
    "policy",
    "receipt",
    "receipts",
    "revocation",
    "risk",
    "risk_class",
    "session",
    "session_id",
}


def availability() -> dict[str, Any]:
    return {
        "state": "available",
        "platforms": ["all"],
        "required_features": [],
    }


def binding(
    *,
    role: str,
    kind: str,
    include: str,
    qualified_name: str,
    target: str,
    header_fingerprint: str,
) -> dict[str, Any]:
    return {
        "role": role,
        "kind": kind,
        "include": include,
        "qualified_name": qualified_name,
        "target": target,
        "availability": availability(),
        "_header_fingerprint": header_fingerprint,
    }


def capability(**row: Any) -> dict[str, Any]:
    row.setdefault("contract_version", {"major": 1, "minor": 0})
    row.setdefault("status", "usable")
    row.setdefault(
        "evolution", {"state": "active", "introduced_in": {"major": 1, "minor": 0}}
    )
    return row


# Every row is an explicit consumer promise about public API that already
# ships. Header fingerprints are maintenance data and are stripped from the
# installed manifest.
EXPORTS = [
    capability(
        key="signal.saturator",
        domain="signal",
        summary="Stateful saturation with an explicit anti-aliasing policy.",
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "audio",
            "release": "none",
        },
        state_model=(
            "Instance state is reset explicitly; prepare/configuration precedes "
            "processing."
        ),
        seed_model="none",
        input_domain="audio samples",
        output_domain="audio samples",
        units=["samples", "decibels", "hertz", "normalized ratio"],
        latency="implementation-reported",
        tail="none",
        scheduling="sample-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/signal/saturator.hpp",
                qualified_name="pulp::signal::SaturatorT<float>",
                target="Pulp::signal",
                header_fingerprint=(
                    "sha256:575576593e3edcd18104082feebfc48584a3f160cc5dcd24d8d48909cb8e6b2b"
                ),
            )
        ],
        forge_descriptor={"catalog": "forge-catalog.json", "node_key": "saturator"},
        _link_probe={
            "binding": "pulp::signal::SaturatorT<float>",
            "operation": "member_call",
            "member": "prepare",
            "arguments": "48000.0",
        },
    ),
    capability(
        key="audio.instrument-voice-allocator",
        domain="audio",
        summary=(
            "Prepared finite voice allocation with choke, steal, release, and "
            "termination records."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "control-or-audio-when-quiescent",
            "release": "destruction-off-audio",
        },
        state_model=(
            "prepare allocates a fixed voice table; trigger and release mutate "
            "prepared slots only."
        ),
        seed_model="none",
        input_domain="voice trigger and release events",
        output_domain="voice allocation and termination records",
        units=["MIDI note", "frames", "voice index"],
        latency="zero",
        tail="termination-fade-frames",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/audio/instrument_voice_allocator.hpp",
                qualified_name="pulp::audio::InstrumentVoiceAllocator",
                target="Pulp::audio",
                header_fingerprint=(
                    "sha256:f500f22c9e5ce4a1a26a03245b68e645ef6752dd2f4d0f59cf122681e0a2327f"
                ),
            )
        ],
        _link_probe={
            "binding": "pulp::audio::InstrumentVoiceAllocator",
            "operation": "member_call",
            "member": "prepare",
            "arguments": "1",
        },
    ),
    capability(
        key="midi.mpe-voice-tracker",
        domain="midi",
        summary=(
            "Fixed-capacity MIDI 1.0 and UMP MPE note ownership and expression "
            "tracking."
        ),
        rt_class="audio",
        lifecycle={
            "construction": "control",
            "prepare": "none",
            "process": "audio",
            "reset": "audio",
            "release": "none",
        },
        state_model=(
            "Fixed 128-slot note table with explicit reset and monotonic note "
            "identities."
        ),
        seed_model="none",
        input_domain="MIDI events and UMP packets",
        output_domain="owned per-note expression state",
        units=["MIDI note", "MIDI channel", "semitones", "normalized ratio"],
        latency="zero",
        tail="owned-notes-until-release-or-reset",
        scheduling="event-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/midi/mpe_voice_tracker.hpp",
                qualified_name="pulp::midi::MpeVoiceTracker",
                target="Pulp::midi",
                header_fingerprint=(
                    "sha256:5244ad204106b63719ccdd729260b5f0874715bc91c8ec863e05f85208554fd6"
                ),
            )
        ],
        _link_probe={
            "binding": "pulp::midi::MpeVoiceTracker",
            "operation": "member_call",
            "member": "reset",
            "arguments": "",
        },
    ),
    capability(
        key="timebase.tick",
        domain="timebase",
        summary=(
            "Saturating integer musical position on the 705600-tick "
            "quarter-note grid."
        ),
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "value-initialization",
            "release": "none",
        },
        state_model="Value type with saturating arithmetic over signed 64-bit ticks.",
        seed_model="none",
        input_domain="document ticks",
        output_domain="document ticks",
        units=["ticks"],
        latency="zero",
        tail="none",
        scheduling="pure",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/timebase/tick.hpp",
                qualified_name="pulp::timebase::TickPosition",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:e648c10386afc397349a341787aa4773326b551fb8f8f33e08bc9925aea42452"
                ),
            )
        ],
        _link_probe={
            "binding": "pulp::timebase::TickPosition",
            "operation": "construct",
            "arguments": "1",
        },
    ),
    capability(
        key="timebase.swing",
        domain="timebase",
        summary=(
            "Exact rational swing projection with bounded-rounding recovery over "
            "integer document ticks."
        ),
        rt_class="any",
        lifecycle={
            "construction": "any",
            "prepare": "none",
            "process": "any",
            "reset": "value-initialization",
            "release": "none",
        },
        state_model=(
            "Pure value and free-function transforms; unswing recovers within the "
            "documented rounding bound, and invalid inputs leave positions unchanged."
        ),
        seed_model="none",
        input_domain="document ticks and rational swing",
        output_domain="document ticks",
        units=["ticks", "rational ratio"],
        latency="zero",
        tail="none",
        scheduling="pure",
        bindings=[
            binding(
                role="configuration",
                kind="cpp_type",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::SwingRatio",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
            binding(
                role="forward-operation",
                kind="cpp_function",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::swing_position",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
            binding(
                role="inverse-operation",
                kind="cpp_function",
                include="pulp/timebase/quantize.hpp",
                qualified_name="pulp::timebase::unswing_position",
                target="Pulp::timebase",
                header_fingerprint=(
                    "sha256:ca242cb57d4963cf48ce6bb41a4b9d90d0959b7a6aff28cdbdb76013ba39ac84"
                ),
            ),
        ],
        _link_probe={
            "binding": "pulp::timebase::swing_position",
            "operation": "function_call",
            "arguments": (
                "pulp::timebase::TickPosition{1}, "
                "pulp::timebase::TickDuration{2}, pulp::timebase::kStraightSwing"
            ),
        },
    ),
    capability(
        key="sequence.host-transport-projector",
        domain="sequence",
        summary=(
            "Prepared projection from host callback transport into Pulp playback "
            "snapshots."
        ),
        rt_class="mixed",
        lifecycle={
            "construction": "control",
            "prepare": "control",
            "process": "audio",
            "reset": "control-or-audio-when-quiescent",
            "release": "none",
        },
        state_model=(
            "Prepared tempo-map reference plus bounded callback history and playback "
            "epoch."
        ),
        seed_model="none",
        input_domain="host process context",
        output_domain="playback transport snapshot",
        units=["samples", "ticks", "beats per minute"],
        latency="zero",
        tail="none",
        scheduling="block-synchronous",
        bindings=[
            binding(
                role="entrypoint",
                kind="cpp_type",
                include="pulp/sequence/host_transport_projector.hpp",
                qualified_name="pulp::sequence::HostTransportProjector",
                target="Pulp::sequence",
                header_fingerprint=(
                    "sha256:3c0a31d541635c9339fadb13b584e9bdec7a4e9e274afd595450f707c8344051"
                ),
            )
        ],
        _link_probe={
            "binding": "pulp::sequence::HostTransportProjector",
            "operation": "member_call",
            "member": "reset",
            "arguments": "",
        },
    ),
]

# Public headers can leave the frozen legacy bucket only through one of these
# explicit reviewed classifications or a capability binding above.
REVIEWED_HEADERS: list[dict[str, Any]] = []
SURFACE_TOMBSTONES: list[dict[str, Any]] = []
CAPABILITY_TOMBSTONES: list[dict[str, Any]] = []


def repo_root() -> pathlib.Path:
    path = pathlib.Path(__file__).resolve()
    for parent in path.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "core").is_dir():
            return parent
    raise SystemExit("agent-capabilities: could not locate the repository root")


def strip_private(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: strip_private(item)
            for key, item in value.items()
            if not key.startswith("_")
        }
    if isinstance(value, list):
        return [strip_private(item) for item in value]
    return copy.deepcopy(value)


def public_rows() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for source in EXPORTS:
        row = strip_private(source)
        row["contract_digest"] = surface.canonical_digest(contract_payload(row))
        rows.append(row)
    return rows


def binding_claims() -> list[dict[str, Any]]:
    claims: list[dict[str, Any]] = []
    for row in EXPORTS:
        for item in row["bindings"]:
            claims.append(
                {
                    "include": item["include"],
                    "fingerprint": item["_header_fingerprint"],
                    "capability_key": row["key"],
                }
            )
    return claims


def legacy_signal_projection(root: pathlib.Path) -> dict[str, Any]:
    path = root / "tools/dsp_vocabulary.py"
    spec = importlib.util.spec_from_file_location("pulp_dsp_vocabulary_source", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.scan_headers()


def build_surface(root: pathlib.Path) -> tuple[dict[str, Any], list[str]]:
    return surface.build_surface_document(
        root,
        binding_claims=binding_claims(),
        reviewed_headers=REVIEWED_HEADERS,
        tombstones=SURFACE_TOMBSTONES,
        baseline=surface.load_baseline(root),
        known_capability_keys={row["key"] for row in EXPORTS},
        inventory_version=SURFACE_INVENTORY_VERSION,
    )


def coverage_from_surface(
    surface_document: dict[str, Any], rows: list[dict[str, Any]]
) -> dict[str, Any]:
    domains: dict[str, dict[str, Any]] = {}
    for domain in sorted(DOMAINS):
        domains[domain] = {
            "state": "partial" if domain in ROOT_DOMAINS else "not_inventoried",
            "capabilities": sum(row["domain"] == domain for row in rows),
        }
    return {
        "state": "partial",
        "absence_semantics": "unknown",
        "domains": domains,
    }


def document(root: pathlib.Path | None = None) -> dict[str, Any]:
    root = root or repo_root()
    rows = sorted(public_rows(), key=lambda row: row["key"])
    surface_document, problems = build_surface(root)
    if problems:
        raise RuntimeError("; ".join(problems))
    counts = {
        domain: sum(row["domain"] == domain for row in rows)
        for domain in sorted(DOMAINS)
    }
    return {
        "$schema": "agent-capabilities.schema.json",
        "schema": SCHEMA,
        "schema_minor": SCHEMA_MINOR,
        "manifest_revision": MANIFEST_REVISION,
        "required_features": REQUIRED_FEATURES,
        "coverage": coverage_from_surface(surface_document, rows),
        "capabilities": rows,
        "tombstones": sorted(CAPABILITY_TOMBSTONES, key=lambda row: row["key"]),
        "counts": {"total": len(rows), "by_domain": counts},
        "compatibility": {
            "signal_vocabulary": {
                "schema": "pulp.signal-vocabulary.compat.v1",
                "source": (
                    "curated manifest regeneration scan of public signal headers"
                ),
                "entries": legacy_signal_projection(root),
            }
        },
    }


def compile_fixture() -> str:
    public = public_rows()
    includes = sorted(
        {item["include"] for row in public for item in row["bindings"]}
    )
    lines = [
        "// Generated by tools/scripts/agent_capability_manifest.py --write.",
        "// Every typed binding is mechanically referenced below.",
        "",
    ]
    lines.extend(f"#include <{header}>" for header in includes)
    lines.extend(["", "int main() {"])
    binding_index = 0
    source_by_key = {row["key"]: row for row in EXPORTS}
    for row in sorted(public, key=lambda item: item["key"]):
        lines.extend(["    {", f"        // {row['key']}"])
        for item in row["bindings"]:
            name = item["qualified_name"]
            if item["kind"] == "cpp_type":
                lines.append(f"        static_assert(sizeof({name}) > 0);")
            else:
                lines.append(
                    f"        auto *volatile binding_{binding_index} = &{name};"
                )
                lines.append(f"        (void)binding_{binding_index};")
            binding_index += 1
        lines.append(f"        {render_link_probe(source_by_key[row['key']])}")
        lines.append("    }")
    lines.extend(["    return 0;", "}", ""])
    return "\n".join(lines)


def render_link_probe(row: dict[str, Any]) -> str:
    probe = row["_link_probe"]
    binding = probe["binding"]
    arguments = probe["arguments"]
    operation = probe["operation"]
    if operation == "construct":
        return f"{binding} probe_value{{{arguments}}}; (void)probe_value;"
    if operation == "member_call":
        return (
            f"{binding} probe_value{{}}; "
            f"(void)probe_value.{probe['member']}({arguments});"
        )
    if operation == "function_call":
        return f"(void){binding}({arguments});"
    raise ValueError(f"unsupported link probe operation: {operation!r}")


def _link_probe_problems(row: dict[str, Any]) -> list[str]:
    key = row.get("key", "<unknown>")
    probe = row.get("_link_probe")
    if not isinstance(probe, dict):
        return [f"{key} requires a structured installed consumer link probe"]
    operation = probe.get("operation")
    required = {"binding", "operation", "arguments"}
    if operation == "member_call":
        required.add("member")
    if set(probe) != required:
        return [f"{key} link probe fields are not exact for {operation!r}"]
    bindings = {
        item["qualified_name"]: item["kind"]
        for item in row.get("bindings", [])
        if isinstance(item, dict)
        and isinstance(item.get("qualified_name"), str)
    }
    binding = probe.get("binding")
    if binding not in bindings:
        return [f"{key} link probe must name an advertised binding"]
    if not isinstance(probe.get("arguments"), str):
        return [f"{key} link probe arguments must be a C++ argument string"]
    if operation in {"construct", "member_call"} and bindings[binding] != "cpp_type":
        return [f"{key} {operation} probe requires a cpp_type binding"]
    if operation == "function_call" and bindings[binding] != "cpp_function":
        return [f"{key} function_call probe requires a cpp_function binding"]
    if operation == "member_call" and not re.fullmatch(
        r"[A-Za-z_][A-Za-z0-9_]*", probe.get("member", "")
    ):
        return [f"{key} member_call probe has an invalid member"]
    if operation not in {"construct", "member_call", "function_call"}:
        return [f"{key} link probe operation is invalid"]
    try:
        render_link_probe(row)
    except (KeyError, TypeError, ValueError) as error:
        return [f"{key} link probe could not render: {error}"]
    return []


def history_entry(
    manifest_document: dict[str, Any], surface_document: dict[str, Any]
) -> dict[str, Any]:
    material = {
        "manifest": {
            "schema": manifest_document["schema"],
            "manifest_revision": manifest_document["manifest_revision"],
            "capabilities": copy.deepcopy(manifest_document["capabilities"]),
            "tombstones": copy.deepcopy(manifest_document["tombstones"]),
        },
        "surface": {
            "schema": surface_document["schema"],
            "inventory_version": surface_document["inventory_version"],
            "headers": [
                {
                    "include": row["include"],
                    "fingerprint": row["fingerprint"],
                    "disposition": row["disposition"],
                }
                for row in surface_document["headers"]
            ],
            "tombstones": copy.deepcopy(surface_document["tombstones"]),
        },
    }
    return {
        **material,
        "entry_digest": surface.canonical_digest(material),
    }


def history_document(entries: list[dict[str, Any]]) -> dict[str, Any]:
    return {"schema": HISTORY_SCHEMA, "entries": copy.deepcopy(entries)}


def history_problems(
    history: Any,
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
) -> list[str]:
    if not isinstance(history, dict):
        return ["capability history must be an object"]
    if set(history) != {"schema", "entries"}:
        return ["capability history fields must be exactly schema and entries"]
    if history.get("schema") != HISTORY_SCHEMA:
        return [f"capability history schema must be {HISTORY_SCHEMA}"]
    entries = history.get("entries")
    if not isinstance(entries, list) or not entries:
        return ["capability history must contain at least one entry"]
    problems: list[str] = []
    previous: dict[str, Any] | None = None
    for index, entry in enumerate(entries):
        where = f"capability history entries[{index}]"
        if not isinstance(entry, dict) or set(entry) != {
            "entry_digest",
            "manifest",
            "surface",
        }:
            problems.append(f"{where} fields are not exact")
            continue
        material = {"manifest": entry["manifest"], "surface": entry["surface"]}
        if entry.get("entry_digest") != surface.canonical_digest(material):
            problems.append(f"{where} digest does not match its material")
        if previous is not None:
            problems.extend(
                evolution_problems(
                    previous["manifest"],
                    entry["manifest"],
                    allow_unpublished_migration=False,
                )
            )
            problems.extend(
                surface_evolution_problems(
                    previous["surface"], entry["surface"], surface.SURFACE_SCHEMA
                )
            )
        previous = entry
    current_entry = history_entry(current_manifest, current_surface)
    if previous is not None:
        problems.extend(
            evolution_problems(
                previous["manifest"],
                current_entry["manifest"],
                allow_unpublished_migration=False,
            )
        )
        problems.extend(
            surface_evolution_problems(
                previous["surface"], current_entry["surface"], surface.SURFACE_SCHEMA
            )
        )
    return _deduplicate(problems)


def protected_base_problems(
    root: pathlib.Path,
    history: dict[str, Any],
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
) -> list[str]:
    """Compare against protected-tip artifacts, which this checkout cannot edit."""
    base_ref = os.environ.get("PULP_AGENT_CAPABILITY_BASE_REF", "origin/main")
    if _git_output(root, ["rev-parse", "--is-inside-work-tree"]) != "true":
        # Source archives have no independently addressable protected history.
        # Their self-contained history is still checked above; PR/CI checkouts
        # must resolve or fetch the immutable protected tip below.
        return []
    protected_tip = _resolve_protected_tip(root, base_ref)
    if protected_tip is None:
        return [
            f"could not resolve protected capability history base {base_ref!r}; "
            "set PULP_AGENT_CAPABILITY_BASE_REF to the CI base ref"
        ]
    tip_manifest = _git_json(root, protected_tip, SNAPSHOT)
    old_manifest = tip_manifest
    old_surface = _git_json(root, protected_tip, surface.SURFACE_SNAPSHOT)
    old_history = _git_json(root, protected_tip, HISTORY_FILE)
    if old_manifest is None and old_surface is None and old_history is None:
        if len(history.get("entries", [])) != 1:
            return ["initial capability history bootstrap must contain exactly one entry"]
        return []
    problems: list[str] = []
    if old_manifest is None or old_surface is None or old_history is None:
        return ["protected base has an incomplete capability history contract"]
    problems.extend(append_only_history_problems(old_history, history))
    problems.extend(
        evolution_problems(
            old_manifest,
            current_manifest,
            allow_unpublished_migration=False,
        )
    )
    problems.extend(
        surface_evolution_problems(
            old_surface, current_surface, surface.SURFACE_SCHEMA
        )
    )
    return _deduplicate(problems)


def append_only_history_problems(previous: Any, current: Any) -> list[str]:
    old_entries = previous.get("entries") if isinstance(previous, dict) else None
    new_entries = current.get("entries") if isinstance(current, dict) else None
    if not isinstance(old_entries, list) or not isinstance(new_entries, list):
        return ["protected capability history entries are invalid"]
    if new_entries[: len(old_entries)] != old_entries:
        return ["capability history is not append-only relative to the protected base"]
    return []


def validate(doc: Any, root: pathlib.Path) -> list[str]:
    schema_path = root / MANIFEST_SCHEMA_FILE
    if not schema_path.is_file():
        return [f"manifest schema is missing: {schema_path}"]
    schema_document = json.loads(schema_path.read_text())
    problems = json_schema_lite.validate(doc, schema_document)
    if not isinstance(doc, dict):
        return problems
    if doc.get("schema") != SCHEMA:
        problems.append(f"schema must be exactly {SCHEMA}")
    if doc.get("schema_minor") != SCHEMA_MINOR:
        problems.append(f"schema_minor must be exactly {SCHEMA_MINOR}")
    revision = doc.get("manifest_revision")
    if isinstance(revision, bool) or not isinstance(revision, int) or revision < 1:
        problems.append("manifest_revision must be a positive integer")
    if doc.get("required_features") != REQUIRED_FEATURES:
        problems.append("required_features must be the sorted supported feature set")

    rows = doc.get("capabilities")
    if not isinstance(rows, list):
        return problems
    expected_rows = {row["key"]: row for row in public_rows()}
    for source_row in EXPORTS:
        problems.extend(_link_probe_problems(source_row))
    expected_bindings = {
        key: {_binding_contract(item) for item in row["bindings"]}
        for key, row in expected_rows.items()
    }
    forge_path = root / "docs/status/forge-catalog.json"
    forge_keys: set[str] = set()
    if forge_path.exists():
        forge_keys = {
            row.get("key")
            for row in json.loads(forge_path.read_text()).get("nodes", [])
        }

    seen: set[str] = set()
    for index, row in enumerate(rows):
        where = f"capabilities[{index}]"
        if not isinstance(row, dict):
            continue
        key = row.get("key")
        if isinstance(key, str):
            if key in seen:
                problems.append(f"duplicate capability key: {key}")
            seen.add(key)
        if row.get("domain") not in DOMAINS:
            problems.append(f"{where} has invalid domain {row.get('domain')!r}")
        if row.get("rt_class") not in RT_CLASSES:
            problems.append(f"{where} has invalid rt_class {row.get('rt_class')!r}")
        if row.get("status") not in STATUSES:
            problems.append(f"{where} has invalid status {row.get('status')!r}")
        if row.get("status") == "planned":
            problems.append(f"{where} may not advertise planned work")
        version = row.get("contract_version")
        if not _valid_version(version, minimum_major=1):
            problems.append(
                f"{where}.contract_version must have major >= 1 and minor >= 0"
            )
        expected_digest = surface.canonical_digest(contract_payload(row))
        if row.get("contract_digest") != expected_digest:
            problems.append(f"{where}.contract_digest does not match its contract")

        evolution = row.get("evolution")
        if isinstance(evolution, dict):
            state = evolution.get("state")
            if state not in EVOLUTION_STATES:
                problems.append(f"{where}.evolution.state is invalid")
            introduced = evolution.get("introduced_in")
            if not _valid_version(introduced):
                problems.append(f"{where}.evolution.introduced_in is invalid")
            elif _valid_version(version, minimum_major=1) and _version_tuple(
                introduced
            ) > _version_tuple(version):
                problems.append(
                    f"{where}.evolution.introduced_in exceeds contract_version"
                )
            if state == "active":
                if set(evolution) != {"state", "introduced_in"}:
                    problems.append(f"{where}.evolution active fields are not exact")
                if row.get("status") == "deprecated":
                    problems.append(f"{where} active capability may not be deprecated")
            if state == "deprecated":
                if set(evolution) != {
                    "state",
                    "introduced_in",
                    "deprecated_in",
                    "replacement_key",
                }:
                    problems.append(f"{where}.evolution deprecated fields are not exact")
                deprecated = evolution.get("deprecated_in")
                if not _valid_version(deprecated):
                    problems.append(f"{where}.evolution.deprecated_in is invalid")
                elif _valid_version(introduced) and _valid_version(
                    version, minimum_major=1
                ) and not (
                    _version_tuple(introduced)
                    <= _version_tuple(deprecated)
                    <= _version_tuple(version)
                ):
                    problems.append(
                        f"{where}.evolution versions must satisfy introduced <= "
                        "deprecated <= contract"
                    )
                if row.get("status") != "deprecated":
                    problems.append(
                        f"{where} deprecated evolution requires deprecated status"
                    )
                replacement = evolution.get("replacement_key")
                if replacement is not None and not isinstance(replacement, str):
                    problems.append(f"{where}.evolution.replacement_key is invalid")

        binding_identities: set[tuple[Any, ...]] = set()
        binding_contracts: set[tuple[Any, ...]] = set()
        for binding_index, item in enumerate(row.get("bindings", [])):
            if not isinstance(item, dict):
                continue
            binding_where = f"{where}.bindings[{binding_index}]"
            identity = tuple(
                _identity_value(item.get(field))
                for field in (
                    "role",
                    "kind",
                    "include",
                    "qualified_name",
                    "target",
                )
            )
            if identity in binding_identities:
                problems.append(f"{binding_where} duplicates another typed binding")
            binding_identities.add(identity)
            contract = _binding_contract(item)
            binding_contracts.add(contract)
            if (
                not isinstance(key, str)
                or key not in expected_bindings
                or contract not in expected_bindings[key]
            ):
                problems.append(
                    f"{binding_where} advertises a binding outside curated exports"
                )
            include = item.get("include")
            if isinstance(include, str) and not list(
                root.glob(f"core/*/include/{include}")
            ):
                problems.append(f"{binding_where} advertises missing include {include}")
            if isinstance(include, str):
                expected_target = _minimal_target_for_include(include)
                if expected_target is None:
                    problems.append(
                        f"{binding_where} include has no covered public target owner"
                    )
                elif item.get("target") != expected_target:
                    problems.append(
                        f"{binding_where}.target must be minimal owning target "
                        f"{expected_target}"
                    )
            if item.get("kind") not in BINDING_KINDS:
                problems.append(f"{binding_where}.kind is invalid")
        if (
            isinstance(key, str)
            and key in expected_bindings
            and binding_contracts != expected_bindings[key]
        ):
            problems.append(f"{where}.bindings must exactly match curated exports")

        descriptor = row.get("forge_descriptor")
        if descriptor is not None:
            if (
                not isinstance(descriptor, dict)
                or set(descriptor) != {"catalog", "node_key"}
                or descriptor.get("catalog") != "forge-catalog.json"
                or not isinstance(descriptor.get("node_key"), str)
                or not descriptor.get("node_key")
            ):
                problems.append(f"{where} has invalid forge_descriptor reference")
            elif descriptor.get("node_key") not in forge_keys:
                problems.append(
                    f"{where} references missing Forge descriptor "
                    f"{descriptor.get('node_key')!r}"
                )

        for path, field in _object_fields(row):
            if field in FORBIDDEN_NUMERIC_CONTRACT_FIELDS:
                problems.append(
                    f"{where}{path} duplicates Forge numeric contract field {field!r}"
                )
            if field in FORBIDDEN_RUNTIME_CONTROL_FIELDS:
                problems.append(
                    f"{where}{path} contains runtime control field {field!r}; "
                    "design capabilities and runtime control are separate contracts"
                )

    if [row.get("key") for row in rows if isinstance(row, dict)] != sorted(seen):
        problems.append("capabilities must be sorted by stable key")
    if seen != set(expected_rows):
        problems.append("capability keys must exactly match curated exports")

    replacement_edges: dict[str, str] = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        replacement = row.get("evolution", {}).get("replacement_key")
        key = row.get("key")
        if not isinstance(replacement, str) or not isinstance(key, str):
            continue
        if replacement == key:
            problems.append(f"{key} replacement_key may not reference itself")
        elif replacement not in seen:
            problems.append(f"{key} replacement_key does not name a live capability")
        else:
            replacement_edges[key] = replacement

    tombstones = doc.get("tombstones")
    tombstone_keys: set[str] = set()
    if isinstance(tombstones, list):
        for index, item in enumerate(tombstones):
            if not isinstance(item, dict):
                continue
            key = item.get("key")
            if isinstance(key, str):
                if key in seen:
                    problems.append(
                        f"capability tombstone overlaps a live key: {key}"
                    )
                if key in tombstone_keys:
                    problems.append(f"duplicate capability tombstone: {key}")
                tombstone_keys.add(key)
            if not _valid_digest(item.get("last_contract_digest")):
                problems.append(
                    f"tombstones[{index}].last_contract_digest is invalid"
                )
            removed_revision = item.get("removed_in_manifest_revision")
            if (
                isinstance(removed_revision, bool)
                or not isinstance(removed_revision, int)
                or removed_revision < 1
                or (isinstance(revision, int) and removed_revision > revision)
            ):
                problems.append(
                    f"tombstones[{index}].removed_in_manifest_revision is invalid"
                )
            if item.get("status") != "removed":
                problems.append(f"tombstones[{index}].status must be removed")
            introduced = item.get("introduced_in")
            deprecated = item.get("deprecated_in")
            last_version = item.get("last_contract_version")
            if not all(
                _valid_version(value)
                for value in (introduced, deprecated, last_version)
            ):
                problems.append(f"tombstones[{index}] lifecycle versions are invalid")
            elif not (
                _version_tuple(introduced)
                <= _version_tuple(deprecated)
                <= _version_tuple(last_version)
            ):
                problems.append(
                    f"tombstones[{index}] lifecycle versions must satisfy "
                    "introduced <= deprecated <= last_contract_version"
                )
            replacement = item.get("replacement_key")
            if replacement is not None:
                if replacement == key:
                    problems.append(f"{key} replacement_key may not reference itself")
                elif replacement not in seen:
                    problems.append(
                        f"{key} replacement_key does not name a live capability"
                    )
                elif isinstance(key, str):
                    replacement_edges[key] = replacement

    problems.extend(_replacement_cycle_problems(replacement_edges))

    actual_by_domain = {
        domain: sum(
            isinstance(row, dict) and row.get("domain") == domain for row in rows
        )
        for domain in sorted(DOMAINS)
    }
    counts = doc.get("counts")
    if not isinstance(counts, dict) or counts != {
        "total": len(rows),
        "by_domain": actual_by_domain,
    }:
        problems.append("counts must exactly match capabilities by domain")

    try:
        current_surface, surface_problems = build_surface(root)
        problems.extend(surface_problems)
        surface_schema_path = root / surface.SURFACE_SCHEMA_FILE
        if not surface_schema_path.is_file():
            problems.append(f"surface schema is missing: {surface_schema_path}")
        else:
            problems.extend(
                json_schema_lite.validate(
                    current_surface,
                    json.loads(surface_schema_path.read_text()),
                    "surface",
                )
            )
        expected_coverage = coverage_from_surface(current_surface, rows)
        if doc.get("coverage") != expected_coverage:
            problems.append("coverage must exactly match the reviewed public surface ledger")
    except (RuntimeError, json.JSONDecodeError) as error:
        problems.append(f"could not validate public surface coverage: {error}")

    compatibility = doc.get("compatibility")
    projection = (
        compatibility.get("signal_vocabulary")
        if isinstance(compatibility, dict)
        else None
    )
    if isinstance(projection, dict):
        if projection.get("entries") != legacy_signal_projection(root):
            problems.append(
                "compatibility signal vocabulary is stale against public signal headers"
            )
    return _deduplicate(problems)


def rendered(root: pathlib.Path | None = None) -> str:
    return json.dumps(document(root), indent=2, ensure_ascii=False) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Pulp installed agent capabilities")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--json", action="store_true", help="emit the consumer manifest")
    mode.add_argument("--check", action="store_true", help="validate all generated artifacts")
    mode.add_argument("--write", action="store_true", help="regenerate checked artifacts")
    mode.add_argument("--validate", metavar="PATH", help="validate a manifest fixture")
    mode.add_argument(
        "--bootstrap-surface",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--snapshot", type=pathlib.Path, help=argparse.SUPPRESS)
    parser.add_argument(
        "--migrate-unpublished-v1", action="store_true", help=argparse.SUPPRESS
    )
    args = parser.parse_args()
    root = repo_root()

    if args.bootstrap_surface:
        path = root / surface.LEGACY_BASELINE
        if path.exists():
            print(
                f"agent-capabilities: INVALID: frozen baseline already exists: {path}",
                file=sys.stderr,
            )
            return 1
        baseline = surface.baseline_document(
            root, {claim["include"] for claim in binding_claims()}
        )
        problems = surface.validate_baseline(baseline)
        if problems:
            return _print_problems(problems)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(surface.rendered(baseline))
        print(
            f"agent-capabilities: bootstrapped frozen baseline with "
            f"{baseline['frozen_count']} headers"
        )
        return 0

    if args.migrate_unpublished_v1 and not args.write:
        return _print_problems(["--migrate-unpublished-v1 requires --write"])

    if args.validate:
        try:
            fixture = json.loads(pathlib.Path(args.validate).read_text())
        except (OSError, json.JSONDecodeError) as error:
            return _print_problems([f"could not read fixture: {error}"])
        return _print_problems(validate(fixture, root), success_message=None)

    try:
        doc = document(root)
        surface_document, surface_problems = build_surface(root)
    except (RuntimeError, json.JSONDecodeError) as error:
        return _print_problems([str(error)])
    problems = validate(doc, root) + surface_problems
    if problems:
        return _print_problems(problems)

    output = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
    surface_output = surface.rendered(surface_document)
    snapshot = args.snapshot or root / SNAPSHOT
    surface_snapshot = root / surface.SURFACE_SNAPSHOT
    history_path = root / HISTORY_FILE
    fixture = root / COMPILE_FIXTURE

    if args.json:
        sys.stdout.write(output)
        return 0
    if args.write:
        previous = _load_optional_json(snapshot)
        previous_surface = _load_optional_json(surface_snapshot)
        base_ref = os.environ.get("PULP_AGENT_CAPABILITY_BASE_REF", "origin/main")
        protected_tip = _resolve_protected_tip(root, base_ref)
        initial_bootstrap = bool(
            protected_tip and _git_json(root, protected_tip, SNAPSHOT) is None
        )
        problems: list[str] = []
        if not (args.migrate_unpublished_v1 and initial_bootstrap):
            problems.extend(
                evolution_problems(
                    previous,
                    doc,
                    allow_unpublished_migration=args.migrate_unpublished_v1,
                )
            )
            problems.extend(
                surface_evolution_problems(
                    previous_surface, surface_document, surface.SURFACE_SCHEMA
                )
            )
        if problems:
            return _print_problems(problems)
        history = _load_optional_json(history_path)
        entries = [] if args.migrate_unpublished_v1 else (
            copy.deepcopy(history.get("entries", []))
            if isinstance(history, dict) and history.get("schema") == HISTORY_SCHEMA
            else []
        )
        if (
            not args.migrate_unpublished_v1
            and isinstance(previous, dict)
            and isinstance(previous_surface, dict)
        ):
            previous_entry = history_entry(previous, previous_surface)
            if not entries or entries[-1] != previous_entry:
                entries.append(previous_entry)
        if not entries:
            entries.append(history_entry(doc, surface_document))
        history = history_document(entries)
        problems = history_problems(history, doc, surface_document)
        problems.extend(
            protected_base_problems(
                root, history, doc, surface_document
            )
        )
        if problems:
            return _print_problems(problems)
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(output)
        surface_snapshot.parent.mkdir(parents=True, exist_ok=True)
        surface_snapshot.write_text(surface_output)
        history_path.parent.mkdir(parents=True, exist_ok=True)
        history_path.write_text(json.dumps(history, indent=2, ensure_ascii=False) + "\n")
        fixture.write_text(compile_fixture())
        print(
            f"agent-capabilities: wrote {SNAPSHOT}, {surface.SURFACE_SNAPSHOT}, "
            f"{HISTORY_FILE}, and {COMPILE_FIXTURE} "
            f"({len(doc['capabilities'])} capabilities)"
        )
        return 0
    if args.check:
        stale: list[str] = []
        if not snapshot.exists() or snapshot.read_text() != output:
            stale.append(f"{snapshot} differs from curated exports")
        if not surface_snapshot.exists() or surface_snapshot.read_text() != surface_output:
            stale.append(f"{surface.SURFACE_SNAPSHOT} differs from public headers")
        if not fixture.exists() or fixture.read_text() != compile_fixture():
            stale.append(f"{COMPILE_FIXTURE} differs from typed bindings")
        history = _load_optional_json(history_path)
        history_issues = history_problems(history, doc, surface_document)
        if isinstance(history, dict):
            history_issues.extend(
                protected_base_problems(root, history, doc, surface_document)
            )
        stale.extend(history_issues)
        if stale:
            for item in stale:
                print(f"agent-capabilities: STALE: {item}", file=sys.stderr)
            return 1
        print(
            f"agent-capabilities: fresh; {len(doc['capabilities'])} keys and "
            f"{surface_document['counts']['public_headers']} public headers checked"
        )
        return 0

    print("Pulp agent capability manifest")
    print(f"  schema        {SCHEMA} minor {SCHEMA_MINOR}")
    print(f"  revision      {MANIFEST_REVISION}")
    print(f"  capabilities  {len(doc['capabilities'])}")
    print(f"  coverage      {doc['coverage']['state']} (absence means unknown)")
    for domain, count in doc["counts"]["by_domain"].items():
        if count:
            print(f"  {domain:<12} {count}")
    return 0


def _object_fields(value: Any, path: str = "") -> list[tuple[str, str]]:
    fields: list[tuple[str, str]] = []
    if isinstance(value, dict):
        for key, item in value.items():
            child = f"{path}.{key}"
            fields.append((child, key))
            fields.extend(_object_fields(item, child))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            fields.extend(_object_fields(item, f"{path}[{index}]"))
    return fields


def _valid_digest(value: Any) -> bool:
    return bool(
        isinstance(value, str)
        and re.fullmatch(r"sha256:[0-9a-f]{64}", value)
    )


def _valid_version(value: Any, *, minimum_major: int = 0) -> bool:
    return bool(
        isinstance(value, dict)
        and set(value) == {"major", "minor"}
        and isinstance(value.get("major"), int)
        and not isinstance(value.get("major"), bool)
        and value["major"] >= minimum_major
        and isinstance(value.get("minor"), int)
        and not isinstance(value.get("minor"), bool)
        and value["minor"] >= 0
    )


def _minimal_target_for_include(include: str) -> str | None:
    for public_root in surface.PUBLIC_ROOTS:
        prefix = public_root["install_prefix"] + "/"
        if include.startswith(prefix):
            return f"Pulp::{public_root['domain']}"
    return None


def _version_tuple(value: dict[str, Any]) -> tuple[int, int]:
    return value["major"], value["minor"]


def _replacement_cycle_problems(edges: dict[str, str]) -> list[str]:
    problems: list[str] = []
    for start in sorted(edges):
        path: list[str] = []
        seen: set[str] = set()
        current = start
        while current in edges:
            if current in seen:
                cycle = path[path.index(current) :] + [current]
                problems.append("replacement_key cycle: " + " -> ".join(cycle))
                break
            seen.add(current)
            path.append(current)
            current = edges[current]
    return _deduplicate(problems)


def _binding_contract(item: dict[str, Any]) -> tuple[Any, ...]:
    return (
        _identity_value(item.get("role")),
        _identity_value(item.get("kind")),
        _identity_value(item.get("include")),
        _identity_value(item.get("qualified_name")),
        _identity_value(item.get("target")),
        _identity_value(item.get("availability")),
    )


def _identity_value(value: Any) -> str:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    )


def _deduplicate(problems: list[str]) -> list[str]:
    return list(dict.fromkeys(problems))


def _load_optional_json(path: pathlib.Path) -> Any:
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as error:
        return {"_invalid": str(error)}


def _git_output(root: pathlib.Path, arguments: list[str]) -> str | None:
    try:
        result = subprocess.run(
            ["git", *arguments], cwd=root, text=True, capture_output=True
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _git_json(root: pathlib.Path, revision: str, path: pathlib.Path) -> Any:
    output = _git_output(root, ["show", f"{revision}:{path.as_posix()}"])
    if output is None:
        return None
    try:
        return json.loads(output)
    except json.JSONDecodeError:
        return {"_invalid": True}


def _resolve_protected_tip(root: pathlib.Path, base_ref: str) -> str | None:
    explicit_ref = "PULP_AGENT_CAPABILITY_BASE_REF" in os.environ
    if explicit_ref:
        tip = _git_output(
            root, ["rev-parse", "--verify", f"{base_ref}^{{commit}}"]
        )
        if tip is not None:
            return tip
    candidate: str | None = None
    event_path = (
        os.environ.get("GITHUB_EVENT_PATH")
        if os.environ.get("GITHUB_ACTIONS") == "true"
        else None
    )
    if event_path is not None:
        try:
            event = json.loads(pathlib.Path(event_path).read_text())
            candidate = event.get("pull_request", {}).get("base", {}).get("sha")
            candidate = candidate or event.get("merge_group", {}).get("base_sha")
            before = event.get("before")
            if (
                candidate is None
                and isinstance(before, str)
                and before != "0" * 40
            ):
                candidate = before
        except (OSError, json.JSONDecodeError, AttributeError):
            candidate = None
    if isinstance(candidate, str) and re.fullmatch(r"[0-9a-fA-F]{40}", candidate):
        if _git_output(root, ["cat-file", "-e", f"{candidate}^{{commit}}"]) is None:
            try:
                fetched = subprocess.run(
                    ["git", "fetch", "--no-tags", "--depth=1", "origin", candidate],
                    cwd=root,
                    text=True,
                    capture_output=True,
                )
            except OSError:
                return None
            if fetched.returncode != 0:
                return None
        return candidate.lower()
    return _git_output(root, ["rev-parse", "--verify", f"{base_ref}^{{commit}}"])


def _print_problems(
    problems: list[str], success_message: str | None = "agent-capabilities: valid"
) -> int:
    for problem in _deduplicate(problems):
        print(f"agent-capabilities: INVALID: {problem}", file=sys.stderr)
    if problems:
        return 1
    if success_message:
        print(success_message)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
