#!/usr/bin/env python3
"""Adversarial final-artifact tests for the capability verifier."""

from __future__ import annotations

import sys

import capability_verify as V
from test_capability_lessons import MEASUREMENTS, ROLES, inventory, lesson


def module(mid, plugin, model, params=None):
    return {"id": mid, "plugin": plugin, "model": model,
            "params": params or []}


def cable(cid, source, output, target, into):
    return {"id": cid, "outputModuleId": source, "outputId": output,
            "inputModuleId": target, "inputId": into}


def fixture():
    spec = lesson()
    spec["controls"][0]["target"]["value"] = 2000.0
    patch = {"modules": [
        module(1, "Installed", "Steps"),
        module(2, "Installed", "Tone", [{"id": 7, "value": 1.0}]),
        module(3, "Installed", "Sink"),
    ], "cables": [
        cable(1, 1, 0, 2, 1),
        cable(2, 2, 0, 3, 0),
    ]}
    observations = {"pitch": {"distinct_pitches": 4}}
    return spec, patch, inventory(), observations


def verdicts(result):
    return {row["id"]: row["verdict"] for row in result["results"]}


def check(condition, message):
    if condition:
        print("  ok    " + message)
        return 0
    print("  WRONG " + message)
    return 1


def test_good_artifact_passes_without_a_plan():
    spec, patch, inv, observations = fixture()
    got = V.verify(spec, patch, inv, observations, roles=ROLES)
    return check(got["verdict"] == V.PASS and got["search_mapping"] == {
                     "pattern": 1, "colour": 2, "sink": 3},
                 "the final patch reconstructs and passes without planner annotations")


def test_cut_cable_fails():
    spec, patch, inv, observations = fixture()
    patch["cables"] = patch["cables"][1:]
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["topology"] == V.FAIL and got["moves"] == V.FAIL,
                 "cutting a required cable fails reconstructed topology")


def test_rewire_to_wrong_jack_fails():
    spec, patch, inv, observations = fixture()
    patch["cables"][0]["inputId"] = 0       # audio input, not CV
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["topology"] == V.FAIL and got["moves"] == V.FAIL,
                 "rewiring to a known wrong jack fails")


def test_rewire_beyond_known_port_range_fails():
    spec, patch, inv, observations = fixture()
    patch["cables"][0]["inputId"] = 999
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["moves"] == V.FAIL,
                 "an out-of-range jack on a mapped module is FAIL, not unknown")


def test_zeroed_physical_control_fails():
    spec, patch, inv, observations = fixture()
    patch["modules"][1]["params"][0]["value"] = 0.0
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["control:cutoff"] == V.FAIL,
                 "zeroing a required non-zero physical target fails")


def test_omitted_control_uses_known_default():
    spec, patch, inv, observations = fixture()
    spec["controls"][0]["target"]["value"] = 1000.0
    inv["Installed"]["modules"]["Tone"]["params"][0]["defaultValue"] = 0.0
    patch["modules"][1]["params"] = []
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["control:cutoff"] == V.PASS,
                 "an omitted parameter is verified at its known default")


def test_shared_lane_fails():
    spec, patch, inv, observations = fixture()
    spec["relations"].insert(1, {
        "id": "second-motion", "from": "pattern", "out": "cv_out",
        "to": "colour", "in": "cv_in",
    })
    spec["assertions"].insert(1, {
        "id": "lanes", "kind": "distinct_source_ports",
        "relations": ["motion", "second-motion"],
    })
    # A second destination jack but the same source lane.
    inv["Installed"]["modules"]["Tone"]["inputs"].append("Second CV")
    inv["Installed"]["modules"]["Tone"]["roles_in"].append("Cv")
    patch["cables"].insert(1, cable(3, 1, 0, 2, 2))
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["lanes"] == V.FAIL and got["topology"] == V.FAIL,
                 "copying one source output into two claimed lanes fails")


def test_shared_destination_jack_fails():
    spec, patch, inv, observations = fixture()
    spec["relations"].insert(1, {
        "id": "second-motion", "from": "pattern", "out": "cv_out",
        "to": "colour", "in": "cv_in",
    })
    spec["assertions"].insert(1, {
        "id": "second-present", "kind": "relation_present",
        "relation": "second-motion",
    })
    # Two cable records are not two usable connections when both occupy the
    # same destination jack.
    patch["cables"].insert(1, cable(3, 1, 1, 2, 1))
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["topology"] == V.FAIL,
                 "distinct cable records cannot share one destination jack")


