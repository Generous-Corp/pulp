#!/usr/bin/env python3
"""Compile source-neutral patch lessons into machine-specific build plans.

The durable side of this file deliberately knows no plugin or model slug.  A
lesson says which *capabilities* a patch needs, how those capabilities relate,
which facts must be measured, and which destructive mutations must make those
facts fail.  ``compile_lesson`` is the replaceable-realisation boundary: it
chooses concrete modules from the user's installed, owned, or free-downloadable
set and records why every choice and control transform was trusted.

Candidate lessons are loadable for harness work but are excluded from normal
generation.  Learning a recurring graph is a proposal, not proof that it is a
good patch.

    python3 capability_lessons.py --check
    python3 capability_lessons.py --list-candidates
"""

from __future__ import annotations

import copy
import json
import os
import sys
from typing import Any

import idiom_check
import param_units

HERE = os.path.dirname(os.path.abspath(__file__))
LESSON_DIR = os.path.join(HERE, "knowledge", "capability")
SCHEMA = 1
STATUSES = ("admitted", "candidate", "quarantined")
OPS = (">", ">=", "<", "<=", "==")
PROBES = frozenset({
    "amplitude-beat-rate",
    "step-synchronous-pitch",
    "step-synchronous-spectrum",
})
TRAITS = frozenset({"voice", "articulation", "generative", "physical-model",
                    "percussion", "texture", "effect", "control", "sequence"})
BEHAVIOURS = frozenset({"melodic", "rhythmic", "keeps_going", "sustained",
                        "spectrally_varying", "varies_timing", "accelerating",
                        "unmetered"})

# These keys cross the abstraction boundary.  Concrete identity belongs only
# in a compiled, machine-local plan; source identity belongs in the private
# research ledger that proposed the lesson.  Neither may leak into a durable
# capability record.
IDENTITY_KEYS = frozenset({
    "plugin", "module", "model", "maker", "author", "source", "source_id",
    "patch", "patch_id", "url", "license",
})

LESSON_KEYS = frozenset({"id", "status", "intent", "traits", "why", "behavior",
                         "nodes", "relations", "controls", "assertions", "mutations",
                         "independent_holdout"})
NODE_KEYS = frozenset({"id", "role"})
RELATION_KEYS = frozenset({"id", "from", "out", "to", "in"})
CONTROL_KEYS = frozenset({"id", "node", "affordance", "target", "required",
                          "allow_clamp"})
TARGET_KEYS = frozenset({"value", "unit"})
ASSERTION_KEYS = {
    "relation_present": frozenset({"id", "kind", "relation"}),
    "distinct_source_ports": frozenset({"id", "kind", "relations"}),
    "metric": frozenset({"id", "kind", "field", "op", "value"}),
    "probe": frozenset({"id", "kind", "probe", "contract"}),
}
MUTATION_KEYS = {
    "remove_relation": frozenset({"id", "operation", "relation", "must_fail"}),
    "collapse_source_ports": frozenset({"id", "operation", "relations",
                                         "must_fail"}),
}


class LessonError(ValueError):
    """A lesson cannot be validated or compiled safely."""


def load(path: str | None = None, include_candidates: bool = False) -> dict:
    """Return lessons keyed by id; normal generation sees admitted rows only."""
    root = path or LESSON_DIR
    out: dict[str, dict] = {}
    if not os.path.isdir(root):
        return out
    for name in sorted(os.listdir(root)):
        if not name.endswith(".json") or name.startswith("_"):
            continue
        with open(os.path.join(root, name)) as f:
            doc = json.load(f)
        if doc.get("schema") != SCHEMA:
            raise LessonError(f"{name} uses capability lesson schema "
                              f"{doc.get('schema')!r}, expected {SCHEMA}")
        for lesson in doc.get("lessons") or []:
            lid = lesson.get("id")
            if not lid:
                raise LessonError(f"{name} contains a lesson with no id")
            if lid in out:
                raise LessonError(f"duplicate capability lesson id {lid!r}")
            if include_candidates or lesson.get("status") == "admitted":
                out[lid] = lesson
    return out


