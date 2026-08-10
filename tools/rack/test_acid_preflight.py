#!/usr/bin/env python3
"""Regression for the first GPT-5.6 acid patch's avoidable failures."""

from __future__ import annotations

import copy
import inspect
import json
import math
import os
import sys
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patch as P                                             # noqa: E402
import patch_vocabulary as V                                  # noqa: E402
import idiom_check as I                                       # noqa: E402

ASSERTIONS = 0


def check(condition: bool, message: str) -> int:
    global ASSERTIONS
    ASSERTIONS += 1
    print(("  ok     " if condition else "  WRONG  ") + message)
    return 0 if condition else 1


def inventory() -> dict:
    modules = {
        "SEQ16": {"name": "SEQ16", "tags": ["Sequencer"],
                  "outputs": ["Gates", "Row 1 CV", "Row 2 CV", "Row 3 CV"],
                  "roles_out": ["Gate", "Cv", "Cv", "Cv"]},
        "SLEWRF": {"name": "SLEW", "tags": ["Slew limiter"],
                   "inputs": ["Signal", "Rise CV", "Fall CV"],
                   "roles_in": ["Cv", "Cv", "Cv"],
                   "outputs": ["Slewed"], "roles_out": ["Cv"]},
        "Process": {"name": "Process", "tags": ["Slew limiter"],
                    "inputs": ["Slew", "Voltage", "Gate"],
                    "roles_in": ["Cv", "Cv", "Gate"],
                    "outputs": ["Glide"], "roles_out": ["Cv"]},
        "VCO": {"name": "VCO", "tags": ["Oscillator"],
                "inputs": ["Pitch"], "roles_in": ["Pitch"],
                "outputs": ["Saw"], "roles_out": ["Audio"]},
        "VCF": {"name": "VCF", "tags": ["Filter"],
                "inputs": ["Cutoff", "Audio"], "roles_in": ["Cv", "Audio"],
                "outputs": ["Low-pass"], "roles_out": ["Audio"]},
        "ENV": {"name": "ENV", "tags": ["Envelope generator"],
                "inputs": ["Gate"], "roles_in": ["Gate"],
                "outputs": ["Envelope"], "roles_out": ["Cv"]},
        "VCA": {"name": "VCA", "tags": ["Voltage-controlled amplifier"],
                "inputs": ["CV", "Audio"], "roles_in": ["Cv", "Audio"],
                "outputs": ["Audio"], "roles_out": ["Audio"]},
    }
    return {"Fixture": {"name": "Fixture", "version": "2.0.6",
                        "modules": modules}}


def test_port_complete_plan() -> int:
    plan = P.intent_module_plan(
        "a melodic acid line with accent and slide from a single sequencer",
        inventory())
    slew = next(line for line in plan.splitlines() if line.startswith("- slew "))
    bad = check("Fixture/Process" in slew,
                "the pre-model acid plan offers the gate-selectable glide")
    bad += check("Fixture/SLEWRF" not in slew,
                 "a generic slew without gate-select is excluded before quota")
    bad += check("Fixture/SEQ16" in plan,
                 "the sequencer's three CV lanes plus gate lane are proven")
    return bad


def test_real_acid_plan_is_narrowed_to_proven_construction() -> int:
    inv = inventory()
    inv["AS"] = {"version": "2.0.6", "modules": {
        "SEQ16": inv["Fixture"]["modules"]["SEQ16"]}}
    inv["Fundamental"] = {"modules": {
        "Process": inv["Fixture"]["modules"]["Process"]}}
    inv["Core"] = {"modules": {"AudioInterface2": {
        "name": "Audio 2", "tags": ["External"],
        "inputs": ['To "device output 1"', 'To "device output 2"'],
        "roles_in": ["Audio", "Audio"],
        "outputs": ['From "device input 1"', 'From "device input 2"'],
        "roles_out": ["Audio", "Audio"]}}}
    inv["ForgeModular"] = {"modules": {
        name: inv["Fixture"]["modules"][name]
        for name in ("VCO", "VCF", "ENV", "VCA")}}
    inv["ForgeModular"]["modules"]["LFO"] = {
        "name": "LFO", "tags": ["Low-frequency oscillator"],
        "inputs": ["Reset"], "roles_in": ["Trigger"],
        "outputs": ["Triangle", "Square"],
        "roles_out": ["Cv", ["Cv", "Clock", "Gate", "Trigger"]]}
    selected = set()
    plan = P.intent_module_plan("acid bassline", inv, selected=selected)
    bad = check(
        "use this exact installed construction" in plan
        and "ForgeModular/LFO + AS/SEQ16 + Fundamental/Process" in plan
        and "LFO Square to SEQ16 External Clock" in plan
        and "same nonzero pitch transition twice" in plan
        and "accent has a matched control" in plan
        and "SLEW output 4, not GLIDE output 5" in plan
        and "0V on slide arrivals and 10V on every hard arrival" in plan
        and "use 0V/1V rather than 0V/10V" in plan
        and "raw Cutoff near -2.0" in plan
        and "VCA raw Level near 0.25" in plan
        and "raw Rate to 2.0" in plan
        and "Attack 0.001 s, Decay 0.06 s" in plan
        and "raw Tune control to -2.25" in plan
        and "[0.0, 0.583, 0.0, 0.583, 1.0, 0.25, 0.0, 1.0]" in plan
        and "Matched same-pitch accented notes" in plan
        and "final audio-interface signal" in plan
        and "Do not substitute" in plan,
        "the acid proof names one exact known construction before quota")
    bad += check(selected == {
        ("AS", "SEQ16"), ("Core", "AudioInterface2"),
        ("Fundamental", "Process"),
        ("ForgeModular", "LFO"), ("ForgeModular", "VCO"),
        ("ForgeModular", "VCF"), ("ForgeModular", "ENV"),
        ("ForgeModular", "VCA")},
        "the model inventory is restricted to the proven acid construction")
    subset = P.inventory_subset(inv, selected)
    bad += check(sum(len(p["modules"]) for p in subset.values()) == 8,
                 "Cartog/Core data is preserved for only eight legal modules")
    return bad


