#!/usr/bin/env python3
"""Build and validate Pulp's installed agent capability manifest.

The curated rows describe only facts owned by the public Pulp API. Numeric
node contracts remain in forge-catalog.json; graph-capable rows reference a
semantic node key instead of copying its parameters.
"""
from __future__ import annotations

import argparse
import copy
import json
import pathlib
import re
import sys
from typing import Any


SCHEMA = "pulp.agent-capabilities.v1"
SNAPSHOT = pathlib.Path("docs/status/agent-capabilities.json")
COMPILE_FIXTURE = pathlib.Path("test/test_agent_capability_compile.cpp")
DOMAINS = {"signal", "music", "midi", "audio", "timebase", "sequence", "offline"}
RT_CLASSES = {"audio", "control", "any", "offline", "mixed"}
REQUIRED = {
    "key", "domain", "include", "symbol", "summary", "rt_class", "lifecycle",
    "state_model", "seed_model", "input_domain", "output_domain", "units",
    "latency", "tail", "scheduling",
}
FORBIDDEN_NUMERIC_CONTRACT_FIELDS = {"min", "max", "default", "range", "choices"}


def capability(**row: Any) -> dict[str, Any]:
    return row


# This is intentionally small and explicit. Adding a row is a reviewable claim
# about an API that already ships, not an inference from a plausible header.
EXPORTS = [
    capability(
        key="signal.saturator", domain="signal",
        include="pulp/signal/saturator.hpp", symbol="pulp::signal::SaturatorT<float>",
        summary="Stateful saturation with an explicit anti-aliasing policy.", rt_class="audio",
        lifecycle={"construction": "control", "prepare": "control", "process": "audio", "reset": "audio", "release": "none"},
        state_model="Instance state is reset explicitly; prepare/configuration precedes processing.",
        seed_model="none", input_domain="audio samples", output_domain="audio samples",
        units=["samples", "decibels", "hertz", "normalized ratio"], latency="implementation-reported",
        tail="none", scheduling="sample-synchronous",
        forge_descriptor={"catalog": "forge-catalog.json", "node_key": "saturator"},
        _compile="pulp::signal::SaturatorT<float> value; (void)value;",
    ),
    capability(
        key="audio.instrument-voice-allocator", domain="audio",
        include="pulp/audio/instrument_voice_allocator.hpp", symbol="pulp::audio::InstrumentVoiceAllocator",
        summary="Prepared finite voice allocation with choke, steal, release, and termination records.", rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control", "process": "audio", "reset": "control-or-audio-when-quiescent", "release": "destruction-off-audio"},
        state_model="prepare allocates a fixed voice table; trigger and release mutate prepared slots only.",
        seed_model="none", input_domain="voice trigger and release events", output_domain="voice allocation and termination records",
        units=["MIDI note", "frames", "voice index"], latency="zero", tail="termination-fade-frames",
        scheduling="event-synchronous",
        _compile="pulp::audio::InstrumentVoiceAllocator value; (void)value;",
    ),
    capability(
        key="midi.mpe-voice-tracker", domain="midi",
        include="pulp/midi/mpe_voice_tracker.hpp", symbol="pulp::midi::MpeVoiceTracker",
        summary="Fixed-capacity MIDI 1.0 and UMP MPE note ownership and expression tracking.", rt_class="audio",
        lifecycle={"construction": "control", "prepare": "none", "process": "audio", "reset": "audio", "release": "none"},
        state_model="Fixed 128-slot note table with explicit reset and monotonic note identities.",
        seed_model="none", input_domain="MIDI events and UMP packets", output_domain="owned per-note expression state",
        units=["MIDI note", "MIDI channel", "semitones", "normalized ratio"], latency="zero", tail="owned-notes-until-release-or-reset",
        scheduling="event-synchronous",
        _compile="pulp::midi::MpeVoiceTracker value; (void)value;",
    ),
    capability(
        key="timebase.tick", domain="timebase", include="pulp/timebase/tick.hpp",
        symbol="pulp::timebase::TickPosition",
        summary="Saturating integer musical position on the 705600-tick quarter-note grid.", rt_class="any",
        lifecycle={"construction": "any", "prepare": "none", "process": "any", "reset": "value-initialization", "release": "none"},
        state_model="Value type with saturating arithmetic over signed 64-bit ticks.", seed_model="none",
        input_domain="document ticks", output_domain="document ticks", units=["ticks"], latency="zero", tail="none",
        scheduling="pure",
        _compile="pulp::timebase::TickPosition value{}; (void)value;",
    ),
    capability(
        key="timebase.swing", domain="timebase", include="pulp/timebase/quantize.hpp",
        symbol="pulp::timebase::SwingRatio",
        summary="Exact rational swing projection and inverse over integer document ticks.", rt_class="any",
        lifecycle={"construction": "any", "prepare": "none", "process": "any", "reset": "value-initialization", "release": "none"},
        state_model="Pure value and free-function transform; invalid inputs leave positions unchanged.", seed_model="none",
        input_domain="document ticks and rational swing", output_domain="document ticks", units=["ticks", "rational ratio"],
        latency="zero", tail="none", scheduling="pure",
        _compile="pulp::timebase::SwingRatio value{}; (void)pulp::timebase::swing_position({}, {1}, value);",
    ),
    capability(
        key="sequence.host-transport-projector", domain="sequence",
        include="pulp/sequence/host_transport_projector.hpp", symbol="pulp::sequence::HostTransportProjector",
        summary="Prepared projection from host callback transport into Pulp playback snapshots.", rt_class="mixed",
        lifecycle={"construction": "control", "prepare": "control", "process": "audio", "reset": "control-or-audio-when-quiescent", "release": "none"},
        state_model="Prepared tempo-map reference plus bounded callback history and playback epoch.", seed_model="none",
        input_domain="host process context", output_domain="playback transport snapshot",
        units=["samples", "ticks", "beats per minute"], latency="zero", tail="none", scheduling="block-synchronous",
        _compile="pulp::sequence::HostTransportProjector value; (void)value;",
    ),
]


