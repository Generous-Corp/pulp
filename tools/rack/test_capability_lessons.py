#!/usr/bin/env python3
"""Executable contract for capability-level patch lessons."""

from __future__ import annotations

import copy
import sys

import capability_lessons as C
import patch


ROLES = {
    "roles": {
        "sequencer": {"tags": ["Sequencer"]},
        "filter": {"tags": ["Filter"]},
        "output": {"tags": ["External"]},
    },
    "ports": {
        "cv_out": {"ports": ["Cv"], "labels": ["CV"]},
        "audio_out": {"ports": ["Audio"], "labels": ["OUT"]},
        "cv_in": {"ports": ["Cv"], "labels": ["CV"]},
        "audio_in": {"ports": ["Audio"], "labels": ["IN"]},
    },
}
MEASUREMENTS = {"pitch.distinct_pitches": {}}


def lesson(status="admitted"):
    return {
        "id": "fixture", "status": status, "independent_holdout": True,
        "intent": ["fixture semantic goal"],
        "traits": ["control"],
        "why": "A control source changes a sound-shaping stage before the audible output.",
        "behavior": ["melodic"],
        "nodes": [
            {"id": "pattern", "role": "sequencer"},
            {"id": "colour", "role": "filter"},
            {"id": "sink", "role": "output"},
        ],
        "relations": [
            {"id": "motion", "from": "pattern", "out": "cv_out",
             "to": "colour", "in": "cv_in"},
            {"id": "heard", "from": "colour", "out": "audio_out",
             "to": "sink", "in": "audio_in"},
        ],
        "controls": [
            {"id": "cutoff", "node": "colour", "affordance": "timbre",
             "target": {"value": 1000.0, "unit": "Hz"}, "required": True},
        ],
        "assertions": [
            {"id": "moves", "kind": "relation_present", "relation": "motion"},
            {"id": "audible", "kind": "relation_present", "relation": "heard"},
            {"id": "melodic", "kind": "metric",
             "field": "pitch.distinct_pitches", "op": ">=", "value": 3},
        ],
        "mutations": [
            {"id": "cut-motion", "operation": "remove_relation",
             "relation": "motion", "must_fail": ["moves"]},
        ],
    }


def inventory():
    return {
        "Installed": {"modules": {
            "Steps": {"tags": ["Sequencer"], "outputs": ["A", "B"],
                      "roles_out": ["Cv", "Cv"]},
            "Tone": {"tags": ["Filter"], "inputs": ["Signal", "Cutoff"],
                     "roles_in": ["Audio", "Cv"], "outputs": ["Out"],
                     "roles_out": ["Audio"], "params": [{
                         "id": 7, "name": "Frequency", "affords": "timbre",
                         "affordance_confidence": "known", "minValue": -5.0,
                         "maxValue": 5.0, "unit": " Hz", "displayBase": 2.0,
                         "displayMultiplier": 1000.0, "displayOffset": 0.0,
                     }]},
            "Sink": {"tags": ["External"], "inputs": ["In"],
                     "roles_in": ["Audio"]},
        }}
    }


def check(condition, message):
    if condition:
        print("  ok    " + message)
        return 0
    print("  WRONG " + message)
    return 1