def test_acid_witness_is_materialized_before_rack() -> int:
    patch = {"modules": [
        {"id": 0, "plugin": "ForgeModular", "model": "LFO",
         "params": [{"id": 0, "value": 0.0}]},
        {"id": 1, "plugin": "ForgeModular", "model": "VCO"},
        {"id": 2, "plugin": "ForgeModular", "model": "ENV",
         "params": [{"id": 1, "value": 0.12}]},
        {"id": 3, "plugin": "AS", "model": "SEQ16", "params": []},
    ]}
    notes = P.materialize_acid_witness(patch)
    params = {(module["plugin"], module["model"]):
              {param["id"]: param["value"] for param in module["params"]}
              for module in patch["modules"]}
    return check(
        params[("ForgeModular", "LFO")][0] == 2.0
        and params[("ForgeModular", "VCO")][0] == -2.25
        and params[("ForgeModular", "ENV")] == {
            0: 0.001, 1: 0.06, 2: 0.0, 3: 0.01}
        and params[("AS", "SEQ16")][3] == 8
        and len(notes) == 4,
        "the admitted acid witness replaces omitted or approximate controls")


def test_acid_owns_slide_semantics_but_keeps_independent_sounds() -> int:
    acid = V.for_prompt(
        "melodic acid line with accent and slide from a single sequencer")
    cello = V.for_prompt("a cello drone")
    bad = check("instrument-r-b-slide" not in acid,
                "the exact acid idiom outranks an unrelated R&B slide recipe")
    bad += check("instrument-cello" in cello,
                 "idiom precedence does not discard an independent named sound")
    return bad


def test_missing_fit_fails_before_model() -> int:
    inv = inventory()
    del inv["Fixture"]["modules"]["Process"]
    plan = P.intent_module_plan("acid bassline", inv)
    return check("slew " in plan and "NO port-complete installed choice" in plan,
                 "an installed tag without the required jacks is a pre-call gap")


def test_generation_stops_before_quota() -> int:
    inv = inventory()
    del inv["Fixture"]["modules"]["Process"]
    calls = []
    old_find, old_ask = P.find_claude, P.ask_model
    P.find_claude = lambda: "/unused/model-cli"
    P.ask_model = lambda *args, **kwargs: calls.append(args) or (0, "", "")
    message = ""
    try:
        P.generate("acid bassline", inv, None, retries=0)
    except SystemExit as exc:
        message = str(exc)
    finally:
        P.find_claude, P.ask_model = old_find, old_ask
    return check(not calls and "Nothing was sent to the model" in message,
                 "a missing port-complete plan spends zero model calls")


def test_malformed_refinement_base_stops_before_model() -> int:
    inv = {"TwinSlide": {"name": "TwinSlide", "version": "2.1.6",
                           "modules": {"TwinSlide": {
                               "name": "TwinSlide", "tags": ["Sequencer"],
                               "inputs": [], "outputs": []}}}}
    calls = []
    provider_calls = []
    catalog_calls = []
    module_index_calls = []
    old_find, old_ask = P.find_claude, P.ask_model
    old_catalog, old_module_index = P.catalog, P.module_index
    P.find_claude = lambda: provider_calls.append(1) or "/unused/model-cli"
    P.ask_model = lambda *args, **kwargs: calls.append(args) or (0, "", "")
    P.catalog = lambda: catalog_calls.append(1) or {}
    P.module_index = lambda: module_index_calls.append(1) or {}
    rejected = 0
    try:
        malformed = (
            None, [], "bad", {"forgePattern": {}},
            {"forgePattern": twinslide_pattern()},
        )
        for data in malformed:
            base = {"modules": [{"id": 1, "plugin": "TwinSlide",
                                   "model": "TwinSlide"}], "cables": []}
            if data is not None:
                base["modules"][0]["data"] = data
            try:
                P.generate("adjust @TwinSlide settings", inv, None, retries=0,
                           base_patch=base)
            except RuntimeError:
                rejected += 1
    finally:
        P.find_claude, P.ask_model = old_find, old_ask
        P.catalog, P.module_index = old_catalog, old_module_index
    return check(rejected == 5 and not provider_calls and not calls
                 and not catalog_calls and not module_index_calls,
                 "unmaterialized refinement bases spend zero discovery, "
                 "network, or model calls")


def test_build_help_spends_zero_calls() -> int:
    calls = []
    old_inventory, old_ask = P.inventory, P.ask_model
    P.inventory = lambda: calls.append("inventory") or {}
    P.ask_model = lambda *args, **kwargs: calls.append("model") or (0, "", "")
    try:
        result = P.main(["patch.py", "build", "--help"])
    finally:
        P.inventory, P.ask_model = old_inventory, old_ask
    return check(result == 0 and not calls,
                 "build --help is a guaranteed zero-inventory zero-model path")


def test_raw_response_retention_is_exact_and_fail_closed() -> int:
    import pathlib
    import tempfile
    with tempfile.TemporaryDirectory() as root:
        old = os.environ.get("FORGE_ATTEMPT_DIR")
        os.environ["FORGE_ATTEMPT_DIR"] = root
        try:
            path = P.keep_model_response("exact paid response\n", 1)
            exact = pathlib.Path(path).read_text() == "exact paid response\n"
            refused = False
            try:
                P.keep_model_response("replacement", 1)
            except RuntimeError:
                refused = True
        finally:
            if old is None:
                os.environ.pop("FORGE_ATTEMPT_DIR", None)
            else:
                os.environ["FORGE_ATTEMPT_DIR"] = old
    return check(exact and refused,
                 "raw paid response is immutable and retention failure stops the run")


def test_seq16_state_materialization() -> int:
    params = [{"id": 0, "value": 1.0}, {"id": 1, "value": 1.0}]
    params += [{"id": i, "value": float(i % 2)} for i in range(56, 72)]
    patch = {"modules": [{"id": 1, "plugin": "AS", "model": "SEQ16",
                          "params": params}], "cables": []}
    inv = {"AS": {"version": "2.0.6", "modules": {"SEQ16": {}}}}
    notes = P.materialize_module_state(patch, inv)
    module = patch["modules"][0]
    ids = {p["id"] for p in module["params"]}
    expected = [i % 2 for i in range(56, 72)]
    bad = check(module["data"] == {"running": True, "gates": expected},
                "SEQ16 run and latched gates become Rack-persistent data")
    bad += check(ids == {0},
                 "momentary Run and step buttons are not held high on load")
    bad += check(len(notes) == 1, "the deterministic repair is observable")

    explicit = copy.deepcopy(patch)
    explicit["modules"][0]["data"] = {"running": False,
                                        "gates": [True] * 16}
    P.materialize_module_state(explicit, inv)
    bad += check(explicit["modules"][0]["data"]["running"] is False,
                 "explicit persistent state outranks adapter defaults")
    bad += check(explicit["modules"][0]["data"]["gates"] == [1] * 16,
                 "boolean Rack gate state is normalized to integer one")
    return bad