def _walk(value: Any, path: str = "lesson"):
    if isinstance(value, dict):
        for key, child in value.items():
            yield path, key, child
            yield from _walk(child, f"{path}.{key}")
    elif isinstance(value, list):
        for i, child in enumerate(value):
            yield from _walk(child, f"{path}[{i}]")


def problems(lessons: dict | None = None, roles: dict | None = None,
             measurements: dict | None = None) -> list[str]:
    """Return every schema, reference, and admission-boundary problem."""
    lessons = lessons if lessons is not None else load(include_candidates=True)
    roles = roles if roles is not None else idiom_check.load_roles()
    measurements = (measurements if measurements is not None
                    else idiom_check.load_measurements())
    bad: list[str] = []

    def rows(lesson: dict, field: str, prefix: str, required: bool = False) -> list:
        raw = lesson.get(field)
        if not isinstance(raw, list):
            bad.append(f"{prefix}: {field} must be a list")
            return []
        if required and not raw:
            bad.append(f"{prefix}: {field} must not be empty")
        out = []
        for index, row in enumerate(raw):
            if not isinstance(row, dict):
                bad.append(f"{prefix}: {field}[{index}] must be an object")
            else:
                out.append(row)
        return out

    def ids(rows_: list[dict], label: str, prefix: str) -> list[str]:
        """Return only schema-valid IDs so malformed JSON always fails closed."""
        raw = [row.get("id") for row in rows_]
        valid = [value for value in raw
                 if isinstance(value, str) and value]
        if len(valid) != len(raw) or len(valid) != len(set(valid)):
            bad.append(f"{prefix}: {label} ids must be present and unique")
        return valid

    for lid, raw_lesson in sorted(lessons.items()):
        prefix = str(lid)
        if not isinstance(raw_lesson, dict):
            bad.append(f"{prefix}: lesson must be an object")
            continue
        lesson = raw_lesson
        extra = set(lesson) - LESSON_KEYS
        missing = {"id", "status", "intent", "traits", "why", "behavior",
                   "nodes", "relations", "controls", "assertions", "mutations",
                   "independent_holdout"} - set(lesson)
        if extra:
            bad.append(f"{prefix}: unknown lesson fields: {', '.join(sorted(extra))}")
        if missing:
            bad.append(f"{prefix}: missing lesson fields: {', '.join(sorted(missing))}")
        if lesson.get("id") != lid:
            bad.append(f"{prefix}: dictionary key and lesson id disagree")
        status = lesson.get("status")
        if status not in STATUSES:
            bad.append(f"{prefix}: status must be one of {STATUSES}")
        holdout = lesson.get("independent_holdout")
        if not isinstance(holdout, bool):
            bad.append(f"{prefix}: independent_holdout must be boolean")
        elif status == "admitted" and not holdout:
            bad.append(f"{prefix}: admitted lessons require an independent holdout")
        intents = lesson.get("intent")
        if not isinstance(intents, list) or not intents \
                or any(not isinstance(item, str) or not item.strip() for item in intents):
            bad.append(f"{prefix}: intent must contain non-empty semantic phrases")
        traits = lesson.get("traits")
        if not isinstance(traits, list) or not traits \
                or any(trait not in TRAITS for trait in traits):
            bad.append(f"{prefix}: traits must use registered capability families")
        behavior = lesson.get("behavior")
        if not isinstance(behavior, list) \
                or any(flag not in BEHAVIOURS for flag in behavior):
            bad.append(f"{prefix}: behavior must use registered behavior flags")
        if not isinstance(lesson.get("why"), str) \
                or len(lesson.get("why", "").strip()) < 40:
            bad.append(f"{prefix}: why must explain the source-neutral mechanism")
        for path, key, _ in _walk(lesson, prefix):
            if key in IDENTITY_KEYS:
                bad.append(f"{path}: {key!r} is concrete identity, not a "
                           "capability-level field")

        nodes = rows(lesson, "nodes", prefix, required=True)
        node_ids = ids(nodes, "capability node", prefix)
        for node in nodes:
            role = node.get("role")
            if set(node) != NODE_KEYS:
                bad.append(f"{prefix}/{node.get('id')}: node fields must be "
                           f"{sorted(NODE_KEYS)}")
            if not isinstance(role, str) or role not in roles.get("roles", {}):
                bad.append(f"{prefix}: node {node.get('id')!r} uses unknown "
                           f"role {role!r}")

        relations = rows(lesson, "relations", prefix, required=True)
        relation_ids = ids(relations, "relation", prefix)
        for rel in relations:
            rid = rel.get("id")
            if set(rel) != RELATION_KEYS:
                bad.append(f"{prefix}/{rid}: relation fields must be "
                           f"{sorted(RELATION_KEYS)}")
            if rel.get("from") not in node_ids or rel.get("to") not in node_ids:
                bad.append(f"{prefix}/{rid}: relation endpoint is not a node")
            if not isinstance(rel.get("out"), str) \
                    or rel.get("out") not in roles.get("ports", {}):
                bad.append(f"{prefix}/{rid}: unknown output role {rel.get('out')!r}")
            if not isinstance(rel.get("in"), str) \
                    or rel.get("in") not in roles.get("ports", {}):
                bad.append(f"{prefix}/{rid}: unknown input role {rel.get('in')!r}")

        controls = rows(lesson, "controls", prefix)
        control_ids = ids(controls, "control", prefix)
        for ctl in controls:
            cid = ctl.get("id")
            target = ctl.get("target")
            if set(ctl) - CONTROL_KEYS \
                    or not {"id", "node", "affordance", "target"} <= set(ctl):
                bad.append(f"{prefix}/{cid}: control fields are invalid")
            for flag in ("required", "allow_clamp"):
                if flag in ctl and not isinstance(ctl[flag], bool):
                    bad.append(f"{prefix}/{cid}: {flag} must be boolean")
            if not isinstance(target, dict):
                bad.append(f"{prefix}/{cid}: physical target must be an object")
                target = {}
            elif set(target) != TARGET_KEYS:
                bad.append(f"{prefix}/{cid}: physical-target fields must be "
                           f"{sorted(TARGET_KEYS)}")
            if ctl.get("node") not in node_ids:
                bad.append(f"{prefix}/{cid}: control node is not declared")
            if not isinstance(ctl.get("affordance"), str) or not ctl.get("affordance"):
                bad.append(f"{prefix}/{cid}: control needs a semantic affordance")
            if not isinstance(target.get("value"), (int, float)) \
                    or not isinstance(target.get("unit"), str) \
                    or not target.get("unit").strip():
                bad.append(f"{prefix}/{cid}: physical target needs numeric value "
                           "and a non-empty unit")
        referenced_nodes = {value for rel in relations for end in ("from", "to")
                            if isinstance((value := rel.get(end)), str)}
        referenced_nodes |= {value for control in controls
                             if isinstance((value := control.get("node")), str)}
        isolated = [node_id for node_id in node_ids if node_id not in referenced_nodes]
        if isolated:
            bad.append(f"{prefix}: isolated capability nodes are not verifiable: "
                       f"{', '.join(str(node) for node in isolated)}")

        assertions = rows(lesson, "assertions", prefix, required=True)
        assertion_ids = ids(assertions, "assertion", prefix)
        for assertion in assertions:
            aid, kind = assertion.get("id"), assertion.get("kind")
            allowed = ASSERTION_KEYS.get(kind) if isinstance(kind, str) else None
            if allowed is None:
                bad.append(f"{prefix}/{aid}: unknown assertion kind {kind!r}")
                continue
            if set(assertion) != allowed:
                bad.append(f"{prefix}/{aid}: {kind} fields must be {sorted(allowed)}")
            if kind == "relation_present":
                if assertion.get("relation") not in relation_ids:
                    bad.append(f"{prefix}/{aid}: assertion names no relation")
            elif kind == "distinct_source_ports":
                refs = assertion.get("relations")
                if not isinstance(refs, list) or len(refs) < 2 \
                        or any(ref not in relation_ids for ref in refs):
                    bad.append(f"{prefix}/{aid}: distinct-port assertion has "
                               "invalid relation references")
            elif kind == "metric":
                if assertion.get("field") not in measurements:
                    bad.append(f"{prefix}/{aid}: unknown measured field "
                               f"{assertion.get('field')!r}")
                if assertion.get("op") not in OPS \
                        or not isinstance(assertion.get("value"), (int, float)):
                    bad.append(f"{prefix}/{aid}: metric comparison is invalid")
            elif kind == "probe":
                if assertion.get("probe") not in PROBES:
                    bad.append(f"{prefix}/{aid}: unregistered probe "
                               f"{assertion.get('probe')!r}")
                if not isinstance(assertion.get("contract"), str) \
                        or not assertion.get("contract"):
                    bad.append(f"{prefix}/{aid}: probe needs a contract")

        assertion_by_id = {assertion["id"]: assertion for assertion in assertions
                           if isinstance(assertion.get("id"), str)}
        mutations = rows(lesson, "mutations", prefix)
        mutation_ids = ids(mutations, "mutation", prefix)
        for mutation in mutations:
            mid, operation = mutation.get("id"), mutation.get("operation")
            allowed = MUTATION_KEYS.get(operation) if isinstance(operation, str) else None
            if allowed is None:
                bad.append(f"{prefix}/{mid}: unknown mutation {operation!r}")
                continue
            if set(mutation) != allowed:
                bad.append(f"{prefix}/{mid}: {operation} fields must be "
                           f"{sorted(allowed)}")
            refs = mutation.get("relations") or [mutation.get("relation")]
            if not isinstance(refs, list):
                refs = []
            refs = [ref for ref in refs if ref]
            if not refs or any(ref not in relation_ids for ref in refs):
                bad.append(f"{prefix}/{mid}: mutation has invalid relation refs")
            must_fail = mutation.get("must_fail")
            if not isinstance(must_fail, list) or not must_fail \
                    or any(a not in assertion_by_id for a in must_fail):
                bad.append(f"{prefix}/{mid}: mutation must name assertions it breaks")
                must_fail = []
            if operation == "remove_relation":
                target = mutation.get("relation")
                if any(assertion_by_id.get(a, {}).get("kind") !=
                       "relation_present" or
                       assertion_by_id.get(a, {}).get("relation") != target
                       for a in must_fail):
                    bad.append(f"{prefix}/{mid}: every must_fail assertion must "
                               f"be the removed relation {target!r}")
            if operation == "collapse_source_ports":
                sources = [next((rel.get("from") for rel in relations
                                 if rel.get("id") == rid), None) for rid in refs]
                if not sources or any(source != sources[0]
                                      for source in sources[1:]) or any(
                        assertion_by_id.get(a, {}).get("kind") !=
                        "distinct_source_ports" or
                        set(assertion_by_id.get(a, {}).get("relations") or []) !=
                        set(refs) for a in must_fail):
                    bad.append(f"{prefix}/{mid}: collapsed lanes must come from "
                               "one node and exactly match every must_fail assertion")
    return bad