def test_schema_and_quarantine():
    bad = 0
    got = C.problems({"fixture": lesson()}, roles=ROLES,
                     measurements=MEASUREMENTS)
    bad += check(not got, "a complete source-neutral lesson validates")
    branded = lesson()
    branded["nodes"][0]["plugin"] = "ConcreteThing"
    got = C.problems({"fixture": branded}, roles=ROLES,
                     measurements=MEASUREMENTS)
    bad += check(any("concrete identity" in p for p in got),
                 "concrete identity is rejected from the durable IR")
    invented = lesson()
    invented["origin_vendor"] = "ConcreteThing"
    got = C.problems({"fixture": invented}, roles=ROLES,
                     measurements=MEASUREMENTS)
    bad += check(any("unknown lesson fields" in p for p in got),
                 "the durable schema is closed to invented provenance fields")
    malformed = lesson()
    malformed["nodes"].append("not an object")
    got = C.problems({"fixture": malformed}, roles=ROLES,
                     measurements=MEASUREMENTS)
    bad += check(any("must be an object" in p for p in got),
                 "malformed learned rows fail closed instead of crashing")
    id_fields = (("nodes", 0), ("relations", 0), ("controls", 0),
                 ("assertions", 0), ("mutations", 0))
    for field, index in id_fields:
        malformed_id = lesson()
        malformed_id[field][index]["id"] = []
        got = C.problems({"fixture": malformed_id}, roles=ROLES,
                         measurements=MEASUREMENTS)
        bad += check(any(f"{field[:-1]} ids" in p or
                         (field == "nodes" and "capability node ids" in p)
                         for p in got),
                     f"an unhashable {field[:-1]} id fails closed")
    isolated = lesson()
    isolated["nodes"].append({"id": "unused", "role": "filter"})
    got = C.problems({"fixture": isolated}, roles=ROLES,
                     measurements=MEASUREMENTS)
    bad += check(any("isolated capability" in p for p in got),
                 "every declared capability must be independently verifiable")
    candidate = lesson("candidate")
    try:
        C.compile_lesson(candidate, inventory(), {}, {}, roles=ROLES)
    except C.LessonError as exc:
        bad += check("quarantined" in str(exc),
                     "candidate lessons cannot enter generation")
    else:
        bad += check(False, "candidate lessons cannot enter generation")
    quarantined = lesson("quarantined")
    try:
        C.compile_lesson(quarantined, inventory(), {}, {}, allow_candidate=True,
                         roles=ROLES)
    except C.LessonError as exc:
        bad += check("quarantined" in str(exc),
                     "a candidate override never admits quarantined lessons")
    else:
        bad += check(False, "a candidate override never admits quarantined lessons")
    dishonest = lesson()
    dishonest["mutations"][0]["must_fail"] = ["audible"]
    got = C.problems({"fixture": dishonest}, roles=ROLES,
                     measurements=MEASUREMENTS)
    bad += check(any("every must_fail" in p for p in got),
                 "negative controls cannot claim to break unrelated assertions")
    return bad


def test_eligibility_and_exact_transform():
    bad = 0
    midx = {
        "Owned": {"StepsPro": {"tags": ["Sequencer"]}},
        "Gratis": {"CheapFilter": {"tags": ["Filter"]}},
        "Paywall": {"FancySink": {"tags": ["External"]}},
    }
    catalog = {
        "Owned": {"premium": True, "arches": [patch.rack_arch()]},
        "Gratis": {"premium": False, "arches": [patch.rack_arch()]},
        "Paywall": {"premium": True, "arches": [patch.rack_arch()]},
    }
    plan = C.compile_lesson(lesson(), inventory(), midx, catalog,
                            owned={"Owned"}, roles=ROLES)
    bad += check(plan["state"] == "ready", "installed realisations compile ready")
    chosen = [n["realisation"]["availability"] for n in plan["nodes"].values()]
    bad += check(chosen == ["installed", "installed", "installed"],
                 "installed outranks owned and free alternatives")
    alternatives = plan["nodes"]["pattern"]["alternatives"]
    bad += check(alternatives and alternatives[0]["availability"] == "owned",
                 "owned modules remain eligible replacements")
    all_options = [n["realisation"] for n in plan["nodes"].values()]
    all_options += [a for n in plan["nodes"].values()
                    for a in n["alternatives"]]
    bad += check(not any(o["plugin"] == "Paywall" for o in all_options),
                 "premium unowned modules are never candidates")
    control = plan["controls"][0]
    bad += check(control["raw_value"] == 0.0
                 and control["display_value"] == 1000.0
                 and control["transform"]["round_trip_exact"],
                 "physical target compiles through the exact display transform")
    bad += check("exact-display-transform" in control["provenance"],
                 "the transform carries confidence and provenance")
    bad += check(plan["deferred_assertions"] == ["melodic"],
                 "audio assertions stay explicit and unevaluated in a build plan")
    return bad