def test_omitted_gate_controls_stay_audible() -> int:
    patch = {"modules": [{"id": 1, "plugin": "AS", "model": "SEQ16",
                          "params": [{"id": 0, "value": 1.0}]}]}
    inv = {"AS": {"version": "2.0.6", "modules": {"SEQ16": {}}}}
    P.materialize_module_state(patch, inv)
    return check(patch["modules"][0]["data"]["gates"] == [1] * 16,
                 "omitted momentary gate controls keep the proven Rack default")


def test_version_gate() -> int:
    patch = {"modules": [{"id": 1, "plugin": "AS", "model": "SEQ16",
                          "params": [{"id": 1, "value": 1.0}]}]}
    before = copy.deepcopy(patch)
    inv = {"AS": {"version": "9.9.9", "modules": {"SEQ16": {}}}}
    P.materialize_module_state(patch, inv)
    return check(patch == before,
                 "module-state knowledge never crosses a plugin version")


def twinslide_pattern(offset: int = 0) -> dict:
    def step(note, gate=True, accent=False, slide=False):
        return {"semitones": note + offset, "gate": gate,
                "accent": accent, "slide": slide}
    return {"tracks": [
        {"steps": [step(0, accent=True), step(3, slide=True), step(7),
                   step(10), step(7, accent=True), step(3),
                   step(12, slide=True), step(7)]},
        {"steps": [step(-12, accent=True), step(-5), step(0, slide=True),
                   step(3), step(7, accent=True), step(3),
                   step(0, slide=True), step(-5)]},
    ]}


