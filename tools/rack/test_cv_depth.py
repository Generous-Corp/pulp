#!/usr/bin/env python3
"""A cable into a CV input carries nothing while its depth control is zero.

Run:  python3 tools/rack/test_cv_depth.py

Hermetic: every case builds its own inventory, so nothing here depends on
which modules happen to be installed.
"""
import copy
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cv_depth as CD                                    # noqa: E402
import patch as P                                        # noqa: E402


def param(pid, name, lo, hi, default):
    return {"id": pid, "name": name, "minValue": lo, "maxValue": hi,
            "defaultValue": default, "min": lo, "max": hi, "default": default}


def inventory() -> dict:
    """Four modules, each isolating one behaviour of the association rules."""
    return {
        # One CV input, one depth control, names that do NOT cover each other.
        # Only the sole-candidate rule can reach this.
        "Toy": {"name": "Toy", "version": "1.0", "modules": {
            "VCO": {"name": "VCO", "tags": ["Oscillator"],
                    "inputs": ["1V/oct pitch", "Frequency modulation"],
                    "roles_in": ["Pitch", "Cv"],
                    "outputs": ["Sine"], "roles_out": ["Audio"],
                    "params": [param(0, "Frequency", -4, 4, 0),
                               param(3, "FM amount", -1, 1, 0)]},
            # Two CV inputs and two depth controls whose names DO cover them:
            # the stem rule resolves both, uniquely.
            "FOLD": {"name": "FOLD", "tags": ["Waveshaper"],
                     "inputs": ["Drive CV", "Symmetry CV", "Audio in"],
                     "roles_in": ["Cv", "Cv", "Audio"],
                     "outputs": ["Folded"], "roles_out": ["Audio"],
                     "params": [param(0, "Drive", 0, 1, 0.25),
                                param(2, "Drive CV amount", -1, 1, 0),
                                param(3, "Symmetry CV amount", -1, 1, 0)]},
            # Two CV inputs, two depth controls, names that cover NEITHER.
            # Every rule must abstain rather than guess a pairing.
            "AMBIG": {"name": "AMBIG", "tags": ["Effect"],
                      "inputs": ["Alpha", "Beta"], "roles_in": ["Cv", "Cv"],
                      "outputs": ["Out"], "roles_out": ["Audio"],
                      "params": [param(0, "Gamma CV amount", -1, 1, 0),
                                 param(1, "Delta CV amount", -1, 1, 0)]},
            # One CV input matched by TWO depth controls: the word rules
            # must abstain rather than take the first.
            "TWOWAY": {"name": "TWOWAY", "tags": ["Filter"],
                       "inputs": ["Cutoff"], "roles_in": ["Cv"],
                       "outputs": ["Out"], "roles_out": ["Audio"],
                       "params": [param(0, "Cutoff CV amount", -1, 1, 0),
                                  param(1, "Cutoff depth", -1, 1, 0)]},
            # No depth control at all: the question is not live here.
            "MULT": {"name": "MULT", "tags": ["Multiple"],
                     "inputs": ["Signal"], "roles_in": ["Cv"],
                     "outputs": ["A"], "roles_out": ["Cv"], "params": []},
        }},
        # Carries a witness receipt in the shipped table.
        "Fundamental": {"name": "Fundamental", "version": "2.0",
                        "modules": {"VCF": {
                            "name": "VCF", "tags": ["Filter"],
                            "inputs": ["Freq", "Res", "Drive", "Audio"],
                            "roles_in": ["Cv", "Cv", "Cv", "Audio"],
                            "outputs": ["Low-pass"], "roles_out": ["Audio"],
                            "params": [param(0, "Freq", 0, 1, 0.5),
                                       param(3, "Freq CV", -1, 1, 0),
                                       param(5, "Res CV", -1, 1, 0),
                                       param(6, "Drive CV", -1, 1, 0)]}}},
    }


def patch_of(modules, cables) -> dict:
    return {"version": "2.6.6", "modules": modules, "cables": cables}


def mod(mid, plugin, model, params=None, pos=(0, 0)):
    m = {"id": mid, "plugin": plugin, "model": model, "pos": list(pos)}
    if params is not None:
        m["params"] = params
    return m