def _premium(value: Any) -> bool:
    return value is True or str(value).strip().casefold() in ("true", "1", "yes")


def _installable(entry: dict) -> bool:
    """A downloadable candidate must publish a build for this architecture."""
    try:
        import patch  # local, dependency-free for this question
        return patch.installable_here(entry)
    except Exception:  # noqa: BLE001
        return False


def _module_matches(role: str, module: dict, roles: dict) -> tuple[bool, int]:
    wanted = [t.casefold() for t in roles["roles"][role].get("tags") or []]
    tags = [t.casefold() for t in module.get("tags") or []]
    positions = [tags.index(tag) for tag in wanted if tag in tags]
    return (not wanted or bool(positions), min(positions) if positions else 999)


def _port_options(module: dict, direction: str, port_kind: str,
                  roles: dict) -> list[dict]:
    names_key = "outputs" if direction == "out" else "inputs"
    roles_key = "roles_out" if direction == "out" else "roles_in"
    names = module.get(names_key) or []
    carried = module.get(roles_key) or []
    inferred = bool(module.get(roles_key + "_inferred"))
    out = []
    for index in range(max(len(names), len(carried))):
        label = names[index] if index < len(names) else None
        role = carried[index] if index < len(carried) else None
        if not idiom_check._port_matches(port_kind, role, label, roles):
            continue
        mine = set(role) if isinstance(role, list) else ({role} if role else set())
        declared = bool(mine & set(roles["ports"][port_kind].get("ports") or []))
        if declared:
            confidence = 0.72 if inferred else 0.97
            provenance = ("inferred-port-role" if inferred
                          else "declared-or-measured-port-role")
        else:
            confidence, provenance = 0.64, "exact-port-label"
        out.append({"index": index, "label": label or f"p{index}",
                    "confidence": confidence, "provenance": provenance})
    return out