def test_cold_twinslide_state_contract() -> int:
    patch = {"modules": [{
        "id": 1, "plugin": "TwinSlide", "model": "TwinSlide",
        "params": [{"id": 2, "value": 1.0},
                   {"id": 58, "value": 0.35},
                   {"id": 67, "value": 0.4}],
        "data": {"forgePattern": twinslide_pattern()},
    }]}
    inv = {"TwinSlide": {"version": "2.1.6", "modules": {"TwinSlide": {}}}}
    notes = P.materialize_module_state(patch, inv)
    module = patch["modules"][0]
    ids = {param["id"] for param in module["params"]}
    data = module["data"]
    bad = check(data["running"] is True and data["trackLength"] == [8, 8]
                and data["pulsesPerStep"] == 1,
                "authored TwinSlide music compiles into running persistent state")
    bad += check(len(set(data["cv"][:8])) >= 4
                 and any(value & 4 for value in data["attributes"][:8])
                 and any(value & 8 for value in data["attributes"][:8]),
                 "the persistent witness has distinct pitches, accents, and slides")
    bad += check(ids == {58, 67} and len(notes) == 1,
                 "momentary editor controls are removed while synth settings survive")

    transposed = {"modules": [{"id": 1, "plugin": "TwinSlide",
                                 "model": "TwinSlide",
                                 "data": {"forgePattern":
                                          twinslide_pattern(2)}}]}
    P.materialize_module_state(transposed, inv)
    bad += check(transposed["modules"][0]["data"]["cv"] != data["cv"],
                 "different authored notes produce different serialized music")

    projected = P.refinement_model_base(
        {"modules": [copy.deepcopy(module)]}, inv)
    editable = projected["modules"][0]["data"]
    bad += check(set(editable) == {"forgePattern"}
                 and editable["forgePattern"] == twinslide_pattern(),
                 "a compiled TwinSlide base is projected back to exact authored state")
    editable["forgePattern"]["tracks"][0]["steps"][0]["semitones"] += 2
    P.materialize_module_state(projected, inv,
                               compiled_state_baseline={"modules": [module]})
    bad += check(projected["modules"][0]["data"]["cv"][0] == 2 / 12.0,
                 "a refinement edits authored notes then overlays compiled state")
    refused_future = False
    future = {"TwinSlide": {"version": "2.1.7",
                              "modules": {"TwinSlide": {}}}}
    try:
        P.refinement_model_base({"modules": [copy.deepcopy(module)]}, future)
    except RuntimeError as exc:
        refused_future = "exact 2.1.6 state contract" in str(exc)
    bad += check(refused_future,
                 "editable state projection never crosses a vendor version")
    authored_future = False
    try:
        P.refinement_model_base(
            {"modules": [{"id": 1, "plugin": "TwinSlide",
                            "model": "TwinSlide",
                            "data": {"forgePattern": twinslide_pattern()}}]},
            future)
    except RuntimeError as exc:
        authored_future = "exact 2.1.6 state contract" in str(exc)
    bad += check(authored_future,
                 "authored refinement projection never crosses a vendor version")

    missing = {"modules": [{"id": 1, "plugin": "TwinSlide",
                              "model": "TwinSlide"}]}
    failed_closed = False
    try:
        P.materialize_module_state(missing, inv)
    except RuntimeError as exc:
        failed_closed = "stock/default sequence is not accepted" in str(exc)
    bad += check(failed_closed,
                 "a named TwinSlide patch cannot silently use stock music")

    bland_pattern = twinslide_pattern()
    for step_value in bland_pattern["tracks"][0]["steps"]:
        step_value["semitones"] = 0
    bland = {"modules": [{"id": 1, "plugin": "TwinSlide",
                            "model": "TwinSlide",
                            "data": {"forgePattern": bland_pattern}}]}
    rejected = False
    try:
        P.materialize_module_state(bland, inv)
    except RuntimeError as exc:
        rejected = "pitch change at every transition" in str(exc)
    bad += check(rejected, "a one-note authored TwinSlide pattern fails closed")

    clustered = twinslide_pattern()
    clustered["tracks"][0]["steps"] = [
        {"semitones": note, "gate": True,
         "accent": index in {0, 6}, "slide": index in {1, 7}}
        for index, note in enumerate([0, 0, 0, 0, 0, 0, 4, 7])]
    rejected = False
    try:
        P.materialize_module_state(
            {"modules": [{"id": 1, "plugin": "TwinSlide",
                            "model": "TwinSlide",
                            "data": {"forgePattern": clustered}}]}, inv)
    except RuntimeError as exc:
        rejected = "pitch change at every transition" in str(exc)
    bad += check(rejected,
                 "clustered pitches cannot underfill the runtime change count")

    two_note = twinslide_pattern()
    two_note["tracks"][0]["steps"] = [
        {"semitones": note, "gate": True,
         "accent": index in {0, 4}, "slide": index in {1, 5}}
        for index, note in enumerate([0, 3] * 4)]
    rejected = False
    try:
        P.materialize_module_state(
            {"modules": [{"id": 1, "plugin": "TwinSlide",
                            "model": "TwinSlide",
                            "data": {"forgePattern": two_note}}]}, inv)
    except RuntimeError as exc:
        rejected = "at least three distinct gated pitches" in str(exc)
    bad += check(rejected,
                 "two-note alternation cannot underfill distinct-pitch runtime")

    accent_only = twinslide_pattern()
    accent_only["tracks"][1] = copy.deepcopy(accent_only["tracks"][0])
    accent_only["tracks"][1]["steps"][0]["accent"] = False
    accent_only["tracks"][1]["steps"][1]["accent"] = True
    rejected = False
    try:
        P.materialize_module_state(
            {"modules": [{"id": 1, "plugin": "TwinSlide",
                            "model": "TwinSlide",
                            "data": {"forgePattern": accent_only}}]}, inv)
    except RuntimeError as exc:
        rejected = "different gated pitch material" in str(exc)
    bad += check(rejected,
                 "accent-only differences cannot fake two musical tracks")

    inactive_only = twinslide_pattern()
    inactive_only["tracks"][1] = copy.deepcopy(inactive_only["tracks"][0])
    for track in inactive_only["tracks"]:
        track["steps"][7].update(
            {"gate": False, "accent": False, "slide": False})
    inactive_only["tracks"][1]["steps"][7]["semitones"] += 2
    rejected = False
    try:
        P.materialize_module_state(
            {"modules": [{"id": 1, "plugin": "TwinSlide",
                            "model": "TwinSlide",
                            "data": {"forgePattern": inactive_only}}]}, inv)
    except RuntimeError as exc:
        rejected = "must be gated" in str(exc)
    bad += check(rejected,
                 "ungated steps cannot satisfy the eight-onset runtime contract")

    polluted = {"modules": [{"id": 1, "plugin": "TwinSlide",
                                "model": "TwinSlide", "data": {
                                    "forgePattern": twinslide_pattern(),
                                    "expertA": {"vcfRange": 999}}}]}
    rejected = False
    try:
        P.materialize_module_state(polluted, inv)
    except RuntimeError as exc:
        rejected = "must contain only forgePattern" in str(exc)
    bad += check(rejected,
                 "uncontracted vendor data cannot ride beside authored music")

    rack_saved = copy.deepcopy(data)
    rack_saved.pop("forgeContract")
    rack_saved["cv"] += [0.0] * (1024 - len(rack_saved["cv"]))
    rack_saved["attributes"] += [1] * (1024 - len(rack_saved["attributes"]))
    for field, fill in (("trackLength", 16), ("trackRunMode", 0),
                        ("trackTranspose", 0), ("trackRotate", 0),
                        ("trackClockDiv", 1)):
        rack_saved[field] += [fill] * (64 - len(rack_saved[field]))
    rack_saved.update({"resetOnRun": True, "runModeSong3": 2,
                       "phrase": [1, 2, 3]})
    rack_saved["cv"][32] = 2.0
    rack_saved["attributes"][32] = 16
    rack_saved["attributes"][33] = 2
    rack_saved["attributes"][34] = 11 << 5
    rack_saved["attributes"][35] = 4 << 9
    rack_saved["attributes"][36] = 0x1000
    rack_saved["trackLength"][2] = 5
    rack_saved["trackRunMode"][2] = 3
    roundtrip = {"modules": [{"id": 1, "plugin": "TwinSlide",
                                "model": "TwinSlide",
                                "data": rack_saved}],
                 "cables": []}
    bad += check(not P._twinslide_compiled_state_errors(rack_saved),
                 "Rack-expanded state reparses without a Forge-only marker")
    try:
        P.materialize_module_state(
            roundtrip, inv, allow_compiled_state=True)
        roundtrip_ok = True
    except RuntimeError:
        roundtrip_ok = False
    bad += check(roundtrip_ok,
                 "Rack-saved TwinSlide state is idempotent during refinement")

    fresh_compiled = {"modules": [{"id": 1, "plugin": "TwinSlide",
                                      "model": "TwinSlide",
                                      "data": copy.deepcopy(data)}]}
    rejected = False
    try:
        P.materialize_module_state(fresh_compiled, inv)
    except RuntimeError as exc:
        rejected = "requires authored persistent state" in str(exc)
    bad += check(rejected,
                 "fresh model output cannot bypass authoring with compiled state")

    preserved = {"modules": [{"id": 1, "plugin": "TwinSlide",
                                 "model": "TwinSlide",
                                 "data": copy.deepcopy(rack_saved)}],
                 "cables": []}
    try:
        _, preserved_errors = P.prepare_and_lint(
            preserved, inv, base_patch=copy.deepcopy(roundtrip))
        preserved_ok = not any("persistent state is invalid" in error
                               for error in preserved_errors)
    except RuntimeError:
        preserved_ok = False
    bad += check(preserved_ok,
                 "refinement may preserve exact validated Rack-saved state")

    authored_refinement = {
        "modules": [{"id": 1, "plugin": "TwinSlide", "model": "TwinSlide",
                     "data": {"forgePattern": twinslide_pattern(2)}}],
        "cables": [],
    }
    try:
        P.prepare_and_lint(
            authored_refinement, inv, base_patch=copy.deepcopy(roundtrip))
        refined_data = authored_refinement["modules"][0]["data"]
        opaque_preserved = all(
            P._json_exact_equal(refined_data[field], rack_saved[field])
            for field in ("resetOnRun", "runModeSong3", "phrase"))
        opaque_preserved = opaque_preserved and all(
            P._json_exact_equal(refined_data[field][index],
                                rack_saved[field][index])
            for field, index in (("cv", 32), ("attributes", 32),
                                 ("attributes", 33), ("attributes", 34),
                                 ("attributes", 35), ("attributes", 36),
                                 ("trackLength", 2), ("trackRunMode", 2)))
        opaque_preserved = opaque_preserved and \
            len(refined_data["cv"]) == 1024 and \
            len(refined_data["trackLength"]) == 64
    except RuntimeError:
        opaque_preserved = False
    bad += check(opaque_preserved,
                 "authored refinement preserves exact vendor-owned Rack state")

    changed_compiled = copy.deepcopy(preserved)
    changed_compiled["modules"][0]["data"]["cv"][0] += 1.0 / 12.0
    rejected = False
    try:
        P.prepare_and_lint(
            changed_compiled, inv, base_patch=copy.deepcopy(roundtrip))
    except RuntimeError as exc:
        rejected = "requires authored persistent state" in str(exc)
    bad += check(rejected,
                 "refinement cannot mutate opaque compiled state directly")

    for field, invalid in (("resetOnRun", 1), ("runModeSong3", 2.0),
                           ("phrase", [1.0, 2, 3])):
        changed_opaque = copy.deepcopy(preserved)
        changed_opaque["modules"][0]["data"][field] = invalid
        rejected = False
        try:
            P.prepare_and_lint(
                changed_opaque, inv, base_patch=copy.deepcopy(roundtrip))
        except RuntimeError as exc:
            rejected = "requires authored persistent state" in str(exc)
        bad += check(rejected,
                     f"refinement rejects JSON-type mutation in opaque {field}")

    for field, invalid in (("running", False), ("pulsesPerStep", 24),
                           ("trackRunMode", [999, 999]),
                           ("trackClockDiv", [5, 5])):
        mutated = copy.deepcopy(data)
        mutated[field] = invalid
        bad += check(bool(P._twinslide_compiled_state_errors(mutated)),
                     f"reparse rejects mutated compiler-owned {field}")
    for field, invalid in (("running", 1), ("pulsesPerStep", True),
                           ("trackLength", [8.0, 8.0]),
                           ("trackRunMode", [False, False]),
                           ("trackClockDiv", [True, True])):
        mutated = copy.deepcopy(data)
        mutated[field] = invalid
        bad += check(bool(P._twinslide_compiled_state_errors(mutated)),
                     f"reparse rejects wrong JSON type for {field}")
    for invalid in (100.0, -100.0, 0.123456789):
        mutated = copy.deepcopy(data)
        mutated["cv"][0] = invalid
        bad += check(bool(P._twinslide_compiled_state_errors(mutated)),
                     f"reparse rejects out-of-contract pitch value {invalid}")
    bad += check(bool(P._twinslide_compiled_state_errors([])),
                 "reparse rejects non-object persistent data without crashing")
    malformed_cv = copy.deepcopy(data)
    malformed_cv["cv"][0] = []
    bad += check(bool(P._twinslide_compiled_state_errors(malformed_cv)),
                 "reparse rejects nonnumeric pitch without crashing")
    huge_cv = copy.deepcopy(data)
    huge_cv["cv"][0] = 10 ** 10000
    try:
        huge_rejected = bool(P._twinslide_compiled_state_errors(huge_cv))
    except Exception:
        huge_rejected = False
    bad += check(huge_rejected,
                 "reparse rejects huge JSON pitch integers without crashing")
    expanded_bad_tail = copy.deepcopy(rack_saved)
    expanded_bad_tail["trackClockDiv"][63] = True
    bad += check(bool(P._twinslide_compiled_state_errors(expanded_bad_tail)),
                 "reparse validates JSON types across expanded track state")
    for field, invalid in (("trackLength", 17), ("trackRunMode", 7),
                           ("trackTranspose", 25), ("trackRotate", 17),
                           ("trackClockDiv", 0)):
        expanded_bad_tail = copy.deepcopy(rack_saved)
        expanded_bad_tail[field][63] = invalid
        bad += check(bool(P._twinslide_compiled_state_errors(
                             expanded_bad_tail)),
                     f"reparse validates vendor range across expanded {field}")
    for invalid in (12 << 5, 5 << 9):
        expanded_bad_tail = copy.deepcopy(rack_saved)
        expanded_bad_tail["attributes"][63] = invalid
        bad += check(bool(P._twinslide_compiled_state_errors(
                             expanded_bad_tail)),
                     f"reparse rejects out-of-range vendor attribute {invalid}")

    future = {"modules": [{"id": 1, "plugin": "TwinSlide",
                            "model": "TwinSlide"}]}
    P.materialize_module_state(
        future, {"TwinSlide": {"version": "2.1.7", "modules": {}}})
    bad += check("data" not in future["modules"][0],
                 "the state contract never crosses an unverified TwinSlide version")
    return bad