def repo_root() -> pathlib.Path:
    path = pathlib.Path(__file__).resolve()
    for parent in path.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "core").is_dir():
            return parent
    raise SystemExit("agent-capabilities: could not locate the repository root")


def public_rows() -> list[dict[str, Any]]:
    return [{k: copy.deepcopy(v) for k, v in row.items() if not k.startswith("_")} for row in EXPORTS]


def document() -> dict[str, Any]:
    rows = sorted(public_rows(), key=lambda row: row["key"])
    counts = {domain: sum(row["domain"] == domain for row in rows) for domain in sorted(DOMAINS)}
    return {
        "schema": SCHEMA,
        "capabilities": rows,
        "counts": {"total": len(rows), "by_domain": counts},
        "compatibility": {
            "signal_vocabulary": "tools/dsp_vocabulary.py remains the legacy signal-only projection during migration"
        },
    }


def compile_fixture() -> str:
    includes = sorted({row["include"] for row in EXPORTS})
    lines = ["// Generated by tools/scripts/agent_capability_manifest.py --write.",
             "// Every advertised public symbol is included and instantiated here.", ""]
    lines.extend(f"#include <{header}>" for header in includes)
    lines.extend(["", "int main() {"])
    for row in sorted(EXPORTS, key=lambda item: item["key"]):
        lines.append("    {")
        lines.append(f"        // {row['key']}")
        lines.append(f"        {row['_compile']}")
        lines.append("    }")
    lines.extend(["    return 0;", "}", ""])
    return "\n".join(lines)