def _candidate_rows(role: str, required_ports: list[tuple[str, str]], inv: dict,
                    module_index: dict, catalog: dict, owned: set,
                    roles: dict) -> list[dict]:
    """Eligible realisations, ranked by friction then evidence quality."""
    identities = {(p, m) for p, plug in inv.items()
                  for m in (plug.get("modules") or {})}
    identities |= {(p, m) for p, mods in module_index.items() for m in mods}
    rows = []
    for plugin, model in sorted(identities):
        installed_module = (inv.get(plugin, {}).get("modules") or {}).get(model)
        catalog_module = (module_index.get(plugin) or {}).get(model)
        module = installed_module or catalog_module or {}
        matches, tag_rank = _module_matches(role, module, roles)
        if not matches:
            continue
        centry = catalog.get(plugin) or {}
        is_premium = _premium(centry.get("premium"))
        if installed_module is not None:
            availability, cost = "installed", 0
        elif is_premium and plugin in owned and _installable(centry):
            availability, cost = "owned", 1
        # Absence is not evidence of being free.  A module-index row without a
        # matching publication record is useful for names/tags, but must never
        # turn an unknown commercial status into permission to download.
        elif plugin in catalog and "premium" in centry \
                and not is_premium and _installable(centry):
            availability, cost = "free", 2
        else:
            continue                         # would require a purchase or cannot run

        ports: dict[str, list] = {}
        if installed_module is not None:
            for direction, kind in required_ports:
                ports[f"{direction}:{kind}"] = _port_options(
                    installed_module, direction, kind, roles)
            if any(not choices for choices in ports.values()):
                continue                    # installed, and demonstrably cannot wire
        rows.append({
            "plugin": plugin, "model": model, "availability": availability,
            "acquisition_cost": cost, "tag_rank": tag_rank,
            "confidence": 0.98 if installed_module is not None else 0.80,
            "provenance": ("installed-module-metadata" if installed_module
                           is not None else "published-module-metadata"),
            "ports": ports,
            "ready": installed_module is not None,
        })
    rows.sort(key=lambda r: (r["acquisition_cost"], r["tag_rank"],
                             -r["confidence"], r["plugin"], r["model"]))
    return rows