def test_twinslide_authoring_contract_reaches_model() -> int:
    inv = {"TwinSlide": {"name": "Twisted Cable", "version": "2.1.6",
                           "modules": {"TwinSlide": {
                               "name": "Twin Slide", "params": [
                                   {"id": 21, "name": "Track A - Step 1",
                                    "min": 0.0, "max": 1.0,
                                    "default": 0.0}]}}}}
    rendered = P.render_inventory(inv)
    bad = check("data.forgePattern" in rendered
                and "semitones" in rendered
                and "params 21..52" in rendered
                and "param 13" in rendered
                and "params 62 and 71" in rendered,
                "the exact installed TwinSlide authoring schema reaches the model")
    future = copy.deepcopy(inv)
    future["TwinSlide"]["version"] = "2.1.7"
    bad += check("data.forgePattern" not in P.render_inventory(future),
                 "persistent authoring knowledge never crosses a vendor version")
    closed = {("TwinSlide", "TwinSlide"),
              ("ForgeModular", "LFO"),
              ("Core", "AudioInterface2")}
    bad += check(P.closed_module_idiom_contract(
        closed, "acid-voice", inv) == "TwinSlide/TwinSlide",
        "an exact self-contained instrument contract supersedes the generic "
        "acid topology")
    rejected_future = False
    try:
        P.closed_module_idiom_contract(closed, "acid-voice", future)
    except RuntimeError as error:
        rejected_future = "Nothing was sent to the model" in str(error)
    bad += check(rejected_future,
                 "a module capability never falls back across vendor versions")
    future_inv = copy.deepcopy(inv)
    future_inv["TwinSlide"]["version"] = "2.1.7"
    future_inv["ForgeModular"] = {"version": "2.0.0", "modules": {
        "LFO": {"name": "LFO"}}}
    future_inv["Core"] = {"version": "2.6.6", "modules": {
        "AudioInterface2": {"name": "Audio 2"}}}
    future_prompt = (
        "Build exactly three modules: @ForgeModular/LFO, "
        "@TwinSlide/TwinSlide, and @Core/AudioInterface2. Create an acid line.")
    calls = []
    try:
        with mock.patch.object(P, "find_claude", return_value="/model"), \
                mock.patch.object(P, "ask_model",
                                  side_effect=lambda *_: calls.append(1)):
            P.generate(future_prompt, future_inv, None, retries=0)
    except RuntimeError:
        pass
    bad += check(calls == [],
                 "a future-version closed capability spends zero model calls")
    idioms = I.load_idioms()
    claimed = I.resolve_intent(
        "an acid line from a single sequencer", idioms)
    modifier_inv = {
        "TwinSlide": {"modules": {"TwinSlide": {"tags": ["Sequencer"]}}},
        "AS": {"modules": {"SEQ16": {"tags": ["Sequencer"]}}},
    }
    modifier_patch = {"modules": [
        {"id": 1, "plugin": "TwinSlide", "model": "TwinSlide"},
        {"id": 2, "plugin": "AS", "model": "SEQ16"}], "cables": []}
    diagnosis = P.diagnose_module_contract_intent(
        "an acid line from a single sequencer", modifier_patch,
        modifier_inv, claimed, idioms)
    bad += check(any("single sequencer" in error and "found 2" in error
                     for error in diagnosis.modifiers),
                 "a module capability cannot erase a literal hard modifier")
    return bad