def cable(cid, om, oi, im, ii):
    return {"id": cid, "outputModuleId": om, "outputId": oi,
            "inputModuleId": im, "inputId": ii}


def values(module) -> dict:
    return {p["id"]: p["value"] for p in module.get("params") or []}


# ── the generation-side rule ────────────────────────────────────────────────

def test_the_rule_opens_a_depth_control_the_patch_never_set(inv):
    """The fix: patch a CV input, and its depth control comes up nonzero."""
    pch = patch_of([mod(1, "Toy", "VCO"), mod(2, "Toy", "MULT")],
                   [cable(1, 2, 0, 1, 1)])
    notes = CD.open_depth_controls(pch, inv)
    got = values(pch["modules"][0]).get(3)
    ok = got is not None and got != 0
    return [("the rule opens an unset depth control on a cabled CV input",
             ok, f"param 3 is {got!r}")] + [
        ("opening a control is reported", bool(notes), f"notes={notes}")]


def test_the_rule_never_overrides_an_authored_value(inv):
    """Nonzero by default, not nonzero by force.

    A value the model or the prompt chose is the author speaking. A
    deliberate zero is the hard case, and it is the one that must survive:
    the rule may only fill a control the patch never mentioned.
    """
    out = []
    for label, authored in (("a deliberate zero", 0.0),
                            ("a chosen nonzero", 0.25)):
        pch = patch_of([mod(1, "Toy", "VCO", [{"id": 3, "value": authored}]),
                        mod(2, "Toy", "MULT")], [cable(1, 2, 0, 1, 1)])
        CD.open_depth_controls(pch, inv)
        got = values(pch["modules"][0]).get(3)
        out.append((f"the rule leaves {label} exactly as authored",
                    got == authored, f"authored {authored}, now {got!r}"))
    return out


def test_a_physical_target_counts_as_the_author_speaking(inv):
    """A control named as a physical target is authored, even before placing.

    `place_physical_targets` converts `{"id": 3, "physical": ..., "unit": ...}`
    into a value, and returns WITHOUT mutating anything if any target in the
    patch fails to place. Reading only values would then see this control as
    unmentioned: the rule would append a second entry for the same id, and the
    patch would come back with a duplicate-param error stacked on the real
    one — sending the next attempt after a fault that is not there.
    """
    pch = patch_of([mod(1, "Toy", "VCO",
                        [{"id": 3, "physical": 0.4, "unit": "x"}]),
                    mod(2, "Toy", "MULT")], [cable(1, 2, 0, 1, 1)])
    CD.open_depth_controls(pch, inv)
    ids = [p["id"] for p in pch["modules"][0]["params"]]
    return [("the rule does not add a second entry for the same param",
             len(ids) == len(set(ids)), str(pch["modules"][0]["params"])),
            ("the transient target survives untouched",
             pch["modules"][0]["params"] == [{"id": 3, "physical": 0.4,
                                              "unit": "x"}],
             str(pch["modules"][0]["params"])),
            ("and an unresolved control is not judged dead",
             CD.findings(pch, inv) == [], str(CD.findings(pch, inv)))]


def test_running_the_rule_twice_changes_nothing(inv):
    """`prepare_and_lint` runs again after a deterministic repair."""
    pch = patch_of([mod(1, "Toy", "VCO"), mod(2, "Toy", "MULT")],
                   [cable(1, 2, 0, 1, 1)])
    CD.open_depth_controls(pch, inv)
    once = copy.deepcopy(pch["modules"][0]["params"])
    CD.open_depth_controls(pch, inv)
    return [("a second run is a no-op",
             pch["modules"][0]["params"] == once,
             f"{once} then {pch['modules'][0]['params']}")]


def test_the_rule_ignores_an_input_with_no_cable(inv):
    """Opening a control nothing is patched into would be noise, not a fix."""
    pch = patch_of([mod(1, "Toy", "VCO")], [])
    CD.open_depth_controls(pch, inv)
    return [("an uncabled CV input keeps its default",
             3 not in values(pch["modules"][0]),
             f"params={pch['modules'][0].get('params')}")]


