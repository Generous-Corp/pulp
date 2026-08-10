#!/usr/bin/env python3
"""Independently verify a capability lesson against a final Rack patch.

This verifier consumes the durable lesson, the final ``.vcv`` JSON, and the
module inventory.  It never reads a planner BuildPlan, selected-node annotation,
or planner verdict.  Capability-node assignment, cable roles, physical control
values, and distinct lanes are reconstructed from the artifact being judged.

Every registered assertion reports PASS, FAIL, or UNMEASURED.  Unknown
assertion/probe types are schema errors, never accidental passes.  The search
is bounded so common lint stays deterministic and fast; exhausting that bound
returns UNMEASURED instead of guessing.
"""

from __future__ import annotations

import math
from typing import Any

import capability_lessons as lessons
import idiom_check
import param_units

PASS, FAIL, UNMEASURED = "PASS", "FAIL", "UNMEASURED"


def _entry(inv: dict, module: dict) -> dict:
    return ((inv.get(module.get("plugin"), {}).get("modules") or {})
            .get(module.get("model"), {}))


def _module_candidates(node: dict, patch: dict, inv: dict, roles: dict) -> list:
    return [module for module in patch.get("modules") or []
            if idiom_check._module_matches(node["role"], _entry(inv, module), roles)]


def _port_verdict(kind: str, direction: str, module: dict, index: Any,
                  inv: dict, roles: dict) -> bool | None:
    if not isinstance(index, int) or index < 0:
        return False
    entry = _entry(inv, module)
    names = entry.get("outputs" if direction == "out" else "inputs") or []
    carried = entry.get("roles_out" if direction == "out" else "roles_in") or []
    if not names and not carried:
        return None                    # genuinely uncartographed
    if index >= max(len(names), len(carried)):
        return False                   # a known module does not have this jack
    label = names[index] if index < len(names) else None
    role = carried[index] if index < len(carried) else None
    return idiom_check._port_matches(kind, role, label, roles)


def _cable_state(rel: dict, source: dict, target: dict, cable: dict,
                 inv: dict, roles: dict) -> bool | None:
    if cable.get("outputModuleId") != source.get("id") \
            or cable.get("inputModuleId") != target.get("id"):
        return False
    out = _port_verdict(rel["out"], "out", source, cable.get("outputId"),
                        inv, roles)
    into = _port_verdict(rel["in"], "in", target, cable.get("inputId"),
                         inv, roles)
    if out is False or into is False:
        return False
    if out is None or into is None:
        return None
    return True


def _relation_choices(rel: dict, mapping: dict, patch: dict, inv: dict,
                      roles: dict, allow_unknown: bool) -> list[tuple[int, dict]]:
    out = []
    for index, cable in enumerate(patch.get("cables") or []):
        state = _cable_state(rel, mapping[rel["from"]], mapping[rel["to"]],
                             cable, inv, roles)
        if state is True or (allow_unknown and state is None):
            out.append((index, cable))
    return out


class _Budget:
    def __init__(self, maximum: int):
        self.maximum = max(1, maximum)
        self.used = 0
        self.exhausted = False

    def take(self) -> bool:
        self.used += 1
        if self.used > self.maximum:
            self.exhausted = True
            return False
        return True