def test_named_sparse_module_contract_is_small_and_exact() -> int:
    inv = {
        "TwinSlide": {"version": "2.1.6", "modules": {"TwinSlide": {
            "name": "TwinSlide", "outputs_xy": [None, None, None, None,
                                                     [10, 10], [20, 10]],
            "inputs_xy": [[10, 10], [20, 10], [30, 10]],
            "params": [
                {"id": 13, "name": "Slide rate", "min": 0.0, "max": 2.0},
                {"id": 62, "name": "Accent A", "min": 0.0, "max": 1.0},
                {"id": 71, "name": "Accent B", "min": 0.0, "max": 1.0},
            ],
            "panel": [480, 380]}}},
        "ForgeModular": {"version": "2.0.0", "modules": {"LFO": {
            "name": "LFO", "params": [
                {"id": 0, "name": "Rate", "min": -8.0, "max": 8.0}],
            "panel": [90, 380]}}},
        "Core": {"version": "2.6.6", "modules": {"AudioInterface2": {
            "name": "Audio 2", "tags": ["External"], "panel": [120, 380]}}},
    }
    bad = 0
    with mock.patch.object(P, "catalog",
                           side_effect=AssertionError("must stay offline")), \
            mock.patch.object(P, "module_index",
                              side_effect=AssertionError("must stay offline")):
        ordinary = P.exact_named_module_selection(
            "an evolving stereo generative patch", inv)
    bad += check(ordinary == set(),
                 "ordinary prompts do not resolve the remote module catalog")

    prompt = ("use exactly @TwinSlide/TwinSlide with ForgeModular/LFO and "
              "Core/AudioInterface2; use only those three modules")
    selected = P.exact_named_module_selection(prompt, inv, {}, {
        "TwinSlide": {"TwinSlide": {"name": "TwinSlide"}},
        "ForgeModular": {"LFO": {"name": "LFO"}},
        "Core": {"AudioInterface2": {"name": "Audio 2"}},
    })
    bad += check(selected == {("TwinSlide", "TwinSlide"),
                              ("ForgeModular", "LFO"),
                              ("Core", "AudioInterface2")},
                 "qualified module names produce one exact legal shortlist")
    with mock.patch.object(P, "catalog",
                           side_effect=AssertionError("must stay offline")), \
            mock.patch.object(P, "module_index",
                              side_effect=AssertionError("must stay offline")):
        offline_selected = P.exact_named_module_selection(prompt, inv)
    bad += check(offline_selected == selected,
                 "installed qualified modules resolve from Cartog inventory "
                 "without remote catalogue access")
    closed = P.closed_named_module_selection(prompt, selected)
    bad += check(closed == selected,
                 "explicit exactly/only wording closes the named module set")

    wrong_set = {"modules": [
        {"plugin": "TwinSlide", "model": "TwinSlide"},
        {"plugin": "TwinSlide", "model": "TwinSlide"},
        {"plugin": "ForgeModular", "model": "LFO"},
        {"plugin": "Core", "model": "AudioInterface2"},
        {"plugin": "Other", "model": "Utility"},
    ]}
    closed_errors = P.closed_named_module_errors(wrong_set, closed)
    bad += check(any("exactly one TwinSlide/TwinSlide; got 2" in error
                     for error in closed_errors)
                 and any("forbids unrequested Other/Utility" in error
                         for error in closed_errors),
                 "closed-set validation rejects duplicate and extra modules")

    modules = [
        {"id": 1, "plugin": "ForgeModular", "model": "LFO", "pos": [0, 0]},
        {"id": 2, "plugin": "TwinSlide", "model": "TwinSlide", "pos": [8, 0],
         "data": {"forgePattern": twinslide_pattern()}},
        {"id": 3, "plugin": "Core", "model": "AudioInterface2", "pos": [50, 0]},
    ]
    wrong = {"modules": copy.deepcopy(modules), "cables": [
        {"id": 1, "outputModuleId": 1, "outputId": 0,
         "inputModuleId": 2, "inputId": 0},
        {"id": 2, "outputModuleId": 2, "outputId": 0,
         "inputModuleId": 3, "inputId": 0},
        {"id": 3, "outputModuleId": 2, "outputId": 1,
         "inputModuleId": 3, "inputId": 1},
    ]}
    _, errors = P.prepare_and_lint(wrong, inv)
    bad += check(any("no physical output jack at index 0" in error
                     for error in errors)
                 and any("persistent behavior contract requires cable" in error
                         for error in errors),
                 "sparse hidden ports and wrong behavior routing fail before Rack")

    slow = {"modules": copy.deepcopy(modules), "cables": [
        {"id": 1, "outputModuleId": 1, "outputId": 1,
         "inputModuleId": 2, "inputId": 1},
        {"id": 2, "outputModuleId": 2, "outputId": 4,
         "inputModuleId": 3, "inputId": 0},
        {"id": 3, "outputModuleId": 2, "outputId": 5,
         "inputModuleId": 3, "inputId": 1},
    ]}
    slow["modules"][0]["params"] = [{"id": 0, "value": -2.0}]
    _, slow_errors = P.prepare_and_lint(slow, inv)
    bad += check(any("ForgeModular/LFO param 0=0.0" in error
                     for error in slow_errors),
                 "a legal but too-slow clock fails before real Rack")

    unrelated = {"modules": copy.deepcopy(modules) + [
        {"id": 4, "plugin": "ForgeModular", "model": "LFO",
         "pos": [60, 0], "params": [{"id": 0, "value": -2.0}]}],
        "cables": [
            {"id": 1, "outputModuleId": 1, "outputId": 1,
             "inputModuleId": 2, "inputId": 1},
            {"id": 2, "outputModuleId": 2, "outputId": 4,
             "inputModuleId": 3, "inputId": 0},
            {"id": 3, "outputModuleId": 2, "outputId": 5,
             "inputModuleId": 3, "inputId": 1}]}
    _, unrelated_errors = P.prepare_and_lint(unrelated, inv)
    bad += check(not any("ForgeModular/LFO param 0=0.0" in error
                         for error in unrelated_errors),
                 "an unrelated slow LFO does not invalidate the proven clock")

    connected_unrelated = {"modules": copy.deepcopy(modules) + [
        {"id": 4, "plugin": "ForgeModular", "model": "LFO",
         "pos": [60, 0], "params": [{"id": 0, "value": -2.0}]}],
        "cables": copy.deepcopy(unrelated["cables"])}
    connected_unrelated["cables"].append(
        {"id": 4, "outputModuleId": 4, "outputId": 0,
         "inputModuleId": 2, "inputId": 0})
    _, connected_unrelated_errors = P.prepare_and_lint(
        connected_unrelated, inv)
    bad += check(not any("ForgeModular/LFO param 0=0.0" in error
                         for error in connected_unrelated_errors),
                 "a slow reset LFO does not invalidate the exact clock source")

    no_accent = {"modules": copy.deepcopy(modules), "cables": copy.deepcopy(
        unrelated["cables"])}
    no_accent["modules"][1]["params"] = [
        {"id": 62, "value": 0.0}, {"id": 71, "value": 0.0}]
    _, no_accent_errors = P.prepare_and_lint(no_accent, inv)
    bad += check(any("TwinSlide/TwinSlide param 62 in 0.1..1" in error
                     for error in no_accent_errors)
                 and any("TwinSlide/TwinSlide param 71 in 0.1..1" in error
                         for error in no_accent_errors),
                 "zero accent depth cannot fake selectively accented music")
    no_slide = {"modules": copy.deepcopy(modules), "cables": copy.deepcopy(
        unrelated["cables"])}
    no_slide["modules"][1]["params"] = [{"id": 13, "value": 0.0}]
    _, no_slide_errors = P.prepare_and_lint(no_slide, inv)
    bad += check(any("TwinSlide/TwinSlide param 13 in 0.1..2" in error
                     for error in no_slide_errors),
                 "zero slide rate cannot fake selectively sliding music")
    for param_id, invalid, maximum in ((13, 2.1, 2.0),
                                       (62, float("inf"), 1.0),
                                       (71, 1.1, 1.0)):
        out_of_range = {"modules": copy.deepcopy(modules),
                        "cables": copy.deepcopy(unrelated["cables"])}
        out_of_range["modules"][1]["params"] = [
            {"id": param_id, "value": invalid}]
        _, range_errors = P.prepare_and_lint(out_of_range, inv)
        bad += check(
            any(("finite JSON number" in error
                 if not math.isfinite(invalid)
                 else f"above Cartog maximum {maximum:g}" in error)
                for error in range_errors),
            f"TwinSlide param {param_id} cannot exceed its Cartog range")
    return bad


