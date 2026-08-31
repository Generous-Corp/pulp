#!/usr/bin/env python3
"""Bounded, evidence-driven repairs that never spend another model call."""

from __future__ import annotations

import copy
import math
from dataclasses import dataclass, field

from module_kinds import is_audio_interface


@dataclass
class Repair:
    patch: dict | None = None
    actions: list[str] = field(default_factory=list)
    refusal: list[str] = field(default_factory=list)


def _roles(value) -> frozenset[str]:
    """One exact semantic role set, or empty when the inventory is unsure."""
    values = value if isinstance(value, list) else [value]
    roles = frozenset(
        item.casefold() for item in values
        if isinstance(item, str) and item.strip())
    if roles & {"any", "any_out", "unknown"}:
        return frozenset()
    return roles


def alternate_output_repairs(
        patch: dict, inv: dict, finding: dict | None,
        ) -> tuple[list[Repair], list[str]]:
    """Offer measured-live, exact-role sibling outputs for a silent path.

    This is deliberately a candidate planner, not a success oracle. The caller
    must run every returned patch through the real DSP gate and keep one only
    when that gate measures it audible. A sibling merely being active in the
    trace is enough to justify an audition, never enough to claim success.
    """
    if not finding or finding.get("whole_module"):
        return [], ["the silence is not one dead output on a live module"]
    module_id = finding.get("id")
    output_id = finding.get("output")
    activity = finding.get("outs")
    if (not isinstance(module_id, int) or not isinstance(output_id, int) or
            not isinstance(activity, list) or not 0 <= output_id < len(activity)):
        return [], ["the silent-output finding is incomplete"]

    modules = {module.get("id"): module
               for module in patch.get("modules") or []}
    module = modules.get(module_id)
    if module is None:
        return [], [f"module {module_id} is absent from the patch"]
    entry = (inv.get(module.get("plugin"), {}).get("modules", {})
                .get(module.get("model"), {}))
    names = entry.get("outputs") or []
    roles = entry.get("roles_out") or []
    coords = entry.get("outputs_xy")
    selected_roles = _roles(
        roles[output_id] if output_id < len(roles) else None)
    if not selected_roles:
        return [], ["the silent output has no specific recorded semantic role"]

    # Only touch cables that can reach a listener. A fan-out from the same jack
    # may drive unrelated modulation elsewhere; changing it would be larger
    # than the audio-path repair justified by this evidence.
    reaches_listener = {candidate_id
                        for candidate_id, candidate in modules.items()
                        if is_audio_interface(candidate)}
    cables = patch.get("cables") or []
    changed = True
    while changed:
        changed = False
        for cable in cables:
            if (cable.get("inputModuleId") in reaches_listener and
                    cable.get("outputModuleId") not in reaches_listener):
                reaches_listener.add(cable.get("outputModuleId"))
                changed = True
    path_cables = [cable for cable in cables
                   if cable.get("outputModuleId") == module_id and
                   cable.get("outputId") == output_id and
                   cable.get("inputModuleId") in reaches_listener]
    if not path_cables:
        return [], ["the silent output has no cable on a path to an audio interface"]

    alternatives = []
    for candidate_id, peak in enumerate(activity):
        if candidate_id == output_id or not isinstance(peak, (int, float)):
            continue
        if not math.isfinite(float(peak)) or peak <= 0.0:
            continue
        if (isinstance(coords, list) and any(row is not None for row in coords)
                and (candidate_id >= len(coords) or coords[candidate_id] is None)):
            continue
        candidate_roles = _roles(
            roles[candidate_id] if candidate_id < len(roles) else None)
        if candidate_roles != selected_roles:
            continue
        alternatives.append((abs(candidate_id - output_id), candidate_id))
    alternatives.sort()
    if not alternatives:
        return [], ["no measured-live sibling output has the exact same semantic role"]

    plugin, model = module.get("plugin"), module.get("model")
    old_label = names[output_id] if output_id < len(names) else f"OUT {output_id}"
    repairs = []
    for _, candidate_id in alternatives:
        repaired = copy.deepcopy(patch)
        changed_ids = {cable.get("id") for cable in path_cables}
        for cable in repaired.get("cables") or []:
            if cable.get("id") in changed_ids:
                cable["outputId"] = candidate_id
        new_label = (names[candidate_id] if candidate_id < len(names)
                     else f"OUT {candidate_id}")
        repairs.append(Repair(
            patch=repaired,
            actions=[
                f"auditioned {plugin}/{model} out{candidate_id} "
                f"'{new_label}' in place of silent same-role out{output_id} "
                f"'{old_label}' on {len(path_cables)} listener path cable(s)"
            ]))
    return repairs, []


def _position(module: dict) -> tuple[float, float] | None:
    pos = module.get("pos")
    if (not isinstance(pos, list) or len(pos) != 2 or
            not all(isinstance(value, (int, float)) for value in pos)):
        return None
    return float(pos[0]), float(pos[1])