def _solve(lesson: dict, patch: dict, inv: dict, roles: dict,
           relation_ids: list[str], distinct_groups: list[list[str]],
           allow_unknown: bool, budget: _Budget,
           solution_predicate=None) -> dict | None:
    """One consistent injective node mapping and cable assignment, or None."""
    relations = {rel["id"]: rel for rel in lesson.get("relations") or []}
    wanted = [relations[rid] for rid in relation_ids]
    participating = {rel[end] for rel in wanted for end in ("from", "to")}
    nodes = [node for node in lesson["nodes"] if node["id"] in participating]
    candidates = {node["id"]: _module_candidates(node, patch, inv, roles)
                  for node in nodes}
    if any(not choices for choices in candidates.values()):
        return None
    order = sorted((node["id"] for node in nodes), key=lambda n: len(candidates[n]))

    def cables_for(mapping: dict) -> dict | None:
        options = [_relation_choices(rel, mapping, patch, inv, roles,
                                     allow_unknown) for rel in wanted]
        if any(not choices for choices in options):
            return None
        chosen: dict[str, dict] = {}

        def choose(index: int, used: set[int],
                   occupied_inputs: set[tuple[Any, Any]]) -> bool:
            if not budget.take():
                return False
            if index == len(wanted):
                for group in distinct_groups:
                    lanes = []
                    for rid in group:
                        rel = relations[rid]
                        cable = chosen.get(rid)
                        if cable is None:
                            return False
                        lanes.append((mapping[rel["from"]]["id"],
                                      cable.get("outputId")))
                    if any(port is None for _, port in lanes) \
                            or len(lanes) != len(set(lanes)):
                        return False
                return True
            rel = wanted[index]
            for cable_index, cable in options[index]:
                destination = (cable.get("inputModuleId"), cable.get("inputId"))
                if cable_index in used or destination in occupied_inputs:
                    continue
                chosen[rel["id"]] = cable
                if choose(index + 1, used | {cable_index},
                          occupied_inputs | {destination}):
                    return True
            chosen.pop(rel["id"], None)
            return False

        return dict(chosen) if choose(0, set(), set()) else None

    mapping: dict[str, dict] = {}

    def assign(index: int, used: set[Any]) -> dict | None:
        if not budget.take():
            return None
        if index == len(order):
            cables = cables_for(mapping)
            if cables is None:
                return None
            solution = {"nodes": dict(mapping), "cables": cables}
            return (solution if solution_predicate is None
                    or solution_predicate(solution) else None)
        node_id = order[index]
        for module in candidates[node_id]:
            module_id = module.get("id")
            if module_id in used:
                continue                    # one capability node, one instance
            mapping[node_id] = module
            got = assign(index + 1, used | {module_id})
            if got is not None:
                return got
        mapping.pop(node_id, None)
        return None

    return assign(0, set())


def _solve_verdict(lesson: dict, patch: dict, inv: dict, roles: dict,
                   relation_ids: list[str], distinct_groups: list[list[str]],
                   max_states: int) -> tuple[str, dict | None, str]:
    strict_budget = _Budget(max_states)
    strict = _solve(lesson, patch, inv, roles, relation_ids, distinct_groups,
                    False, strict_budget)
    if strict is not None:
        return PASS, strict, "reconstructed from concrete module instances and jacks"
    if strict_budget.exhausted:
        return UNMEASURED, None, f"verification exceeded {max_states} search states"
    unknown_budget = _Budget(max_states)
    possible = _solve(lesson, patch, inv, roles, relation_ids, distinct_groups,
                      True, unknown_budget)
    if possible is not None or unknown_budget.exhausted:
        return UNMEASURED, possible, ("a required module has no exact port map"
                                     if possible is not None else
                                     f"verification exceeded {max_states} search states")
    return FAIL, None, "no consistent capability-node and cable assignment exists"


def _observation(observations: dict, field: str):
    if field in observations:
        return observations[field]
    value: Any = observations
    for part in field.split("."):
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return value


def _metric(assertion: dict, observations: dict) -> tuple[str, str]:
    actual = _observation(observations, assertion["field"])
    if not isinstance(actual, (int, float)) or not math.isfinite(float(actual)):
        return UNMEASURED, f"no numeric observation for {assertion['field']}"
    op = assertion["op"]
    expected = assertion["value"]
    comparisons = {
        ">": actual > expected, ">=": actual >= expected,
        "<": actual < expected, "<=": actual <= expected,
        "==": actual == expected,
    }
    return (PASS if comparisons[op] else FAIL,
            f"{assertion['field']} was {actual}; expected {op} {expected}")


def _probe(assertion: dict, observations: dict) -> tuple[str, str]:
    observed = (observations.get("probes") or {}).get(assertion["probe"])
    if not isinstance(observed, dict):
        return UNMEASURED, f"probe {assertion['probe']} was not run"
    verdict = str(observed.get("verdict") or "").upper()
    if verdict not in (PASS, FAIL, UNMEASURED):
        raise lessons.LessonError(f"probe {assertion['probe']} returned "
                                  f"unregistered verdict {verdict!r}")
    return verdict, str(observed.get("why") or assertion["contract"])


def _written_param(module: dict, param_id: Any) -> float | None:
    params = module.get("params") or []
    if isinstance(params, dict):
        value = params.get(str(param_id), params.get(param_id))
        if isinstance(value, dict):
            value = value.get("value")
        return float(value) if isinstance(value, (int, float)) else None
    for param in params:
        if isinstance(param, dict) and param.get("id") == param_id \
                and isinstance(param.get("value"), (int, float)):
            return float(param["value"])
    return None