def test_wrong_instance_cannot_satisfy_a_split_graph():
    spec, patch, inv, observations = fixture()
    patch["modules"].append(module(4, "Installed", "Tone",
                                   [{"id": 7, "value": 1.0}]))
    # Modulation reaches one filter; audible output comes from the other.
    patch["cables"][0]["inputModuleId"] = 4
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["moves"] == V.PASS and got["audible"] == V.PASS
                 and got["topology"] == V.FAIL,
                 "relations on different instances cannot masquerade as one graph")


def test_missing_port_map_is_unmeasured():
    spec, patch, inv, observations = fixture()
    inv["Installed"]["modules"]["Tone"].pop("roles_in")
    inv["Installed"]["modules"]["Tone"].pop("inputs")
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES))
    return check(got["moves"] == V.UNMEASURED,
                 "an absent port map is UNMEASURED, never pass or fail")


def test_metrics_and_probes_are_closed_and_tristate():
    spec, patch, inv, _ = fixture()
    got = verdicts(V.verify(spec, patch, inv, {}, roles=ROLES))
    bad = check(got["melodic"] == V.UNMEASURED,
                "an absent audio observation is UNMEASURED")
    spec["assertions"].append({"id": "mystery", "kind": "probe",
                               "probe": "unregistered", "contract": "x"})
    try:
        V.verify(spec, patch, inv, {}, roles=ROLES)
    except Exception as exc:
        bad += check("unregistered probe" in str(exc),
                     "an unregistered probe is rejected before evaluation")
    else:
        bad += check(False, "an unregistered probe is rejected before evaluation")
    return bad


def test_optional_control_does_not_block_pass():
    spec, patch, inv, observations = fixture()
    spec["controls"][0]["required"] = False
    patch["modules"][1]["params"] = []
    got = V.verify(spec, patch, inv, observations, roles=ROLES)
    rows = verdicts(got)
    return check(rows["control:cutoff"] == V.UNMEASURED
                 and got["verdict"] == V.PASS,
                 "an optional unmeasured control is reported but non-blocking")


def test_required_control_selects_the_right_complete_mapping():
    spec, patch, inv, observations = fixture()
    # The first complete filter/sink pair has the wrong value; the second has
    # the right one. Verification must search mappings, not trust module order.
    patch["modules"][1]["params"][0]["value"] = 0.0
    patch["modules"] += [
        module(4, "Installed", "Tone", [{"id": 7, "value": 1.0}]),
        module(5, "Installed", "Sink"),
    ]
    patch["cables"] += [cable(3, 1, 1, 4, 1), cable(4, 4, 0, 5, 0)]
    got = V.verify(spec, patch, inv, observations, roles=ROLES)
    return check(got["verdict"] == V.PASS
                 and got["search_mapping"]["colour"] == 4,
                 "required controls participate in final mapping selection")


def test_search_budget_fails_honestly():
    spec, patch, inv, observations = fixture()
    got = verdicts(V.verify(spec, patch, inv, observations, roles=ROLES,
                            max_states=1))
    return check(got["topology"] == V.UNMEASURED,
                 "a bounded verifier says UNMEASURED when its budget expires")


def test_control_mapping_budget_fails_honestly():
    spec, patch, inv, observations = fixture()
    patch["modules"][1]["params"][0]["value"] = 0.0
    patch["modules"] += [
        module(4, "Installed", "Tone", [{"id": 7, "value": 1.0}]),
        module(5, "Installed", "Sink"),
    ]
    patch["cables"] += [cable(3, 1, 1, 4, 1), cable(4, 4, 0, 5, 0)]
    got = V.verify(spec, patch, inv, observations, roles=ROLES, max_states=12)
    rows = verdicts(got)
    return check(rows["topology"] == V.PASS and
                 rows["control:cutoff"] == V.UNMEASURED and
                 got["verdict"] == V.UNMEASURED,
                 "an exhausted control-aware remap cannot become false FAIL")


def main():
    bad = 0
    for fn in (test_good_artifact_passes_without_a_plan,
               test_cut_cable_fails,
               test_rewire_to_wrong_jack_fails,
               test_rewire_beyond_known_port_range_fails,
               test_zeroed_physical_control_fails,
               test_omitted_control_uses_known_default,
               test_shared_lane_fails,
               test_shared_destination_jack_fails,
               test_wrong_instance_cannot_satisfy_a_split_graph,
               test_missing_port_map_is_unmeasured,
               test_metrics_and_probes_are_closed_and_tristate,
               test_optional_control_does_not_block_pass,
               test_required_control_selects_the_right_complete_mapping,
               test_search_budget_fails_honestly,
               test_control_mapping_budget_fails_honestly):
        print(fn.__name__ + ":")
        bad += fn()
    print("\n" + ("all good" if bad == 0 else f"FAILED ({bad})"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
