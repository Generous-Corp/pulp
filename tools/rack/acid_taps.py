#!/usr/bin/env python3
"""Plan synchronized, semantically witnessed taps for an acid patch.

This module deliberately does not infer a jack from its position.  A tap is
available only when the acid idiom and the installed Rack/Cartog inventory can
prove the entire path to it.  Missing or ambiguous evidence is UNMEASURED,
which is different from a patch whose measured structure is wrong.

    acid_taps.py patch.vcv                 # print the tap plan as JSON
    acid_taps.py patch.vcv --inventory x   # use a saved inventory

The eight taps are ordered because one Fidelity probe records them on one
sample clock.  `evaluate_capture` currently judges only that measurement
contract; it does not claim the captured sound is a convincing 303.
"""

from __future__ import annotations

import dataclasses
import json
import math
import sys
from typing import Iterable

import idiom_check as I
from module_kinds import is_audio_interface


ORDER = ("clock", "raw_pitch", "post_slew_pitch", "accent", "slide",
         "effective_cutoff", "filter_audio", "final_audio")
READY, PASS, FAIL, UNMEASURED = "READY", "PASS", "FAIL", "UNMEASURED"


@dataclasses.dataclass(frozen=True)
class Path:
    """One fully cartographed semantic route through the patch."""

    origin: tuple[int, int]
    terminal: tuple[int, int]
    source_module: int
    destination_module: int
    edges: tuple[tuple[int, int, int, int], ...]


@dataclasses.dataclass
class Plan:
    status: str
    taps: list[tuple[int, int, str]]
    witnesses: dict[str, dict]
    reasons: list[str]
    failures: list[str]

    def as_dict(self) -> dict:
        return dataclasses.asdict(self)


def _known_port(inv: dict, module: dict, side: str, index: int,
                kind: str, roles: dict) -> bool:
    """Match a real inventory port; absence is never a label guess."""
    entry = I._entry(inv, module)
    names = entry.get("outputs" if side == "out" else "inputs") or []
    if not isinstance(index, int) or index < 0 or index >= len(names):
        return False
    role, label = I._port_info(inv, module, side, index)
    return I._port_matches(kind, role, label, roles)


def _transparent_modules(by_id: dict, inv: dict, roles: dict) -> set[int]:
    transparent_roles = [name for name, spec in roles["roles"].items()
                         if spec.get("transparent")]
    return {mid for mid, module in by_id.items()
            if any(I._module_matches(role, I._entry(inv, module), roles)
                   for role in transparent_roles)}


def _paths(patch: dict, inv: dict, roles: dict, req: dict) -> list[Path]:
    """All strict paths satisfying one idiom requirement.

    `origin` remains the first physical source jack while `terminal` advances
    across transparent modules.  This is the distinction needed to prove that
    pitch/accent/slide are separate lanes while also observing the combined
    cutoff CV that the filter actually receives.
    """
    by_id = {m.get("id"): m for m in patch.get("modules") or []}
    from_role = req.get("from_module", "any")
    to_role = req.get("to_module", "any")
    froms = {mid for mid, module in by_id.items()
             if I._module_matches(from_role, I._entry(inv, module), roles)}
    tos = {mid for mid, module in by_id.items()
           if I._module_matches(to_role, I._entry(inv, module), roles)}
    transparent = _transparent_modules(by_id, inv, roles)
    outgoing: dict[int, list[dict]] = {}
    for cable in patch.get("cables") or []:
        outgoing.setdefault(cable.get("outputModuleId"), []).append(cable)
    for cables in outgoing.values():
        cables.sort(key=lambda c: (c.get("outputId", -1),
                                   c.get("inputModuleId", -1),
                                   c.get("inputId", -1)))

    queue: list[tuple[dict, tuple[int, int], tuple]] = []
    for source_id in sorted(froms):
        source = by_id[source_id]
        for cable in outgoing.get(source_id, []):
            output_id = cable.get("outputId")
            if _known_port(inv, source, "out", output_id,
                           req.get("from_port", "any_out"), roles):
                edge = (source_id, output_id, cable.get("inputModuleId"),
                        cable.get("inputId"))
                queue.append((cable, (source_id, output_id), (edge,)))

    found: set[Path] = set()
    while queue:
        cable, origin, edges = queue.pop(0)
        dst_id = cable.get("inputModuleId")
        dst = by_id.get(dst_id)
        if dst is None:
            continue
        terminal = (cable.get("outputModuleId"), cable.get("outputId"))
        if dst_id in tos and _known_port(
                inv, dst, "in", cable.get("inputId"),
                req.get("to_port", "any_in"), roles):
            if not req.get("same_module") or origin[0] == dst_id:
                if not req.get("different_module") or origin[0] != dst_id:
                    found.add(Path(origin, terminal, origin[0], dst_id, edges))

        # A repeated edge is a feedback loop, not another route to a witness.
        if dst_id in transparent:
            for next_cable in outgoing.get(dst_id, []):
                edge = (dst_id, next_cable.get("outputId"),
                        next_cable.get("inputModuleId"),
                        next_cable.get("inputId"))
                if edge not in edges:
                    queue.append((next_cable, origin, edges + (edge,)))
    return sorted(found, key=lambda p: (p.origin, p.terminal, p.edges))