def _control(module: dict, spec: dict) -> tuple[dict | None, str]:
    affordance = spec["affordance"]
    candidates = []
    for param in module.get("params") or []:
        if param.get("affords") != affordance:
            continue
        known = param.get("affordance_confidence") == "known"
        placement = param_units.place(param, spec["target"]["value"],
                                      unit=spec["target"]["unit"])
        if placement.value is None:
            continue
        if placement.clamped and not spec.get("allow_clamp", False):
            continue
        candidates.append((not known, placement.clamped, int(param.get("id", 0)),
                           param, placement))
    if not candidates:
        return None, (f"no {affordance!r} control has an exact "
                      f"{spec['target']['unit']} transform")
    _, _, _, param, placement = sorted(candidates, key=lambda x: x[:3])[0]
    shown = param_units.to_display(placement.value, param)
    return {
        "parameter": {"id": param.get("id"), "name": param.get("name")},
        "raw_value": placement.value,
        "display_value": shown,
        "unit": spec["target"]["unit"],
        "transform": {
            "display_base": float(param.get("displayBase", 0.0)),
            "display_multiplier": float(param.get("displayMultiplier", 1.0)),
            "display_offset": float(param.get("displayOffset", 0.0)),
            "round_trip_exact": shown is not None and abs(
                shown - float(spec["target"]["value"])) <= 1e-6,
        },
        "confidence": (0.99 if param.get("affordance_confidence") == "known"
                       else 0.70),
        "provenance": ("known-semantic-affordance+exact-display-transform"
                       if param.get("affordance_confidence") == "known" else
                       "inferred-semantic-affordance+exact-display-transform"),
        "clamped": placement.clamped,
    }, ""