def test_the_rule_leaves_a_patch_it_cannot_evaluate_alone(inv):
    """No association means no action. A guess must not edit a patch either."""
    pch = patch_of([mod(1, "Toy", "AMBIG"), mod(2, "Toy", "MULT")],
                   [cable(1, 2, 0, 1, 0)])
    CD.open_depth_controls(pch, inv)
    return [("an ambiguous module is left untouched",
             not values(pch["modules"][0]),
             f"params={pch['modules'][0].get('params')}")]


def test_the_rule_fixes_what_the_check_reports(inv):
    """The two halves must agree: after the fix, the finding is gone."""
    pch = patch_of([mod(1, "Toy", "VCO"), mod(2, "Toy", "MULT")],
                   [cable(1, 2, 0, 1, 1)])
    before = CD.findings(pch, inv)
    CD.open_depth_controls(pch, inv)
    after = CD.findings(pch, inv)
    return [("the check reports a dead edge before the rule runs",
             any(f["check"] == "dead-cv-edge" for f in before), str(before)),
            ("and reports none after it", not [f for f in after
                                               if f["check"] == "dead-cv-edge"],
             str(after))]


# ── grading: what may and may not reject a patch ────────────────────────────

def test_a_nominated_association_can_never_block(inv):
    """Structural, not policy.

    `Nominated` carries no field that reaches `blocking`. This asserts the
    property directly, at confidences well past anything the rules emit, and
    then over every association the rules actually produce.
    """
    out = []
    for confidence in (0.0, 0.5, 0.85, 1.0, 99.0):
        n = CD.Nominated(param=0, depth_param_default=0.0, rule="r",
                         confidence=confidence)
        out.append((f"Nominated(confidence={confidence}) does not block",
                    n.blocking is False, f"blocking={n.blocking}"))
    bad = []
    for plugin, rec in inv.items():
        for model in rec["modules"]:
            for idx, a in CD.associations(inv, plugin, model).items():
                if a.blocking and a.evidence != CD.MEASURED:
                    bad.append(f"{plugin}/{model} in:{idx} {a}")
    out.append(("no association blocks without a witness", not bad, str(bad)))
    return out


def test_a_witnessed_association_fails_the_patch(inv):
    """The one thing that may reject: a receipt that rendered audio."""
    pch = patch_of([mod(1, "Fundamental", "VCF"), mod(2, "Toy", "MULT")],
                   [cable(1, 2, 0, 1, 0)])
    fs = CD.findings(pch, inv)
    errs = CD.dead_edge_errors(pch, inv)
    return [("a measured blocking association yields FAIL",
             any(f["verdict"] == CD.FAIL for f in fs), str(fs)),
            ("and reaches the linter's error list", len(errs) == 1, str(errs))]


def test_a_nominated_association_only_advises(inv):
    """It is reported to the reader and kept out of the error list."""
    pch = patch_of([mod(1, "Toy", "VCO"), mod(2, "Toy", "MULT")],
                   [cable(1, 2, 0, 1, 1)])
    fs = CD.findings(pch, inv)
    return [("a nominated dead edge is ADVISORY",
             [f["verdict"] for f in fs if f["check"] == "dead-cv-edge"]
             == [CD.ADVISORY], str(fs)),
            ("it never reaches the error list",
             CD.dead_edge_errors(pch, inv) == [], str(fs)),
            ("it does reach the reader",
             len(CD.advisory_notes(pch, inv)) == 1, str(fs))]


def test_an_unevaluated_edge_does_not_reject_the_patch(inv):
    """A gap in our coverage must not punish the user.

    AMBIG has depth controls, so the question is live, but no rule can say
    which control governs which input. That is UNEVALUATED — distinct from
    'no depth control here', and distinct from a finding.
    """
    pch = patch_of([mod(1, "Toy", "AMBIG"), mod(2, "Toy", "MULT")],
                   [cable(1, 2, 0, 1, 0)])
    fs = CD.findings(pch, inv)
    quiet = patch_of([mod(1, "Toy", "MULT"), mod(2, "Toy", "MULT")],
                     [cable(1, 2, 0, 1, 0)])
    return [("a live-but-unanswerable edge reports UNEVALUATED",
             [f["verdict"] for f in fs] == [CD.UNEVALUATED], str(fs)),
            ("and never rejects the patch",
             CD.dead_edge_errors(pch, inv) == [], str(fs)),
            ("a module with no depth control reports nothing at all",
             CD.findings(quiet, inv) == [], str(CD.findings(quiet, inv)))]