def _one(paths: Iterable[Path], name: str,
         reasons: list[str]) -> Path | None:
    paths = list(paths)
    if not paths:
        reasons.append(f"{name}: no fully cartographed witness path exists")
        return None
    # Separate cables can duplicate a route without making its tap ambiguous.
    by_tap: dict[tuple, Path] = {}
    for path in paths:
        by_tap.setdefault((path.origin, path.terminal,
                           path.source_module, path.destination_module), path)
    if len(by_tap) != 1:
        choices = sorted((p.origin, p.terminal) for p in by_tap.values())
        reasons.append(f"{name}: {len(by_tap)} witness paths are possible "
                       f"{choices}; choosing one would be a guess")
        return None
    return next(iter(by_tap.values()))


def _witness(path: Path, tap: tuple[int, int], meaning: str) -> dict:
    return {"module": tap[0], "output": tap[1], "meaning": meaning,
            "physical_origin": list(path.origin),
            "terminal_output": list(path.terminal),
            "destination_module": path.destination_module,
            "path": [list(edge) for edge in path.edges]}


def plan(patch: dict, inv: dict, roles: dict | None = None,
         idiom: dict | None = None) -> Plan:
    """Return the one coherent eight-tap acid plan this patch proves."""
    roles = roles or I.load_roles()
    idiom = idiom or I.load_idioms().get("acid-voice")
    reasons: list[str] = []
    failures: list[str] = []
    if not idiom:
        return Plan(UNMEASURED, [], {},
                    ["acid-voice: the required idiom record is unavailable"], [])
    required = {req.get("id"): req for req in idiom.get("topology") or []}
    ids = ("seq-to-slew", "slide-control", "slew-to-osc",
           "filter-to-vca", "accent-to-cutoff")
    absent = [rid for rid in ids if rid not in required]
    if absent:
        return Plan(UNMEASURED, [], {},
                    [f"acid-voice: idiom lacks {', '.join(absent)}"], [])

    by_id = {m.get("id"): m for m in patch.get("modules") or []}
    sequencers = sorted(mid for mid, module in by_id.items()
                        if I._module_matches("sequencer", I._entry(inv, module),
                                             roles))
    if len(sequencers) != 1:
        reasons.append(f"sequencer: expected one cartographed acid sequencer, "
                       f"found {len(sequencers)}")
        return Plan(UNMEASURED, [], {}, reasons, failures)
    sequencer = sequencers[0]

    raw = _one((p for p in _paths(patch, inv, roles,
                                   required["seq-to-slew"])
                if p.source_module == sequencer), "raw_pitch", reasons)
    slide = _one((p for p in _paths(patch, inv, roles,
                                     required["slide-control"])
                  if p.source_module == sequencer
                  and (raw is None or p.destination_module
                       == raw.destination_module)), "slide", reasons)
    post = _one((p for p in _paths(patch, inv, roles,
                                    required["slew-to-osc"])
                 if raw is None or p.source_module == raw.destination_module),
                "post_slew_pitch", reasons)
    accent = _one((p for p in _paths(patch, inv, roles,
                                      required["accent-to-cutoff"])
                   if p.source_module == sequencer), "accent", reasons)
    filtered = _one((p for p in _paths(patch, inv, roles,
                                        required["filter-to-vca"])
                     if accent is None or p.source_module
                     == accent.destination_module), "filter_audio", reasons)

    # The synchronization witness is the mapped clock signal entering the
    # selected sequencer, not its trigger output after the step has occurred.
    clock_req = {"from_module": "any", "from_port": "clock_out",
                 "to_module": "sequencer", "to_port": "clock_in"}
    clock = _one((p for p in _paths(patch, inv, roles, clock_req)
                  if p.destination_module == sequencer), "clock", reasons)

    # Final audio needs no guessed Core input: the source jack is cartographed
    # as audio and the destination identity is a known Rack audio interface.
    final_paths: list[Path] = []
    for cable in patch.get("cables") or []:
        src = by_id.get(cable.get("outputModuleId"))
        dst = by_id.get(cable.get("inputModuleId"))
        if not src or not dst or not is_audio_interface(dst):
            continue
        if not _known_port(inv, src, "out", cable.get("outputId"),
                           "audio_out", roles):
            continue
        edge = (src.get("id"), cable.get("outputId"), dst.get("id"),
                cable.get("inputId"))
        final_paths.append(Path((src.get("id"), cable.get("outputId")),
                                (src.get("id"), cable.get("outputId")),
                                src.get("id"), dst.get("id"), (edge,)))
    final = _one((p for p in final_paths
                  if filtered is None or p.source_module
                  == filtered.destination_module), "final_audio", reasons)

    paths = {"clock": clock, "raw_pitch": raw, "post_slew_pitch": post,
             "accent": accent, "slide": slide,
             "effective_cutoff": accent, "filter_audio": filtered,
             "final_audio": final}
    if reasons or any(path is None for path in paths.values()):
        return Plan(UNMEASURED, [], {}, reasons, failures)

    assert raw and accent and slide
    lanes = {"raw_pitch": raw.origin, "accent": accent.origin,
             "slide": slide.origin}
    if len(set(lanes.values())) != len(lanes):
        failures.append("pitch, accent, and slide share a physical sequencer "
                        f"lane: {lanes}")

    tap_at = {"clock": clock.terminal, "raw_pitch": raw.origin,
              "post_slew_pitch": post.terminal, "accent": accent.origin,
              "slide": slide.origin,
              "effective_cutoff": accent.terminal,
              "filter_audio": filtered.origin, "final_audio": final.origin}
    meanings = {
        "clock": "clock entering the one sequencer",
        "raw_pitch": "sequencer pitch before slew",
        "post_slew_pitch": "pitch after the selected slew path",
        "accent": "physical sequencer accent lane",
        "slide": "physical sequencer slide-select lane",
        "effective_cutoff": "external CV actually entering the filter",
        "filter_audio": "filter output entering the acid amplifier",
        "final_audio": "amplifier output entering Rack audio",
    }
    taps = [(tap_at[name][0], tap_at[name][1], name) for name in ORDER]
    witnesses = {name: _witness(paths[name], tap_at[name], meanings[name])
                 for name in ORDER}
    post_module = by_id.get(post.source_module, {})
    if (post_module.get("plugin"), post_module.get("model"), post.origin[1]) \
            == ("Fundamental", "Process", 4):
        # VCV Process SLEW jumps while Gate is high and slews while it is low.
        # This active-low realization is intentionally distinct from its
        # GLIDE output, whose same-edge 1ms guard cannot select a pitch change
        # arriving simultaneously from one sequencer.
        witnesses["slide"]["active_polarity"] = "low"
    return Plan(READY, taps, witnesses, reasons, failures)