def compile_lesson(lesson: dict, inv: dict, module_index: dict, catalog: dict,
                   owned: set | None = None, allow_candidate: bool = False,
                   roles: dict | None = None) -> dict:
    """Resolve one lesson into a machine-specific, provenance-carrying plan.

    The returned plan can say ``needs_acquisition`` while naming an eligible
    owned/free realisation.  It says ``ready`` only when every jack and required
    physical control was resolved from installed metadata.  It never admits a
    premium unowned module.
    """
    roles = roles if roles is not None else idiom_check.load_roles()
    validation = problems({lesson.get("id"): lesson}, roles=roles)
    if validation:
        raise LessonError("; ".join(validation))
    status = lesson.get("status")
    if status == "quarantined" or (status != "admitted" and not allow_candidate):
        raise LessonError(f"{lesson['id']} is {status} and is quarantined "
                          "from generation")
    owned = set(owned or set())

    port_needs: dict[str, list[tuple[str, str]]] = {
        node["id"]: [] for node in lesson["nodes"]}
    for rel in lesson.get("relations") or []:
        port_needs[rel["from"]].append(("out", rel["out"]))
        port_needs[rel["to"]].append(("in", rel["in"]))
    control_needs: dict[str, list[dict]] = {
        node["id"]: [] for node in lesson["nodes"]}
    for control in lesson.get("controls") or []:
        control_needs[control["node"]].append(control)

    node_choices, issues = {}, []
    for node in lesson["nodes"]:
        choices = _candidate_rows(node["role"], port_needs[node["id"]], inv,
                                  module_index, catalog, owned, roles)
        usable = []
        for choice in choices:
            if not choice["ready"]:
                usable.append(choice)       # control metadata arrives after install
                continue
            module = ((inv.get(choice["plugin"], {}).get("modules") or {})
                      .get(choice["model"], {}))
            resolved_controls, failed = {}, False
            for control in control_needs[node["id"]]:
                value, reason = _control(module, control)
                if value is None and control.get("required", True):
                    failed = True            # known incapable; try the next module
                    break
                if value is not None:
                    resolved_controls[control["id"]] = value
            if not failed:
                choice = copy.deepcopy(choice)
                choice["resolved_controls"] = resolved_controls
                usable.append(choice)
        choices = usable
        if not choices:
            issues.append(f"no eligible realisation for {node['id']} "
                          f"({node['role']})")
            continue
        node_choices[node["id"]] = choices

    relation_specs = {rel["id"]: rel for rel in lesson.get("relations") or []}
    distinct_groups = [assertion["relations"]
                       for assertion in lesson.get("assertions") or []
                       if assertion.get("kind") == "distinct_source_ports"]

    def choice_supports(node_id: str, choice: dict) -> bool:
        if not choice["ready"]:
            return True                 # exact capacity arrives after acquisition
        incoming = [rel for rel in relation_specs.values() if rel["to"] == node_id]

        def assign_inputs(index: int, used: set[int]) -> bool:
            if index == len(incoming):
                return True
            rel = incoming[index]
            options = choice["ports"].get(f"in:{rel['in']}") or []
            return any(option["index"] not in used and
                       assign_inputs(index + 1, used | {option["index"]})
                       for option in options)

        if not assign_inputs(0, set()):
            return False
        constrained = list(dict.fromkeys(
            rid for group in distinct_groups for rid in group
            if relation_specs[rid]["from"] == node_id))

        def assign_outputs(index: int, picked: dict[str, int]) -> bool:
            if index == len(constrained):
                for group in distinct_groups:
                    local = [rid for rid in group
                             if relation_specs[rid]["from"] == node_id]
                    if len(local) != len({picked[rid] for rid in local}):
                        return False
                return True
            rid = constrained[index]
            rel = relation_specs[rid]
            options = choice["ports"].get(f"out:{rel['out']}") or []
            return any(assign_outputs(index + 1,
                                      {**picked, rid: option["index"]})
                       for option in options)

        return assign_outputs(0, {})

    nodes = {}
    for node in lesson["nodes"]:
        choices = node_choices.get(node["id"], [])
        viable = [choice for choice in choices if choice_supports(node["id"], choice)]
        if not viable:
            if choices:
                issues.append(f"no realisation has enough ports for {node['id']}")
            continue
        nodes[node["id"]] = {"capability": node["role"],
                             "realisation": viable[0],
                             "alternatives": viable[1:4]}

    compiled_relations = []
    if len(nodes) == len(lesson["nodes"]):
        for rel in lesson.get("relations") or []:
            src = nodes[rel["from"]]["realisation"]
            dst = nodes[rel["to"]]["realisation"]
            outs = src["ports"].get(f"out:{rel['out']}") or []
            ins = dst["ports"].get(f"in:{rel['in']}") or []
            compiled_relations.append({
                "id": rel["id"], "from": rel["from"], "to": rel["to"],
                "output": copy.deepcopy(outs[0]) if outs else None,
                "input": None,
            })

        # Rack permits output fan-out but exactly one cable per input jack.
        # Allocate destination ports jointly so two sources cannot both be
        # compiled onto input zero of a mixer that actually has one input.
        by_spec = {rel["id"]: rel for rel in lesson["relations"]}
        input_options = []
        for rel in compiled_relations:
            spec = by_spec[rel["id"]]
            dst = nodes[spec["to"]]["realisation"]
            input_options.append(dst["ports"].get(f"in:{spec['in']}") or [])

        def assign_inputs(i: int, used: set[tuple[str, int]],
                          picked: list[dict | None]) -> list[dict | None] | None:
            if i == len(input_options):
                return picked
            spec = by_spec[compiled_relations[i]["id"]]
            if not nodes[spec["to"]]["realisation"]["ready"]:
                return assign_inputs(i + 1, used, picked + [None])
            for option in input_options[i]:
                key = (spec["to"], option["index"])
                if key not in used:
                    got = assign_inputs(i + 1, used | {key}, picked + [option])
                    if got is not None:
                        return got
            return None

        inputs = assign_inputs(0, set(), [])
        if inputs is None:
            issues.append("cannot assign one distinct destination jack per cable")
        else:
            for rel, option in zip(compiled_relations, inputs):
                rel["input"] = copy.deepcopy(option)

        # Assign all overlapping distinct-lane constraints together. Solving
        # each assertion separately lets a later group overwrite the shared
        # relation chosen by an earlier one and can reject a feasible plan.
        by_id = {r["id"]: r for r in compiled_relations}
        groups = [assertion["relations"]
                  for assertion in lesson.get("assertions") or []
                  if assertion.get("kind") == "distinct_source_ports"]
        constrained = list(dict.fromkeys(rid for group in groups for rid in group))
        relation_specs = {rel["id"]: rel for rel in lesson["relations"]}
        output_options = {}
        for rid in constrained:
            spec = relation_specs[rid]
            src = nodes[spec["from"]]["realisation"]
            output_options[rid] = src["ports"].get(f"out:{spec['out']}") or []

        def assign_outputs(index: int, picked: dict[str, dict]) -> dict | None:
            if index == len(constrained):
                for group in groups:
                    lanes = [(relation_specs[rid]["from"],
                              picked[rid]["index"]) for rid in group]
                    if len(lanes) != len(set(lanes)):
                        return None
                return picked
            rid = constrained[index]
            for option in output_options[rid]:
                got = assign_outputs(index + 1, {**picked, rid: option})
                if got is not None:
                    return got
            return None

        picked = assign_outputs(0, {}) if constrained else {}
        all_ready = all(nodes[relation_specs[rid]["from"]]["realisation"]["ready"]
                        for rid in constrained)
        if picked is None and all_ready:
            issues.append("cannot jointly assign distinct source ports")
        elif picked is not None:
            for rid, option in picked.items():
                by_id[rid]["output"] = copy.deepcopy(option)

    controls = []
    for spec in lesson.get("controls") or []:
        resolved = nodes.get(spec["node"], {}).get("realisation")
        module = ((inv.get(resolved["plugin"], {}).get("modules") or {})
                  .get(resolved["model"])) if resolved else None
        if module is None:
            if spec.get("required", True):
                issues.append(f"{spec['id']} waits for installed control metadata")
            continue
        value = (resolved.get("resolved_controls") or {}).get(spec["id"])
        reason = ""
        if value is None:
            value, reason = _control(module, spec)
        if value is None:
            if spec.get("required", True):
                issues.append(f"{spec['id']}: {reason}")
            continue
        controls.append({"id": spec["id"], "node": spec["node"], **value})

    plan = {"schema": SCHEMA, "lesson_id": lesson["id"],
            "lesson_status": lesson["status"], "nodes": nodes,
            "relations": compiled_relations, "controls": controls,
            "assertions": copy.deepcopy(lesson.get("assertions") or []),
            "issues": issues}
    structural = evaluate_static(plan)
    plan["static_assertions"] = structural
    plan["deferred_assertions"] = [
        assertion["id"] for assertion in plan["assertions"]
        if assertion.get("kind") in ("metric", "probe")]
    failed = [a for a, passed in structural.items() if not passed]
    if failed:
        plan["issues"].append("static assertions failed: " + ", ".join(failed))
    all_installed = len(nodes) == len(lesson["nodes"]) and all(
        node["realisation"]["ready"] for node in nodes.values())
    complete = len(nodes) == len(lesson["nodes"])
    plan["state"] = ("ready" if all_installed and not plan["issues"] else
                     "needs_acquisition" if complete and any(
                         not node["realisation"]["ready"] for node in nodes.values())
                     else "unresolved")
    return plan