# ── the association rules themselves ────────────────────────────────────────

def test_the_rules_abstain_rather_than_guess(inv):
    """Two inputs and two controls that no rule can pair is an abstention."""
    a = CD.associations(inv, "Toy", "AMBIG")
    return [("an ambiguous module yields no association", a == {}, str(a))]


def test_one_input_matched_by_two_controls_is_an_abstention(inv):
    """Taking the first of several matches is how a rule gets it wrong.

    TWOWAY's single input is covered by both `Cutoff CV amount` and
    `Cutoff depth`. Nothing in the names says which one governs it, so the
    only correct answer is no answer.
    """
    a = CD.associations(inv, "Toy", "TWOWAY")
    pch = patch_of([mod(1, "Toy", "TWOWAY"), mod(2, "Toy", "MULT")],
                   [cable(1, 2, 0, 1, 0)])
    CD.open_depth_controls(pch, inv)
    return [("two candidate controls yield no association", a == {}, str(a)),
            ("and the rule edits nothing",
             not values(pch["modules"][0]),
             f"params={pch['modules'][0].get('params')}"),
            ("the edge is reported UNEVALUATED, not guessed at",
             [f["verdict"] for f in CD.findings(pch, inv)] == [CD.UNEVALUATED],
             str(CD.findings(pch, inv)))]


def test_the_stem_rule_resolves_each_input_separately(inv):
    """Where names do cover, both inputs resolve, and to different controls."""
    a = CD.associations(inv, "Toy", "FOLD")
    return [("Drive CV maps to Drive CV amount",
             getattr(a.get(0), "param", None) == 2, str(a)),
            ("Symmetry CV maps to Symmetry CV amount",
             getattr(a.get(1), "param", None) == 3, str(a)),
            ("the audio input gets no association", 2 not in a, str(a))]


def test_the_sole_candidate_rule_reaches_the_abbreviation_gap(inv):
    """`Frequency modulation` governed by `FM amount`.

    The word rules correctly abstain here — FREQUENCY is not covered by FM —
    and abstaining would leave the single most common dead edge in the corpus
    invisible. Counting resolves it: one CV input, one depth control.
    """
    a = CD.associations(inv, "Toy", "VCO")
    got = a.get(1)
    return [("the FM input resolves to the FM amount control",
             getattr(got, "param", None) == 3, str(a)),
            ("by the sole-candidate rule",
             "sole-candidate" in getattr(got, "rule", ""), str(a)),
            ("it stays NOMINATED",
             getattr(got, "evidence", None) == CD.NOMINATED, str(a)),
            ("the pitch input is not nominated", 0 not in a, str(a))]


def test_a_witness_outranks_every_heuristic(inv):
    """Fundamental/VCF in:0 has a receipt; no name rule may displace it."""
    a = CD.associations(inv, "Fundamental", "VCF")
    got = a.get(0)
    return [("the witnessed input keeps its measured association",
             getattr(got, "evidence", None) == CD.MEASURED, str(a)),
            ("and its receipt's parameter",
             getattr(got, "param", None) == 3, str(a)),
            ("and it is blocking", getattr(got, "blocking", None) is True,
             str(a))]


def test_only_a_witness_enters_the_receipt_table():
    """A nomination must not be able to launder itself into a gate."""
    import tempfile
    doc = {"modules": {"X/Y": {
        "0": {"param": 1, "evidence": "NOMINATED", "blocking": True},
        "1": {"param": 2, "evidence": "MEASURED", "blocking": True}}}}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
        json.dump(doc, fh)
        path = fh.name
    try:
        table = CD.receipts(path)
    finally:
        os.unlink(path)
    entries = table.get("X/Y", {})
    return [("a NOMINATED row is refused entry", 0 not in entries, str(entries)),
            ("a MEASURED row is loaded", 1 in entries, str(entries)),
            ("every loaded row is Measured",
             all(isinstance(v, CD.Measured) for v in entries.values()),
             str(entries))]