def evaluate_capture(plan_: Plan, series: dict[str, list[float]]) -> dict:
    """Judge capture integrity only, without pretending to judge acid taste."""
    if plan_.status != READY:
        return {"verdict": UNMEASURED, "scope": "capture-contract-only",
                "reasons": list(plan_.reasons), "observations": {}}
    if plan_.failures:
        return {"verdict": FAIL, "scope": "capture-contract-only",
                "reasons": list(plan_.failures), "observations": {}}
    reasons, observations, lengths = [], {}, set()
    for name in ORDER:
        values = series.get(name)
        if not isinstance(values, list) or not values:
            reasons.append(f"{name}: synchronized series is missing")
            continue
        if not all(isinstance(v, (int, float)) and math.isfinite(v)
                   for v in values):
            reasons.append(f"{name}: series contains a non-finite sample")
            continue
        lengths.add(len(values))
        changes = sum(a != b for a, b in zip(values, values[1:]))
        rises = sum(a <= 1.0 and b > 1.0
                    for a, b in zip(values, values[1:]))
        observations[name] = {"minimum": min(values), "maximum": max(values),
                              "changes": changes, "rising_edges": rises}
    if len(lengths) > 1:
        reasons.append(f"series lengths differ: {sorted(lengths)}")
    if reasons:
        return {"verdict": UNMEASURED, "scope": "capture-contract-only",
                "reasons": reasons, "observations": observations}
    return {"verdict": PASS, "scope": "capture-contract-only", "reasons": [],
            "observations": observations,
            "note": "PASS proves synchronized witness coverage, not 303 quality"}


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.split("\n\n")[1].strip())
        return 2
    with open(argv[1]) as source:
        patch = json.load(source)
    if "--inventory" in argv:
        at = argv.index("--inventory") + 1
        if at >= len(argv):
            print("--inventory needs a path", file=sys.stderr)
            return 2
        with open(argv[at]) as source:
            inv = json.load(source)
    else:
        import patch as patch_module
        inv = patch_module.inventory()
    result = plan(patch, inv)
    print(json.dumps(result.as_dict(), indent=2, sort_keys=True))
    return 0 if result.status == READY and not result.failures else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