def test_free_plan_waits_for_install_and_scan():
    inv = {"Installed": {"modules": {
        "Steps": inventory()["Installed"]["modules"]["Steps"],
        "Sink": inventory()["Installed"]["modules"]["Sink"],
    }}}
    midx = {"Gratis": {"Tone": {"tags": ["Filter"]}}}
    catalog = {"Gratis": {"premium": False,
                           "arches": [patch.rack_arch()]}}
    plan = C.compile_lesson(lesson(), inv, midx, catalog, roles=ROLES)
    bad = check(plan["nodes"]["colour"]["realisation"]["availability"] == "free",
                "a free-downloadable module is eligible")
    bad += check(plan["state"] == "needs_acquisition",
                 "but no cable/control is claimed before install and scan")
    return bad


def test_unknown_price_is_not_assumed_free():
    inv = {"Installed": {"modules": {
        "Steps": inventory()["Installed"]["modules"]["Steps"],
        "Sink": inventory()["Installed"]["modules"]["Sink"],
    }}}
    # The module index can know a name/tag before the publication catalog has
    # established whether it may be downloaded.  That gap must fail closed.
    midx = {"Mystery": {"Tone": {"tags": ["Filter"]}}}
    plan = C.compile_lesson(lesson(), inv, midx, {}, roles=ROLES)
    return check("colour" not in plan["nodes"] and plan["state"] == "unresolved",
                 "unknown commercial status is never treated as free")


def test_unavailable_build_is_not_assumed_downloadable():
    inv = {"Installed": {"modules": {
        "Steps": inventory()["Installed"]["modules"]["Steps"],
        "Sink": inventory()["Installed"]["modules"]["Sink"],
    }}}
    midx = {"WrongArch": {"Tone": {"tags": ["Filter"]}}}
    catalog = {"WrongArch": {"premium": False,
                              "arches": ["definitely-not-this-machine"]}}
    plan = C.compile_lesson(lesson(), inv, midx, catalog, roles=ROLES)
    return check("colour" not in plan["nodes"] and plan["state"] == "unresolved",
                 "free without a compatible build is not downloadable here")


def test_resolver_skips_a_known_incapable_installed_module():
    inv = inventory()
    inv["Earlier"] = {"modules": {"WrongTone": {
        "tags": ["Filter"], "inputs": ["Signal", "Cutoff"],
        "roles_in": ["Audio", "Cv"], "outputs": ["Out"],
        "roles_out": ["Audio"], "params": [],
    }}}
    plan = C.compile_lesson(lesson(), inv, {}, {}, roles=ROLES)
    return check(plan["nodes"]["colour"]["realisation"]["model"] == "Tone",
                 "required semantic controls participate in module selection")


def test_control_resolution_skips_disallowed_clamping():
    inv = inventory()
    exact = copy.deepcopy(inv["Installed"]["modules"]["Tone"]["params"][0])
    exact.update({"id": 8, "affordance_confidence": "inferred"})
    clamped = copy.deepcopy(exact)
    clamped.update({"id": 7, "affordance_confidence": "known",
                    "maxValue": -1.0})
    inv["Installed"]["modules"]["Tone"]["params"] = [clamped, exact]
    plan = C.compile_lesson(lesson(), inv, {}, {}, roles=ROLES)
    return check(plan["state"] == "ready" and
                 plan["controls"][0]["parameter"]["id"] == 8,
                 "an exact inferred transform outranks disallowed clamping")


def test_mutations_break_the_named_assertion():
    plan = C.compile_lesson(lesson(), inventory(), {}, {}, roles=ROLES)
    before = C.evaluate_static(plan)
    changed = C.mutate_plan(plan, lesson()["mutations"][0])
    after = C.evaluate_static(changed)
    bad = check(before["moves"], "the unmutated relation assertion passes")
    bad += check(not after["moves"] and after["audible"],
                 "its negative control fails only the named structural fact")
    return bad