def test_the_shipped_receipts_are_all_witnessed():
    table = CD.receipts()
    rows = [(s, i, e) for s, m in table.items() for i, e in m.items()]
    return [("the shipped table is not empty", bool(rows), str(len(rows))),
            ("every shipped row is MEASURED",
             all(e.evidence == CD.MEASURED for _, _, e in rows), str(rows))]


# ── the wiring ──────────────────────────────────────────────────────────────

def test_the_linter_rejects_a_witnessed_dead_edge(inv):
    pch = patch_of([mod(1, "Fundamental", "VCF"),
                    mod(2, "Toy", "MULT", pos=(20, 0))],
                   [cable(1, 2, 0, 1, 0)])
    errs = P.lint(copy.deepcopy(pch), inv)
    hit = [e for e in errs if "carries nothing" in e]
    return [("lint reports the witnessed dead edge", len(hit) == 1, str(errs))]


def test_the_linter_does_not_reject_a_nominated_dead_edge(inv):
    pch = patch_of([mod(1, "Toy", "VCO"), mod(2, "Toy", "MULT", pos=(20, 0))],
                   [cable(1, 2, 0, 1, 1)])
    errs = P.lint(copy.deepcopy(pch), inv)
    return [("lint stays silent on a nominated dead edge",
             not [e for e in errs if "carries nothing" in e], str(errs))]


def test_explain_tells_the_reader_about_a_quiet_modulation(inv):
    pch = patch_of([mod(1, "Toy", "VCO"), mod(2, "Toy", "MULT", pos=(20, 0))],
                   [cable(1, 2, 0, 1, 1)])
    text = P.explain(pch, inv)
    return [("explain names the modulation that may not be heard",
             "MODULATION THAT MAY NOT BE HEARD" in text, text)]


def test_prepare_and_lint_opens_the_control_before_judging(inv):
    """End to end: the same patch that had a dead edge comes out working."""
    pch = patch_of([mod(1, "Toy", "VCO"), mod(2, "Toy", "MULT", pos=(20, 0))],
                   [cable(1, 2, 0, 1, 1)])
    out, errs = P.prepare_and_lint(copy.deepcopy(pch), inv)
    got = values(out["modules"][0]).get(3)
    return [("the depth control is open after prepare_and_lint",
             got is not None and got != 0, f"param 3 = {got!r}"),
            ("and no dead edge remains",
             not CD.advisory_notes(out, inv), str(CD.advisory_notes(out, inv)))]


CASES = [
    test_the_rule_opens_a_depth_control_the_patch_never_set,
    test_the_rule_never_overrides_an_authored_value,
    test_a_physical_target_counts_as_the_author_speaking,
    test_running_the_rule_twice_changes_nothing,
    test_the_rule_ignores_an_input_with_no_cable,
    test_the_rule_leaves_a_patch_it_cannot_evaluate_alone,
    test_the_rule_fixes_what_the_check_reports,
    test_a_nominated_association_can_never_block,
    test_a_witnessed_association_fails_the_patch,
    test_a_nominated_association_only_advises,
    test_an_unevaluated_edge_does_not_reject_the_patch,
    test_the_rules_abstain_rather_than_guess,
    test_one_input_matched_by_two_controls_is_an_abstention,
    test_the_stem_rule_resolves_each_input_separately,
    test_the_sole_candidate_rule_reaches_the_abbreviation_gap,
    test_a_witness_outranks_every_heuristic,
    test_only_a_witness_enters_the_receipt_table,
    test_the_shipped_receipts_are_all_witnessed,
    test_the_linter_rejects_a_witnessed_dead_edge,
    test_the_linter_does_not_reject_a_nominated_dead_edge,
    test_explain_tells_the_reader_about_a_quiet_modulation,
    test_prepare_and_lint_opens_the_control_before_judging,
]


def main() -> int:
    inv = inventory()
    bad = ran = 0
    for case in CASES:
        try:
            results = (case(inv) if case.__code__.co_argcount else case())
        except Exception as error:                      # noqa: BLE001
            print(f"  ERROR  {case.__name__}: {error!r}")
            bad += 1
            ran += 1
            continue
        for name, ok, detail in results:
            ran += 1
            if not ok:
                bad += 1
                print(f"  WRONG  {name}\n         {detail}")
    print(f"cv-depth: {ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