def validate(doc: Any, root: pathlib.Path) -> list[str]:
    problems: list[str] = []
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        return [f"schema must be exactly {SCHEMA}"]
    rows = doc.get("capabilities")
    if not isinstance(rows, list) or not rows:
        return ["capabilities must be a non-empty array"]
    known = {(row["include"], row["symbol"]) for row in public_rows()}
    forge_path = root / "docs/status/forge-catalog.json"
    forge_keys: set[str] = set()
    if forge_path.exists():
        forge_keys = {row.get("key") for row in json.loads(forge_path.read_text()).get("nodes", [])}
    seen: set[str] = set()
    for index, row in enumerate(rows):
        where = f"capabilities[{index}]"
        if not isinstance(row, dict):
            problems.append(f"{where} must be an object")
            continue
        missing = sorted(REQUIRED - row.keys())
        if missing:
            problems.append(f"{where} missing required fields: {', '.join(missing)}")
        key = row.get("key")
        if not isinstance(key, str) or not re.fullmatch(r"[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*", key):
            problems.append(f"{where} has invalid stable key {key!r}")
        elif key in seen:
            problems.append(f"duplicate capability key: {key}")
        else:
            seen.add(key)
        if row.get("domain") not in DOMAINS:
            problems.append(f"{where} has invalid domain {row.get('domain')!r}")
        if row.get("rt_class") not in RT_CLASSES:
            problems.append(f"{where} has invalid rt_class {row.get('rt_class')!r}")
        include, symbol = row.get("include"), row.get("symbol")
        if isinstance(include, str) and not (root / "core" / include.removeprefix("pulp/")).exists():
            # Public includes are rooted one module below core, e.g. core/signal/include/pulp/...
            candidates = list(root.glob(f"core/*/include/{include}"))
            if not candidates:
                problems.append(f"{where} advertises missing include {include}")
        if (include, symbol) not in known:
            problems.append(f"{where} advertises symbol outside curated exports: {symbol!r}")
        forbidden = sorted(FORBIDDEN_NUMERIC_CONTRACT_FIELDS & row.keys())
        if forbidden:
            problems.append(f"{where} duplicates Forge numeric contract fields: {', '.join(forbidden)}")
        units = row.get("units")
        if not isinstance(units, list) or not all(isinstance(unit, str) and unit for unit in units):
            problems.append(f"{where} units must be an array of non-empty strings")
        lifecycle = row.get("lifecycle")
        lifecycle_fields = {"construction", "prepare", "process", "reset", "release"}
        if not isinstance(lifecycle, dict) or set(lifecycle) != lifecycle_fields:
            problems.append(f"{where} lifecycle must contain exactly {', '.join(sorted(lifecycle_fields))}")
        descriptor = row.get("forge_descriptor")
        if descriptor is not None:
            if not isinstance(descriptor, dict) or descriptor.get("catalog") != "forge-catalog.json":
                problems.append(f"{where} has invalid forge_descriptor reference")
            elif descriptor.get("node_key") not in forge_keys:
                problems.append(f"{where} references missing Forge descriptor {descriptor.get('node_key')!r}")
    return problems


def rendered() -> str:
    return json.dumps(document(), indent=2, ensure_ascii=False) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Pulp installed agent capability manifest")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--json", action="store_true", help="emit the curated manifest")
    mode.add_argument("--check", action="store_true", help="validate collisions and snapshot freshness")
    mode.add_argument("--write", action="store_true", help="regenerate the snapshot and compile fixture")
    mode.add_argument("--validate", metavar="PATH", help="validate a manifest or negative fixture")
    parser.add_argument("--snapshot", type=pathlib.Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    root = repo_root()
    if args.validate:
        problems = validate(json.loads(pathlib.Path(args.validate).read_text()), root)
        for problem in problems:
            print(f"agent-capabilities: INVALID: {problem}", file=sys.stderr)
        return 1 if problems else 0
    doc = document()
    problems = validate(doc, root)
    if problems:
        for problem in problems:
            print(f"agent-capabilities: INVALID: {problem}", file=sys.stderr)
        return 1
    output = rendered()
    snapshot = args.snapshot or root / SNAPSHOT
    fixture = root / COMPILE_FIXTURE
    if args.json:
        sys.stdout.write(output)
        return 0
    if args.write:
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(output)
        fixture.write_text(compile_fixture())
        print(f"agent-capabilities: wrote {SNAPSHOT} and {COMPILE_FIXTURE} ({len(doc['capabilities'])} capabilities)")
        return 0
    if args.check:
        failed = False
        if not snapshot.exists() or snapshot.read_text() != output:
            print(f"agent-capabilities: STALE: {snapshot} differs from curated exports", file=sys.stderr)
            failed = True
        if not fixture.exists() or fixture.read_text() != compile_fixture():
            print(f"agent-capabilities: STALE: {COMPILE_FIXTURE} differs from curated exports", file=sys.stderr)
            failed = True
        if not failed:
            print(f"agent-capabilities: fresh; {len(doc['capabilities'])} stable keys are collision-free")
        return 1 if failed else 0
    print("Pulp agent capability manifest")
    print(f"  schema        {SCHEMA}")
    print(f"  capabilities  {len(doc['capabilities'])}")
    for domain, count in doc["counts"]["by_domain"].items():
        if count:
            print(f"  {domain:<12} {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
