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
import pathlib
import re
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
SNAPSHOT = pathlib.Path("docs/status/agent-capabilities.json")
MANIFEST_SCHEMA_FILE = pathlib.Path(
    "docs/status/agent-capabilities.schema.json"
)
COMPILE_FIXTURE = pathlib.Path("test/test_agent_capability_compile.cpp")
DOMAINS = {"signal", "music", "midi", "audio", "timebase", "sequence", "offline"}
ROOT_DOMAINS = {item["domain"] for item in surface.PUBLIC_ROOTS}
RT_CLASSES = {"audio", "control", "any", "offline", "mixed"}
STATUSES = {"stable", "usable", "experimental", "partial", "unsupported"}
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
    row.setdefault("evolution", {"state": "active"})
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
        _link_probe=(
            "pulp::audio::InstrumentVoiceAllocator allocator; "
            "(void)allocator.prepare(1);"
        ),
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
        _link_probe="pulp::midi::MpeVoiceTracker tracker; tracker.reset();",
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
        _link_probe=(
            "pulp::sequence::HostTransportProjector projector; projector.reset();"
        ),
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
    surface_domains = surface_document["counts"]["by_domain"]
    domains: dict[str, dict[str, Any]] = {}
    for domain in sorted(DOMAINS):
        counts = surface_domains.get(
            domain,
            {
                "public_headers": 0,
                "reviewed_headers": 0,
                "legacy_unreviewed_headers": 0,
                "unsupported_headers": 0,
            },
        )
        domains[domain] = {
            "state": "partial" if domain in ROOT_DOMAINS else "not_inventoried",
            **counts,
            "capabilities": sum(row["domain"] == domain for row in rows),
        }
    return {
        "state": "partial",
        "absence_semantics": "unknown",
        "surface_schema": surface.SURFACE_SCHEMA,
        "surface_inventory_version": surface_document["inventory_version"],
        "public_headers": surface_document["counts"]["public_headers"],
        "reviewed_headers": surface_document["counts"]["reviewed_headers"],
        "legacy_unreviewed_headers": surface_document["counts"][
            "legacy_unreviewed_headers"
        ],
        "unsupported_headers": surface_document["counts"]["unsupported_headers"],
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
                lines.append(f"        auto *binding_{binding_index} = &{name};")
                lines.append(f"        (void)binding_{binding_index};")
            binding_index += 1
        link_probe = source_by_key[row["key"]].get("_link_probe")
        if link_probe:
            lines.append(f"        {link_probe}")
        lines.append("    }")
    lines.extend(["    return 0;", "}", ""])
    return "\n".join(lines)


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
        if isinstance(version, dict):
            major, minor = version.get("major"), version.get("minor")
            if (
                isinstance(major, bool)
                or not isinstance(major, int)
                or major < 1
                or isinstance(minor, bool)
                or not isinstance(minor, int)
                or minor < 0
            ):
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
            if state == "active" and set(evolution) != {"state"}:
                problems.append(f"{where}.evolution active state has extra fields")
            if state == "deprecated":
                if not isinstance(evolution.get("deprecated_in"), dict):
                    problems.append(f"{where}.evolution.deprecated_in is required")
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
    fixture = root / COMPILE_FIXTURE

    if args.json:
        sys.stdout.write(output)
        return 0
    if args.write:
        previous = _load_optional_json(snapshot)
        previous_surface = _load_optional_json(surface_snapshot)
        problems = evolution_problems(
            previous,
            doc,
            allow_unpublished_migration=args.migrate_unpublished_v1,
        )
        problems.extend(
            surface_evolution_problems(
                previous_surface, surface_document, surface.SURFACE_SCHEMA
            )
        )
        if problems:
            return _print_problems(problems)
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(output)
        surface_snapshot.parent.mkdir(parents=True, exist_ok=True)
        surface_snapshot.write_text(surface_output)
        fixture.write_text(compile_fixture())
        print(
            f"agent-capabilities: wrote {SNAPSHOT}, {surface.SURFACE_SNAPSHOT}, "
            f"and {COMPILE_FIXTURE} ({len(doc['capabilities'])} capabilities)"
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