def test_distinct_lane_assignment_and_negative_control():
    spec = lesson()
    spec["relations"].insert(1, {
        "id": "second-motion", "from": "pattern", "out": "cv_out",
        "to": "colour", "in": "cv_in",
    })
    spec["assertions"].insert(1, {
        "id": "lanes", "kind": "distinct_source_ports",
        "relations": ["motion", "second-motion"],
    })
    spec["mutations"].append({
        "id": "collapse", "operation": "collapse_source_ports",
        "relations": ["motion", "second-motion"], "must_fail": ["lanes"],
    })
    inv = inventory()
    inv["Installed"]["modules"]["Tone"]["inputs"].append("Second CV")
    inv["Installed"]["modules"]["Tone"]["roles_in"].append("Cv")
    plan = C.compile_lesson(spec, inv, {}, {}, roles=ROLES)
    before = C.evaluate_static(plan)
    changed = C.mutate_plan(plan, spec["mutations"][-1])
    after = C.evaluate_static(changed)
    bad = check(before["lanes"], "distinct lane assertion gets distinct jacks")
    bad += check(not after["lanes"], "collapsing lanes is a failing mutation")
    return bad


def test_destination_jacks_are_allocated_once():
    spec = lesson()
    spec["relations"].insert(1, {
        "id": "second-motion", "from": "pattern", "out": "cv_out",
        "to": "colour", "in": "cv_in",
    })
    inv = inventory()
    # Only one compatible CV input: a physically impossible fan-in.
    plan = C.compile_lesson(spec, inv, {}, {}, roles=ROLES)
    bad = check(plan["state"] == "unresolved"
                and any("enough ports" in issue for issue in plan["issues"]),
                "two cables can never share one Rack input jack")
    inv["Installed"]["modules"]["Tone"]["inputs"].append("Second CV")
    inv["Installed"]["modules"]["Tone"]["roles_in"].append("Cv")
    plan = C.compile_lesson(spec, inv, {}, {}, roles=ROLES)
    inputs = [r["input"]["index"] for r in plan["relations"]
              if r["to"] == "colour"]
    bad += check(plan["state"] == "ready" and len(inputs) == len(set(inputs)),
                 "a multi-input destination gets distinct physical jacks")
    return bad


def test_distinct_outputs_are_scoped_to_each_source():
    spec = lesson()
    spec["nodes"].insert(1, {"id": "pattern_b", "role": "sequencer"})
    spec["relations"].insert(1, {
        "id": "second-motion", "from": "pattern_b", "out": "cv_out",
        "to": "colour", "in": "cv_in",
    })
    spec["assertions"].insert(1, {
        "id": "two-sources", "kind": "distinct_source_ports",
        "relations": ["motion", "second-motion"],
    })
    inv = inventory()
    inv["Installed"]["modules"]["Tone"]["inputs"].append("Second CV")
    inv["Installed"]["modules"]["Tone"]["roles_in"].append("Cv")
    # Both sequencer instances may use their own output zero.
    inv["Installed"]["modules"]["Steps"]["outputs"] = ["A"]
    inv["Installed"]["modules"]["Steps"]["roles_out"] = ["Cv"]
    plan = C.compile_lesson(spec, inv, {}, {}, roles=ROLES)
    return check(plan["state"] == "ready",
                 "port zero on two source instances is two physical lanes")