def test_cartog_parameter_contract_fails_closed() -> int:
    inv = {"Fixture": {"modules": {"Unit": {"params": [
        {"id": 0, "name": "Depth", "min": -1.0, "max": 1.0},
    ]}}}}

    def errors(params):
        patch = {"modules": [{"id": 1, "plugin": "Fixture",
                              "model": "Unit", "pos": [0, 0],
                              "params": params}], "cables": []}
        return P.prepare_and_lint(patch, inv)[1]

    bad = check(not any("Fixture/Unit param" in error
                        for error in errors([{"id": 0, "value": 0.25}])),
                "a finite in-range Cartog parameter passes")
    controls = (
        ([{"id": 0, "value": True}], "finite JSON number"),
        ([{"id": 0, "value": float("nan")}], "finite JSON number"),
        ([{"id": 0, "value": 2.0}], "above Cartog maximum"),
        ([{"id": 0, "value": -2.0}], "below Cartog minimum"),
        ([{"id": 1, "value": 0.0}], "no Cartog param 1"),
        ([{"id": 0, "value": 0.0}, {"id": 0, "value": 0.5}],
         "repeats param 0"),
    )
    for params, wanted in controls:
        bad += check(any(wanted in error for error in errors(params)),
                     f"Cartog parameter validation rejects {wanted}")
    rangeless = {"Fixture": {"modules": {"Unit": {"params": [
        {"id": 0, "name": "Native control"},
    ]}}}}
    for value, wanted in ((1e39, "finite in Rack's float32"),
                          (1e308, "finite in Rack's float32"),
                          (10 ** 100, "signed 64-bit JSON")):
        patch = {"modules": [{"id": 1, "plugin": "Fixture",
                              "model": "Unit", "pos": [0, 0],
                              "params": [{"id": 0, "value": value}]}],
                 "cables": []}
        _, range_errors = P.prepare_and_lint(patch, rangeless)
        bad += check(any(wanted in error for error in range_errors),
                     f"rangeless Cartog params still reject {wanted}")
    huge_physical = [{"id": 0, "physical": 10 ** 10000, "unit": "Hz"}]
    try:
        physical_errors = errors(huge_physical)
        closed = any("invalid physical target" in error
                     for error in physical_errors)
    except Exception:
        closed = False
    bad += check(closed,
                 "oversized physical targets fail without numeric overflow")
    return bad


def test_broken_override_registry_fails_closed() -> int:
    import tempfile
    with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as handle:
        handle.write("{")
        handle.flush()
        failed = False
        try:
            P.materialize_module_state({"modules": []}, {}, handle.name)
        except RuntimeError:
            failed = True
    return check(failed, "a broken exact-state registry cannot fail open")


def test_rack_integer_identity_types_fail_closed() -> int:
    malformed = {"modules": [
        {"id": [], "plugin": "TwinSlide", "model": "TwinSlide",
         "data": {"forgePattern": twinslide_pattern()}}],
        "cables": [{"id": 1.0, "outputModuleId": [], "outputId": 0.0,
                    "inputModuleId": {}, "inputId": 1.0}]}
    try:
        _, errors = P.prepare_and_lint(malformed, {})
        closed = (any("module id []" in error for error in errors)
                  and any("cable outputId 0.0" in error for error in errors))
    except Exception:
        closed = False
    bad = check(closed,
                "noninteger and unhashable Rack identities fail without crashing")
    for field, value in (("plugin", []), ("model", {})):
        module = {"id": 1, "plugin": "TwinSlide", "model": "TwinSlide"}
        module[field] = value
        try:
            _, errors = P.prepare_and_lint(
                {"modules": [module], "cables": []}, {})
            closed = any(f"module {field}" in error for error in errors)
        except Exception:
            closed = False
        bad += check(closed,
                     f"malformed module {field} fails without crashing")
    try:
        _, errors = P.prepare_and_lint(
            {"modules": [{"id": 10 ** 10000, "plugin": "TwinSlide",
                           "model": "TwinSlide"}], "cables": []}, {})
        closed = any("module id" in error for error in errors)
    except Exception:
        closed = False
    bad += check(closed,
                 "oversized Rack identity fails without formatting or hashing")
    for invalid in (False, 0, "", {}):
        try:
            _, errors = P.prepare_and_lint(
                {"modules": [{"id": 1, "plugin": "TwinSlide",
                               "model": "TwinSlide", "params": invalid}],
                 "cables": []}, {})
            closed = any("params must be a JSON array" in error
                         for error in errors)
        except Exception:
            closed = False
        bad += check(closed,
                     f"present malformed params {invalid!r} fails closed")
    duplicate_base = {
        "modules": [{"id": 1, "plugin": "TwinSlide", "model": "TwinSlide"},
                    {"id": 1, "plugin": "TwinSlide", "model": "TwinSlide"}],
        "cables": [],
    }
    try:
        _, errors = P.prepare_and_lint(
            {"modules": [], "cables": []}, {}, base_patch=duplicate_base)
        closed = any("refinement base: duplicate module ids" in error
                     for error in errors)
    except Exception:
        closed = False
    bad += check(closed,
                 "programmatic refinement rejects an ambiguous baseline")
    return bad