def _verify_control(spec: dict, mapping: dict | None, inv: dict) -> tuple[str, str]:
    if mapping is None or spec["node"] not in mapping.get("nodes", {}):
        return UNMEASURED, "the capability instance could not be reconstructed"
    module = mapping["nodes"][spec["node"]]
    entry = _entry(inv, module)
    possible = []
    for param in entry.get("params") or []:
        if param.get("affords") != spec["affordance"]:
            continue
        placement = param_units.place(param, spec["target"]["value"],
                                      unit=spec["target"]["unit"])
        if placement.value is not None and (not placement.clamped
                                             or spec.get("allow_clamp", False)):
            possible.append((param, placement.value))
    if not possible:
        return UNMEASURED, "no exact semantic control transform is available"
    for param, expected in possible:
        actual = _written_param(module, param.get("id"))
        if actual is None:
            default = param.get("defaultValue", param.get("default"))
            if isinstance(default, (int, float)):
                actual = float(default)
        if actual is not None and math.isclose(actual, expected,
                                               rel_tol=1e-6, abs_tol=1e-6):
            return PASS, f"param {param.get('id')} is {actual:g} raw"
    if any(_written_param(module, param.get("id")) is not None or
           isinstance(param.get("defaultValue", param.get("default")),
                      (int, float)) for param, _ in possible):
        return FAIL, "the final patch wrote a different physical-control value"
    return UNMEASURED, "the final patch carries no readable value for the control"


def verify(lesson: dict, patch: dict, inv: dict, observations: dict | None = None,
           roles: dict | None = None, max_states: int = 10000) -> dict:
    """Reconstruct and verify the final artifact; never consume a BuildPlan."""
    roles = roles if roles is not None else idiom_check.load_roles()
    bad = lessons.problems({lesson.get("id"): lesson}, roles=roles)
    if bad:
        raise lessons.LessonError("; ".join(bad))
    observations = observations or {}
    relation_ids = [rel["id"] for rel in lesson.get("relations") or []]
    distinct = [a["relations"] for a in lesson.get("assertions") or []
                if a["kind"] == "distinct_source_ports"]
    topology, mapping, why = _solve_verdict(
        lesson, patch, inv, roles, relation_ids, distinct, max_states)
    required_controls = [control for control in lesson.get("controls") or []
                         if control.get("required", True)]
    if topology == PASS and required_controls:
        def controls_are(solution: dict, accepted: set[str]) -> bool:
            return all(_verify_control(control, solution, inv)[0] in accepted
                       for control in required_controls)

        # Prefer a mapping that proves every required control. If none does,
        # prefer an honest unknown over a mapping that actively contradicts a
        # target. This makes the verdict independent of module order.
        control_search_exhausted = False
        control_mapping = None
        for accepted in ({PASS}, {PASS, UNMEASURED}):
            budget = _Budget(max_states)
            candidate = _solve(lesson, patch, inv, roles, relation_ids, distinct,
                               False, budget,
                               lambda solution, accepted=accepted:
                               controls_are(solution, accepted))
            if candidate is not None:
                control_mapping = candidate
                break
            control_search_exhausted |= budget.exhausted
        if control_mapping is not None:
            mapping = control_mapping
        elif control_search_exhausted:
            # Topology itself was proven, but the bounded search did not prove
            # which complete mapping owns the required controls.  Withhold the
            # mapping so control checks remain honest UNMEASURED, not false FAIL.
            mapping = None
    results = [{"id": "topology", "verdict": topology, "why": why}]

    for assertion in lesson.get("assertions") or []:
        kind = assertion["kind"]
        if kind == "relation_present":
            verdict, _, reason = _solve_verdict(
                lesson, patch, inv, roles, [assertion["relation"]], [], max_states)
        elif kind == "distinct_source_ports":
            verdict, _, reason = _solve_verdict(
                lesson, patch, inv, roles, assertion["relations"],
                [assertion["relations"]], max_states)
        elif kind == "metric":
            verdict, reason = _metric(assertion, observations)
        elif kind == "probe":
            verdict, reason = _probe(assertion, observations)
        else:                              # schema check should make unreachable
            raise lessons.LessonError(f"unregistered assertion type {kind!r}")
        results.append({"id": assertion["id"], "verdict": verdict,
                        "why": reason})

    for control in lesson.get("controls") or []:
        verdict, reason = _verify_control(control, mapping, inv)
        results.append({"id": "control:" + control["id"],
                        "verdict": verdict, "why": reason,
                        "required": control.get("required", True)})
    blocking = [result for result in results if result.get("required", True)]
    return {"lesson_id": lesson["id"], "results": results,
            "verdict": (FAIL if any(r["verdict"] == FAIL for r in blocking) else
                        UNMEASURED if any(r["verdict"] == UNMEASURED
                                          for r in blocking)
                        else PASS),
            "search_mapping": ({node: module.get("id")
                                for node, module in mapping["nodes"].items()}
                               if mapping else None)}