def test_overlapping_distinct_groups_are_solved_together():
    spec = lesson()
    for name in ("second-motion", "third-motion"):
        spec["relations"].insert(1, {
            "id": name, "from": "pattern", "out": "cv_out",
            "to": "colour", "in": "cv_in",
        })
    spec["assertions"][1:1] = [
        {"id": "first-pair", "kind": "distinct_source_ports",
         "relations": ["motion", "second-motion"]},
        {"id": "second-pair", "kind": "distinct_source_ports",
         "relations": ["second-motion", "third-motion"]},
    ]
    inv = inventory()
    inv["Installed"]["modules"]["Steps"]["outputs"].append("C")
    inv["Installed"]["modules"]["Steps"]["roles_out"].append("Cv")
    inv["Installed"]["modules"]["Tone"]["inputs"] += ["Second CV", "Third CV"]
    inv["Installed"]["modules"]["Tone"]["roles_in"] += ["Cv", "Cv"]
    plan = C.compile_lesson(spec, inv, {}, {}, roles=ROLES)
    return check(plan["state"] == "ready"
                 and all(plan["static_assertions"][aid]
                         for aid in ("first-pair", "second-pair")),
                 "overlapping lane constraints receive one global allocation")


def test_module_choice_backtracks_for_port_capacity():
    spec = lesson()
    spec["relations"].insert(1, {
        "id": "second-motion", "from": "pattern", "out": "cv_out",
        "to": "colour", "in": "cv_in",
    })
    inv = inventory()
    inv["Installed"]["modules"]["Tone"]["inputs"].append("Second CV")
    inv["Installed"]["modules"]["Tone"]["roles_in"].append("Cv")
    inv["Earlier"] = {"modules": {"TooSmall": {
        "tags": ["Filter"], "inputs": ["Signal", "Cutoff"],
        "roles_in": ["Audio", "Cv"], "outputs": ["Out"],
        "roles_out": ["Audio"],
        "params": copy.deepcopy(inv["Installed"]["modules"]["Tone"]["params"]),
    }}}
    plan = C.compile_lesson(spec, inv, {}, {}, roles=ROLES)
    return check(plan["state"] == "ready" and
                 plan["nodes"]["colour"]["realisation"]["model"] == "Tone",
                 "module selection skips a higher-ranked port-incapable option")


def test_one_intent_preserves_multiple_blueprints():
    first = lesson()
    second = copy.deepcopy(first)
    second["id"] = "fixture-alternative"
    entries = {first["id"]: first, second["id"]: second}
    plans = C.compile_intent("fixture semantic goal", inventory(), {}, {},
                             entries=entries, roles=ROLES)
    return check([plan["lesson_id"] for plan in plans] ==
                 ["fixture", "fixture-alternative"],
                 "one intent keeps every valid blueprint instead of one graph")


def test_shipped_lessons_preserve_admission_boundary():
    all_lessons = C.load(include_candidates=True)
    admitted = C.load()
    bad = check(len(all_lessons) >= 2, "bounded example lessons load")
    bad += check(set(admitted) == {"accented-sliding-sequence"},
                 "only the real-Rack acid holdout enters generation")
    bad += check("layered-continuous-tone" not in admitted,
                 "the unproven candidate stays absent from generation")
    got = C.problems(all_lessons)
    bad += check(not got, "all shipped lesson schemas and mutations validate")
    return bad


def main():
    bad = 0
    for fn in (test_schema_and_quarantine,
               test_eligibility_and_exact_transform,
               test_free_plan_waits_for_install_and_scan,
               test_unknown_price_is_not_assumed_free,
               test_unavailable_build_is_not_assumed_downloadable,
               test_resolver_skips_a_known_incapable_installed_module,
               test_control_resolution_skips_disallowed_clamping,
               test_mutations_break_the_named_assertion,
               test_distinct_lane_assignment_and_negative_control,
               test_destination_jacks_are_allocated_once,
               test_distinct_outputs_are_scoped_to_each_source,
               test_overlapping_distinct_groups_are_solved_together,
               test_module_choice_backtracks_for_port_capacity,
               test_one_intent_preserves_multiple_blueprints,
               test_shipped_lessons_preserve_admission_boundary):
        print(fn.__name__ + ":")
        bad += fn()
    print("\n" + ("all good" if bad == 0 else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
