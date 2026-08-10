#!/usr/bin/env python3
"""Bounded, evidence-driven repairs that never spend another model call."""

from __future__ import annotations

import copy
from dataclasses import dataclass, field


@dataclass
class Repair:
    patch: dict | None = None
    actions: list[str] = field(default_factory=list)
    refusal: list[str] = field(default_factory=list)


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