def compile_intent(intent: str, inv: dict, module_index: dict, catalog: dict,
                   owned: set | None = None, entries: dict | None = None,
                   allow_candidate: bool = False, roles: dict | None = None) -> list:
    """Compile every blueprint for one semantic intent; never choose one graph.

    One request can have several sound constructions and each construction can
    have several replaceable module realisations.  The planner above preserves
    the latter as alternatives; this boundary preserves the former as separate
    plans for the caller to rank using readiness and independent verification.
    """
    entries = entries if entries is not None else load(
        include_candidates=allow_candidate)
    wanted = " ".join(intent.casefold().split())
    matched = [lesson for lesson in entries.values()
               if wanted in {" ".join(phrase.casefold().split())
                             for phrase in lesson.get("intent") or []}]
    return [compile_lesson(lesson, inv, module_index, catalog, owned,
                           allow_candidate=allow_candidate, roles=roles)
            for lesson in matched]


def evaluate_static(plan: dict) -> dict[str, bool]:
    """Evaluate assertions the compiled graph can settle without audio."""
    relations = {r["id"]: r for r in plan.get("relations") or []}
    out: dict[str, bool] = {}
    for assertion in plan.get("assertions") or []:
        kind, aid = assertion.get("kind"), assertion.get("id")
        if kind == "relation_present":
            rel = relations.get(assertion.get("relation"))
            out[aid] = bool(rel and rel.get("output") is not None
                            and rel.get("input") is not None)
        elif kind == "distinct_source_ports":
            refs = [relations.get(rid) for rid in assertion.get("relations") or []]
            sources = [(r.get("from"), (r.get("output") or {}).get("index"))
                       for r in refs if r]
            out[aid] = (len(sources) == len(refs)
                        and all(index is not None for _, index in sources)
                        and len(sources) == len(set(sources)))
    return out


def mutate_plan(plan: dict, mutation: dict) -> dict:
    """Apply a declared destructive control for harness validation."""
    out = copy.deepcopy(plan)
    relations = {r["id"]: r for r in out.get("relations") or []}
    if mutation["operation"] == "remove_relation":
        rel = relations.get(mutation["relation"])
        if rel:
            rel["output"] = rel["input"] = None
    elif mutation["operation"] == "collapse_source_ports":
        refs = [relations[r] for r in mutation["relations"] if r in relations]
        first = next((r.get("output") for r in refs if r.get("output")), None)
        for rel in refs:
            rel["output"] = copy.deepcopy(first)
    else:
        raise LessonError(f"unknown mutation {mutation.get('operation')!r}")
    return out


def main(argv: list[str]) -> int:
    if argv == ["--check"]:
        bad = problems()
        if bad:
            print("\n".join("FAIL " + problem for problem in bad))
            return 1
        lessons = load(include_candidates=True)
        admitted = len(load())
        print(f"all good: {len(lessons)} capability lessons; {admitted} admitted")
        return 0
    if argv == ["--list-candidates"]:
        for lesson in load(include_candidates=True).values():
            if lesson.get("status") == "candidate":
                print(lesson["id"])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