def _next_id(rows: list[dict]) -> int:
    return max((row.get("id") for row in rows
                if isinstance(row.get("id"), int)), default=-1) + 1


def repair_activation(patch: dict, inv: dict,
                      findings: list[str]) -> Repair:
    """Repair exact CVfunk 2.0.48 activation defects when topology decides.

    A Glass gate source is already proven suitable by its existing Gate cable.
    It can therefore be fanned into that same Glass V/Oct input.  An adjacent
    Aulos may share it only when exactly one gate-driven Glass is closest;
    ties refuse rather than guessing at the intended layer.
    """
    if (inv.get("CVfunk") or {}).get("version") != "2.0.48":
        return Repair(refusal=["CVfunk activation repair has no exact 2.0.48 contract"])

    glass_ids, aulos_ids = set(), set()
    for finding in findings:
        words = finding.split()
        try:
            module_id = int(words[2])
        except (IndexError, ValueError):
            continue
        if finding.startswith("CVfunk/Glass module ") and "no cable to input 0" in finding:
            glass_ids.add(module_id)
        elif finding.startswith("CVfunk/Aulos module ") and "without input 2" in finding:
            aulos_ids.add(module_id)
    if not glass_ids and not aulos_ids:
        return Repair(refusal=["no supported structured activation findings"])

    repaired = copy.deepcopy(patch)
    modules = {module.get("id"): module for module in repaired.get("modules") or []}
    cables = repaired.get("cables") or []
    next_cable = _next_id(cables)
    actions = []
    refusal = []

    # Record each existing Glass Gate feeder before adding any fan-outs.
    glass_gate = {}
    for glass_id in glass_ids | {
            module_id for module_id, module in modules.items()
            if module.get("plugin") == "CVfunk" and module.get("model") == "Glass"}:
        feeds = [cable for cable in cables
                 if cable.get("inputModuleId") == glass_id and
                 cable.get("inputId") == 1]
        endpoints = {(c.get("outputModuleId"), c.get("outputId")) for c in feeds}
        if len(endpoints) == 1:
            glass_gate[glass_id] = next(iter(endpoints))

    for glass_id in sorted(glass_ids):
        source = glass_gate.get(glass_id)
        if source is None:
            refusal.append(f"Glass module {glass_id} has no unique existing Gate source")
            continue
        cables.append({"id": next_cable, "outputModuleId": source[0],
                       "outputId": source[1], "inputModuleId": glass_id,
                       "inputId": 0})
        next_cable += 1
        actions.append(f"fanned Glass {glass_id} Gate source into V/Oct")

    gate_targets = [(modules.get(glass_id), source)
                    for glass_id, source in glass_gate.items()
                    if modules.get(glass_id) is not None]
    ordered_aulos = sorted(
        ((modules.get(module_id), _position(modules.get(module_id) or {}))
         for module_id in aulos_ids),
        key=lambda item: item[1] or (float("inf"), float("inf")))
    ordered_glass = sorted(
        ((module, _position(module)) for module, _ in gate_targets),
        key=lambda item: item[1] or (float("inf"), float("inf")))
    paired_gate = {}
    if (len(ordered_aulos) == len(ordered_glass) and ordered_aulos and
            all(ap is not None and gp is not None and ap[1] == gp[1] and
                ap[0] < gp[0]
                for (_, ap), (_, gp) in zip(ordered_aulos, ordered_glass))):
        intervals = [(ap[0], gp[0]) for (_, ap), (_, gp)
                     in zip(ordered_aulos, ordered_glass)]
        if all(left[1] < right[0]
               for left, right in zip(intervals, intervals[1:])):
            sources = {module.get("id"): source
                       for module, source in gate_targets}
            paired_gate = {
                aulos.get("id"): (glass.get("id"), sources[glass.get("id")])
                for (aulos, _), (glass, _) in zip(ordered_aulos, ordered_glass)}
    for aulos_id in sorted(aulos_ids):
        aulos = modules.get(aulos_id)
        at = _position(aulos or {})
        ranked = []
        if at is not None:
            for glass, source in gate_targets:
                gp = _position(glass)
                if gp is not None:
                    ranked.append(((at[0] - gp[0]) ** 2 + (at[1] - gp[1]) ** 2,
                                   glass.get("id"), source))
        ranked.sort()
        if aulos_id in paired_gate:
            glass_id, source = paired_gate[aulos_id]
        elif not ranked or (len(ranked) > 1 and ranked[0][0] == ranked[1][0]):
            refusal.append(
                f"Aulos module {aulos_id} has no unique nearest proven Gate source")
            continue
        else:
            _, glass_id, source = ranked[0]
        cables.append({"id": next_cable, "outputModuleId": source[0],
                       "outputId": source[1], "inputModuleId": aulos_id,
                       "inputId": 2})
        next_cable += 1
        actions.append(f"shared Glass {glass_id} Gate source with Aulos {aulos_id}")

    if refusal:
        return Repair(refusal=refusal)
    repaired["cables"] = cables
    return Repair(patch=repaired, actions=actions)