def test_twinslide_runtime_contract() -> int:
    inv = {"TwinSlide": {"version": "2.1.6", "modules": {}}}
    compiled = P._compile_twinslide_pattern(twinslide_pattern())
    compiled.pop("forgeContract")
    patch = {"modules": [{"id": 1, "plugin": "TwinSlide",
                           "model": "TwinSlide", "data": compiled}]}

    def cable(source, pitches, changes, onsets, median, centroid, peak):
        return {"source": source, "mean_abs_v": 0.1, "peak_abs_v": peak,
                "pitch": {"distinct_pitches": pitches,
                          "pitch_changes": changes, "median_hz": median},
                "onsets": {"onsets": onsets},
                "spectrum": {"centroid_mean_hz": centroid}}

    good = {"schema": 1, "cables": [
        cable("TwinSlide out 4", 4, 23, 23, 262.0, 853.0, 2.0),
        cable("TwinSlide out 5", 5, 26, 23, 260.8, 832.0, 1.1),
    ]}
    prefix = "BEHAVIOUR_JSON "
    bad = check(not P.module_runtime_contract_errors(
        patch, inv, prefix + json.dumps(good)),
        "both changing and measurably distinct TwinSlide outputs pass runtime")
    held = copy.deepcopy(good)
    held["cables"][1]["pitch"]["distinct_pitches"] = 1
    held["cables"][1]["pitch"]["pitch_changes"] = 0
    bad += check(any("pitch_changes >= 8" in problem
                     for problem in P.module_runtime_contract_errors(
                         patch, inv, prefix + json.dumps(held))),
                 "a merely audible held TwinSlide side fails runtime")
    same = copy.deepcopy(good)
    same["cables"][1] = copy.deepcopy(same["cables"][0])
    same["cables"][1]["source"] = "TwinSlide out 5"
    bad += check(any("do not differ" in problem
                     for problem in P.module_runtime_contract_errors(
                         patch, inv, prefix + json.dumps(same))),
                 "two active copies fail the stereo runtime contract")
    bad += check(any("UNMEASURED" in problem
                     for problem in P.module_runtime_contract_errors(
                         patch, inv, "no structured report")),
                 "missing real DSP evidence cannot pass the module contract")
    duplicate = copy.deepcopy(patch)
    duplicate["modules"].append(copy.deepcopy(duplicate["modules"][0]))
    duplicate["modules"][1]["id"] = 2
    bad += check(any("does not identify duplicate instances" in problem
                     for problem in P.module_runtime_contract_errors(
                         duplicate, inv, prefix + json.dumps(good))),
                 "duplicate exact-module instances cannot share one DSP report")
    return bad


def test_generation_defaults_to_one_model_call() -> int:
    return check(
        inspect.signature(P._generate).parameters["retries"].default == 0
        and inspect.signature(P.generate).parameters["retries"].default == 0,
        "patch generation defaults to one call unless retries are explicit")


def test_stereo_runtime_requires_two_live_lanes() -> int:
    idiom = {"slug": "stereo-spread", "runtime_contract": {
        "kind": "live_output_lanes", "requirements": ["left", "right"],
        "min_mean_abs_v": 0.0001, "max_abs_correlation": 0.995}}
    patch = {"modules": [
        {"id": 1, "plugin": "Fixture", "model": "VCO"},
        {"id": 2, "plugin": "Fixture", "model": "VCA"},
        {"id": 3, "plugin": "Core", "model": "AudioInterface2"},
    ], "cables": [
        {"outputModuleId": 1, "outputId": 0,
         "inputModuleId": 3, "inputId": 0},
        {"outputModuleId": 2, "outputId": 0,
         "inputModuleId": 3, "inputId": 1},
    ]}
    report = "BEHAVIOUR_JSON " + json.dumps({"schema": 1, "cables": [
        {"source": "VCO out 0", "mean_abs_v": 0.2},
        {"source": "VCA out 0", "mean_abs_v": 0.0},
    ], "pairwise": [{"left": 0, "right": 1, "correlation": 0.2}]})
    bad = check(any("VCA out 0 requires mean_abs_v" in problem
                    for problem in P.idiom_runtime_contract_errors(
                        patch, idiom, report)),
                "stereo cannot pass with one live side and one silent side")
    live = report.replace('"mean_abs_v": 0.0', '"mean_abs_v": 0.1')
    bad += check(not P.idiom_runtime_contract_errors(patch, idiom, live),
                 "two independently measured live stereo lanes pass")
    mono = live.replace('"correlation": 0.2', '"correlation": 1.0')
    bad += check(any("effectively mono" in problem
                     for problem in P.idiom_runtime_contract_errors(
                         patch, idiom, mono)),
                 "two live but sample-identical lanes do not count as stereo")
    return bad


def main() -> int:
    bad = sum(test() for test in (
        test_port_complete_plan,
        test_real_acid_plan_is_narrowed_to_proven_construction,
        test_acid_witness_is_materialized_before_rack,
        test_acid_owns_slide_semantics_but_keeps_independent_sounds,
        test_missing_fit_fails_before_model,
        test_generation_stops_before_quota,
        test_malformed_refinement_base_stops_before_model,
        test_build_help_spends_zero_calls,
        test_raw_response_retention_is_exact_and_fail_closed,
        test_seq16_state_materialization,
        test_omitted_gate_controls_stay_audible,
        test_version_gate,
        test_cold_twinslide_state_contract,
        test_twinslide_authoring_contract_reaches_model,
        test_named_sparse_module_contract_is_small_and_exact,
        test_cartog_parameter_contract_fails_closed,
        test_twinslide_runtime_contract,
        test_generation_defaults_to_one_model_call,
        test_stereo_runtime_requires_two_live_lanes,
        test_broken_override_registry_fails_closed,
        test_rack_integer_identity_types_fail_closed,
    ))
    print(f"\n{ASSERTIONS - bad}/{ASSERTIONS} acid and state preflight "
          "assertions passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
