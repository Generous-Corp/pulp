#!/usr/bin/env python3
"""A cable can reach a real jack at a valid index and still be inaudible.

Run:  python3 tools/rack/test_signal_path.py

Hermetic by default: every case builds its own inventory, so nothing here
depends on which modules happen to be installed. The two cases that DO read
the installed inventory guard themselves and SKIP rather than pass when it
is absent -- a check with nothing to check against proves nothing, and
reporting it as a pass is worse than not running it.

Every check below is paired with an OPPOSITE-DIRECTION CONTROL: an input
that must produce the other answer on the same instrument. A rule that
fires on everything and a rule that fires on nothing both look like a clean
pass from one side, and only the control tells them apart.
"""
import copy
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patch as P                                        # noqa: E402
import signal_path as SP                                 # noqa: E402


def param(pid, name, lo, hi, default):
    return {"id": pid, "name": name, "minValue": lo, "maxValue": hi,
            "defaultValue": default, "min": lo, "max": hi, "default": default}


def inventory() -> dict:
    """Modules chosen so each isolates one decision the rules must make."""
    read_vca = {"name": "VCA", "tags": ["VCA"],
                "inputs": ["In", "Level CV"], "roles_in": ["Audio", "Cv"],
                "outputs": ["Out"], "roles_out": ["Audio"],
                "params": [param(0, "Level", 0, 1, 1)]}
    return {"ForgeModular": {"name": "ForgeModular", "version": "1.0",
                             # Structurally identical to `Toy/VCA`; the ONLY
                             # difference is that this vendor's process() has
                             # been read, so `cv-fed-abstain` does not cover it.
                             "modules": {"VCA": read_vca}},
            "Toy": {"name": "Toy", "version": "1.0", "modules": {
        # One audio in, one level, names sharing no word: only the
        # sole-candidate rule can reach it.
        "VCA": {"name": "VCA", "tags": ["VCA"],
                "inputs": ["In", "Level CV"], "roles_in": ["Audio", "Cv"],
                "outputs": ["Out"], "roles_out": ["Audio"],
                "params": [param(0, "Level", 0, 1, 1)]},
        # The same, but its own default is zero: there is no default to
        # restore and the fraction of range has to be used.
        "VCAZERO": {"name": "VCAZERO", "tags": ["VCA"],
                    "inputs": ["In"], "roles_in": ["Audio"],
                    "outputs": ["Out"], "roles_out": ["Audio"],
                    "params": [param(0, "Level", 0, 2, 0)]},
        # Numbered channels the stem rule must resolve separately, plus a
        # master that is in series with both.
        "MIXER": {"name": "MIXER", "tags": ["Mixer"],
                  "inputs": ["Channel 1", "Channel 2"],
                  "roles_in": ["Audio", "Audio"],
                  "outputs": ["Mix"], "roles_out": ["Audio"],
                  "params": [param(0, "Channel 1 level", 0, 1, 0.8),
                             param(1, "Channel 2 level", 0, 1, 0.8),
                             param(2, "Master volume", 0, 1, 1)]},
        # Two levels, neither master: nothing is stage-wide here.
        "TWOLEVEL": {"name": "TWOLEVEL", "tags": ["Mixer"],
                     "inputs": ["Left", "Right"],
                     "roles_in": ["Audio", "Audio"],
                     "outputs": ["Out"], "roles_out": ["Audio"],
                     "params": [param(0, "Left level", 0, 1, 0.5),
                                param(1, "Right level", 0, 1, 0.5)]},
        # Gain-tagged, but its only level-ish control is BIPOLAR: zero is
        # its centre, not its off, and reading it as a level would report a
        # healthy module as closed.
        "BIPOLAR": {"name": "BIPOLAR", "tags": ["VCA"],
                    "inputs": ["In"], "roles_in": ["Audio"],
                    "outputs": ["Out"], "roles_out": ["Audio"],
                    "params": [param(0, "VCA response level", -1, 1, 0)]},
        # Abbreviated jack against a spelled-out level: only alias folding
        # closes the gap.
        "DUCK": {"name": "DUCK", "tags": ["Mixer"],
                 "inputs": ["Chan. 1 L / Poly", "Chan. 2 L / Poly"],
                 "roles_in": ["Audio", "Audio"],
                 "outputs": ["Out L"], "roles_out": ["Audio"],
                 "params": [param(0, "Channel 1 Volume", 0, 2, 1),
                            param(1, "Channel 2 Volume", 0, 2, 1)]},
        # One modulation jack, one signal jack: Rule B's repairable shape.
        "VCF": {"name": "VCF", "tags": ["Filter"],
                "inputs": ["Frequency", "Resonance", "Audio"],
                "roles_in": ["Cv", "Cv", "Audio"],
                "outputs": ["Low-pass"], "roles_out": ["Audio"],
                "params": [param(0, "Frequency", 0, 1, 0.5)]},
        # TWO audio inputs: which one the carrier belongs on is a guess.
        "DELAY": {"name": "DELAY", "tags": ["Delay"],
                  "inputs": ["Time", "Mix", "Audio"],
                  "roles_in": ["Cv", "Audio", "Audio"],
                  "outputs": ["Out"], "roles_out": ["Audio"],
                  "params": []},
        # No audio input at all: an FM target, never a Rule B candidate.
        "VCO": {"name": "VCO", "tags": ["Oscillator"],
                "inputs": ["1V/oct", "FM"], "roles_in": ["Pitch", "Cv"],
                "outputs": ["Sine"], "roles_out": ["Audio"],
                "params": [param(0, "Frequency", -4, 4, 0)]},
        # A modulation source: its output is Cv, so it is never a carrier.
        "LFO": {"name": "LFO", "tags": ["LFO"], "inputs": [],
                "roles_in": [], "outputs": ["Sine"], "roles_out": ["Cv"],
                "params": []},
        "AUDIO": {"name": "AUDIO", "tags": ["Audio"],
                  "inputs": ["L", "R"], "roles_in": ["Audio", "Audio"],
                  "outputs": [], "roles_out": [], "params": []},
    }}}


def inferred_inventory() -> dict:
    """The same modules, with their input roles flagged as GUESSED."""
    inv = inventory()
    for mod in inv["Toy"]["modules"].values():
        mod["roles_in_inferred"] = True
    return inv


def mod(mid, model, params=None):
    m = {"id": mid, "plugin": "Toy", "model": model, "pos": [mid, 0]}
    if params is not None:
        m["params"] = list(params)
    return m


_CABLE_ID = [0]


def cable(out_id, out_port, in_id, in_port):
    _CABLE_ID[0] += 1
    return {"id": _CABLE_ID[0], "outputModuleId": out_id, "outputId": out_port,
            "inputModuleId": in_id, "inputId": in_port}


def patch_of(modules, cables) -> dict:
    return {"version": "2.6.6", "modules": modules, "cables": cables}


def values(patch, mid) -> dict:
    for m in patch["modules"]:
        if m["id"] == mid:
            return {p["id"]: p["value"] for p in m.get("params") or []
                    if "value" in p}
    return {}


def voice(vca_params, extra_cables=()):
    """VCO -> VCA -> audio out, with the VCA's level as given."""
    return patch_of(
        [mod(1, "VCO"), mod(2, "VCA", vca_params), mod(3, "AUDIO")],
        [cable(1, 0, 2, 0), cable(2, 0, 3, 0)] + list(extra_cables))


# ── Rule A: a gain stage must be opened ──────────────────────────────────────

def test_a_closed_gain_stage_in_the_path_is_opened(inv):
    shut = voice([{"id": 0, "value": 0}])
    notes = SP.open_gain_stages(shut, inv)
    # CONTROL, opposite direction: the same stage already open must be left
    # exactly as authored. Without this, a writer that opened every level it
    # could see would pass the check above.
    open_ = voice([{"id": 0, "value": 0.4}])
    open_notes = SP.open_gain_stages(open_, inv)
    return [
        ("a level of 0 in the carrier's path is opened",
         values(shut, 2).get(0) == 1.0 and len(notes) == 1,
         f"{values(shut, 2)} {notes}"),
        ("CONTROL: an already-open level is left as authored",
         values(open_, 2).get(0) == 0.4 and open_notes == [],
         f"{values(open_, 2)} {open_notes}"),
    ]


def test_opening_restores_the_modules_own_default(inv):
    shut = voice([{"id": 0, "value": 0}])
    SP.open_gain_stages(shut, inv)
    # CONTROL: a module whose OWN default is zero has no default to restore,
    # so it must fall through to the fraction of range instead. Same writer,
    # different answer.
    zero = patch_of(
        [mod(1, "VCO"), mod(2, "VCAZERO", [{"id": 0, "value": 0}]),
         mod(3, "AUDIO")],
        [cable(1, 0, 2, 0), cable(2, 0, 3, 0)])
    SP.open_gain_stages(zero, inv)
    return [
        ("a level with a working default is restored to it",
         values(shut, 2).get(0) == 1.0, str(values(shut, 2))),
        ("CONTROL: a level whose default is 0 opens to a fraction of range",
         values(zero, 2).get(0) == 1.0 and values(zero, 2).get(0) != 0,
         str(values(zero, 2))),
    ]


def test_a_cv_fed_stage_is_left_alone(inv):
    """`cv-fed-abstain`: unread is unevaluated, not assumed."""
    fed = voice([{"id": 0, "value": 0}], [cable(1, 0, 2, 1)])
    SP.open_gain_stages(fed, inv)
    notes = SP.findings(fed, inv)
    # CONTROL: the identical patch WITHOUT the CV cable must be written.
    # Without it, a writer that never fired would pass the abstention check.
    unfed = voice([{"id": 0, "value": 0}])
    SP.open_gain_stages(unfed, inv)
    return [
        ("a level is not written while a CV input is cabled",
         values(fed, 2).get(0) == 0, str(values(fed, 2))),
        ("the abstention is reported as unevaluated, not as silence",
         any(f["verdict"] == SP.UNEVALUATED
             and f["check"] == "closed-gain-stage" for f in notes),
         str(notes)),
        ("CONTROL: the same patch with no CV cable is written",
         values(unfed, 2).get(0) == 1.0, str(values(unfed, 2))),
    ]


def test_a_read_vendors_cv_fed_stage_is_still_judged(inv):
    """`cv-fed-abstain` covers UNREAD vendors, not every CV-fed stage.

    A level that multiplies its CV is dead at zero however hard the CV is
    driven. Abstaining there leaves a genuinely silent patch silent, which is
    the defect this module exists to remove -- so the abstention is scoped to
    the vendors nobody has read, and this proves the scope has an inside.
    """
    read = patch_of(
        [mod(1, "VCO"),
         {"id": 2, "plugin": "ForgeModular", "model": "VCA", "pos": [2, 0],
          "params": [{"id": 0, "value": 0}]},
         mod(3, "AUDIO")],
        [cable(1, 0, 2, 0), cable(2, 0, 3, 0), cable(1, 0, 2, 1)])
    SP.open_gain_stages(read, inv)
    notes = SP.findings(read, inv)
    # CONTROL: the same topology under the UNREAD vendor must still abstain.
    # Without it, deleting the abstention entirely would pass this test.
    unread = voice([{"id": 0, "value": 0}], [cable(1, 0, 2, 1)])
    SP.open_gain_stages(unread, inv)
    return [
        ("a read vendor's CV-fed level IS opened",
         values(read, 2).get(0) == 1.0, str(values(read, 2))),
        ("and it is not filed as unevaluated",
         not any(f["verdict"] == SP.UNEVALUATED
                 and f["check"] == "closed-gain-stage" for f in notes),
         str(notes)),
        ("CONTROL: the unread vendor in the same shape still abstains",
         values(unread, 2).get(0) == 0, str(values(unread, 2))),
        ("CONTROL: the vendor lists differ, so the split can bite",
         "ForgeModular" in SP.MULTIPLYING_LEVEL_PLUGINS
         and "Toy" not in SP.MULTIPLYING_LEVEL_PLUGINS,
         str(sorted(SP.MULTIPLYING_LEVEL_PLUGINS))),
    ]


def test_a_bipolar_control_is_not_a_level(inv):
    bip = patch_of(
        [mod(1, "VCO"), mod(2, "BIPOLAR", [{"id": 0, "value": 0}]),
         mod(3, "AUDIO")],
        [cable(1, 0, 2, 0), cable(2, 0, 3, 0)])
    SP.open_gain_stages(bip, inv)
    return [
        ("a -1..1 control named 'level' is left at its centre",
         values(bip, 2).get(0) == 0, str(values(bip, 2))),
        ("no finding is raised about it",
         SP.findings(bip, inv) == [], str(SP.findings(bip, inv))),
        # CONTROL: the structural filter must still admit a real level, or
        # it would be excluding everything and this would read as a pass.
        ("CONTROL: a 0..1 control named 'level' IS a level",
         SP.is_level_param(param(0, "Level", 0, 1, 1))
         and not SP.is_level_param(param(0, "VCA response level", -1, 1, 0)),
         "structural filter"),
    ]


def test_a_stage_nothing_passes_through_is_not_touched(inv):
    idle = patch_of(
        [mod(1, "VCO"), mod(2, "VCA", [{"id": 0, "value": 0}]),
         mod(3, "AUDIO")],
        [cable(1, 0, 3, 0)])                       # the VCA is not in the path
    SP.open_gain_stages(idle, inv)
    dangling = patch_of(                           # fed, but feeding nothing
        [mod(1, "VCO"), mod(2, "VCA", [{"id": 0, "value": 0}]),
         mod(3, "AUDIO")],
        [cable(1, 0, 2, 0), cable(1, 0, 3, 0)])
    SP.open_gain_stages(dangling, inv)
    live = voice([{"id": 0, "value": 0}])          # CONTROL
    SP.open_gain_stages(live, inv)
    return [
        ("a stage with no carrier cabled in is not touched",
         values(idle, 2).get(0) == 0, str(values(idle, 2))),
        ("a stage whose output feeds nothing is not touched",
         values(dangling, 2).get(0) == 0, str(values(dangling, 2))),
        ("CONTROL: the same module in a live path IS touched",
         values(live, 2).get(0) == 1.0, str(values(live, 2))),
    ]


def test_a_partly_open_mixer_is_reported_not_written(inv):
    """One open channel means the stage passes something; opening a second
    is a mix decision rather than a repair."""
    mixed = patch_of(
        [mod(1, "VCO"), mod(2, "VCO"),
         mod(3, "MIXER", [{"id": 0, "value": 0}, {"id": 1, "value": 0.6}]),
         mod(4, "AUDIO")],
        [cable(1, 0, 3, 0), cable(2, 0, 3, 1), cable(3, 0, 4, 0)])
    SP.open_gain_stages(mixed, inv)
    notes = SP.findings(mixed, inv)
    # CONTROL: both channels shut IS a repair, so the writer must fire.
    both = patch_of(
        [mod(1, "VCO"), mod(2, "VCO"),
         mod(3, "MIXER", [{"id": 0, "value": 0}, {"id": 1, "value": 0}]),
         mod(4, "AUDIO")],
        [cable(1, 0, 3, 0), cable(2, 0, 3, 1), cable(3, 0, 4, 0)])
    SP.open_gain_stages(both, inv)
    return [
        ("a partly open mixer is left as authored",
         values(mixed, 3).get(0) == 0, str(values(mixed, 3))),
        ("it is reported as an advisory rather than silently ignored",
         any(f["check"] == "partly-closed-gain-stage"
             and f["verdict"] == SP.ADVISORY for f in notes), str(notes)),
        ("CONTROL: a fully shut mixer is written",
         values(both, 3).get(0) == 0.8 and values(both, 3).get(1) == 0.8,
         str(values(both, 3))),
    ]


def test_a_master_level_is_in_series_with_every_channel(inv):
    shut = patch_of(
        [mod(1, "VCO"), mod(3, "MIXER", [{"id": 2, "value": 0}]),
         mod(4, "AUDIO")],
        [cable(1, 0, 3, 0), cable(3, 0, 4, 0)])
    SP.open_gain_stages(shut, inv)
    # CONTROL: a module with two levels and no master word has nothing
    # stage-wide, so the same reasoning must NOT reach it.
    two = SP.stage_wide_levels(inv, "Toy", "TWOLEVEL")
    return [
        ("a master volume at 0 is opened",
         values(shut, 3).get(2) == 1.0, str(values(shut, 3))),
        ("it is recognised as stage-wide",
         SP.stage_wide_levels(inv, "Toy", "MIXER") == {2},
         str(SP.stage_wide_levels(inv, "Toy", "MIXER"))),
        ("CONTROL: two peer levels give nothing stage-wide",
         two == set(), str(two)),
    ]


def test_the_stem_rule_resolves_each_channel_separately(inv):
    assoc = SP.level_associations(inv, "Toy", "MIXER")
    sole = SP.level_associations(inv, "Toy", "VCA")
    # CONTROL: a module with no level at all must associate nothing, or an
    # association function that returned a fixed map would pass above.
    none = SP.level_associations(inv, "Toy", "VCO")
    return [
        ("each numbered channel finds its own level",
         assoc == {0: 0, 1: 1}, str(assoc)),
        ("the sole-candidate rule crosses a shared-no-word gap",
         sole == {0: 0}, str(sole)),
        ("CONTROL: a module with no level associates nothing",
         none == {}, str(none)),
    ]


def test_alias_folding_reaches_an_abbreviated_jack(inv):
    assoc = SP.level_associations(inv, "Toy", "DUCK")
    return [
        ("'Chan. 1 L / Poly' finds 'Channel 1 Volume'",
         assoc == {0: 0, 1: 1}, str(assoc)),
        ("the fold happens in the words, not by position",
         SP.words("Chan. 1 L / Poly") <= SP.words("Channel 1 Volume"),
         f"{SP.words('Chan. 1 L / Poly')} vs {SP.words('Channel 1 Volume')}"),
        # CONTROL: folding must not make everything match everything.
        ("CONTROL: channel 1 does not match channel 2's level",
         not SP.words("Chan. 1 L / Poly") <= SP.words("Channel 2 Volume"),
         "alias folding is not a wildcard"),
    ]


# ── Rule B: a carrier must land on a signal jack ─────────────────────────────

def filtered(in_port, source="VCO", src_out=0):
    return patch_of(
        [mod(1, source), mod(2, "VCF"), mod(3, "AUDIO")],
        [cable(1, src_out, 2, in_port), cable(2, 0, 3, 0)])


def test_a_carrier_on_a_modulation_jack_is_moved(inv):
    wrong = filtered(0)                                  # into Frequency
    notes = SP.route_carriers_to_signal_inputs(wrong, inv)
    # CONTROL: a carrier already on the signal jack must not be moved, or a
    # writer that rewrote every cable would pass the check above.
    right = filtered(2)                                  # into Audio
    right_notes = SP.route_carriers_to_signal_inputs(right, inv)
    return [
        ("the cable is moved onto the audio jack",
         wrong["cables"][0]["inputId"] == 2 and len(notes) == 1,
         f"{wrong['cables'][0]} {notes}"),
        ("CONTROL: a carrier already on the audio jack is untouched",
         right["cables"][0]["inputId"] == 2 and right_notes == [],
         f"{right['cables'][0]} {right_notes}"),
    ]


def test_a_modulation_source_is_never_treated_as_a_carrier(inv):
    """An LFO into a filter's Frequency jack is the point of the jack."""
    lfo = filtered(0, source="LFO")
    notes = SP.route_carriers_to_signal_inputs(lfo, inv)
    # CONTROL: the identical topology with an AUDIO-role source must move.
    osc = filtered(0)
    SP.route_carriers_to_signal_inputs(osc, inv)
    return [
        ("a Cv-role output into a modulation jack is left alone",
         lfo["cables"][0]["inputId"] == 0 and notes == [],
         f"{lfo['cables'][0]} {notes}"),
        ("nothing is reported about it either",
         SP.findings(lfo, inv) == [], str(SP.findings(lfo, inv))),
        ("CONTROL: an Audio-role source in the same topology IS moved",
         osc["cables"][0]["inputId"] == 2, str(osc["cables"][0])),
    ]


def test_frequency_modulation_between_oscillators_is_protected(inv):
    fm = patch_of(
        [mod(1, "VCO"), mod(2, "VCO"), mod(3, "AUDIO")],
        [cable(1, 0, 2, 1), cable(2, 0, 3, 0)])          # audio into FM
    notes = SP.route_carriers_to_signal_inputs(fm, inv)
    # CONTROL: the same audio-into-modulation shape on a module that HAS an
    # audio input is a defect. The discriminator is the destination, and
    # without this control the rule could simply be inert.
    thru = filtered(0)
    SP.route_carriers_to_signal_inputs(thru, inv)
    return [
        ("audio into an oscillator's FM jack is left alone",
         fm["cables"][0]["inputId"] == 1 and notes == [],
         f"{fm['cables'][0]} {notes}"),
        ("CONTROL: the same shape into a filter IS repaired",
         thru["cables"][0]["inputId"] == 2, str(thru["cables"][0])),
    ]


def test_two_audio_inputs_are_an_abstention(inv):
    """`one-audio-input`: choosing between Mix and Audio is a guess."""
    amb = patch_of(
        [mod(1, "VCO"), mod(2, "DELAY"), mod(3, "AUDIO")],
        [cable(1, 0, 2, 0), cable(2, 0, 3, 0)])          # into Time
    notes = SP.route_carriers_to_signal_inputs(amb, inv)
    found = SP.findings(amb, inv)
    # CONTROL: one audio input in the same shape must be repaired.
    one = filtered(0)
    SP.route_carriers_to_signal_inputs(one, inv)
    return [
        ("no cable is moved when two audio jacks could be the target",
         amb["cables"][0]["inputId"] == 0 and notes == [],
         f"{amb['cables'][0]} {notes}"),
        ("the ambiguity is reported as unevaluated",
         any(f["check"] == "carrier-on-modulation-jack"
             and f["verdict"] == SP.UNEVALUATED for f in found), str(found)),
        ("CONTROL: one audio jack in the same shape IS repaired",
         one["cables"][0]["inputId"] == 2, str(one["cables"][0])),
    ]


def test_two_carriers_are_an_abstention(inv):
    """`one-carrier`: which cable belongs on the jack is a topology call."""
    two = patch_of(
        [mod(1, "VCO"), mod(2, "VCO"), mod(3, "VCF"), mod(4, "AUDIO")],
        [cable(1, 0, 3, 0), cable(2, 0, 3, 1), cable(3, 0, 4, 0)])
    notes = SP.route_carriers_to_signal_inputs(two, inv)
    found = SP.findings(two, inv)
    one = filtered(0)                                    # CONTROL
    SP.route_carriers_to_signal_inputs(one, inv)
    return [
        ("neither cable is moved",
         [c["inputId"] for c in two["cables"][:2]] == [0, 1] and notes == [],
         f"{two['cables'][:2]} {notes}"),
        ("the ambiguity is reported rather than dropped",
         any(f["check"] == "carrier-on-modulation-jack"
             and f["verdict"] == SP.UNEVALUATED for f in found), str(found)),
        ("CONTROL: a single carrier in the same shape IS moved",
         one["cables"][0]["inputId"] == 2, str(one["cables"][0])),
    ]


def test_a_fed_signal_jack_stops_the_rule(inv):
    both = patch_of(
        [mod(1, "VCO"), mod(2, "VCO"), mod(3, "VCF"), mod(4, "AUDIO")],
        [cable(1, 0, 3, 2), cable(2, 0, 3, 0), cable(3, 0, 4, 0)])
    notes = SP.route_carriers_to_signal_inputs(both, inv)
    one = filtered(0)                                    # CONTROL
    SP.route_carriers_to_signal_inputs(one, inv)
    return [
        ("a carrier on the modulation jack is fine once signal is fed",
         both["cables"][1]["inputId"] == 0 and notes == [],
         f"{both['cables'][1]} {notes}"),
        ("CONTROL: with the signal jack empty the same cable moves",
         one["cables"][0]["inputId"] == 2, str(one["cables"][0])),
    ]


def test_a_dangling_processor_is_not_repaired(inv):
    dangling = patch_of(
        [mod(1, "VCO"), mod(2, "VCF"), mod(3, "AUDIO")],
        [cable(1, 0, 2, 0)])                       # VCF output goes nowhere
    notes = SP.route_carriers_to_signal_inputs(dangling, inv)
    one = filtered(0)                                    # CONTROL
    SP.route_carriers_to_signal_inputs(one, inv)
    return [
        ("a processor feeding nothing is left to the audio-sink check",
         dangling["cables"][0]["inputId"] == 0 and notes == [],
         f"{dangling['cables'][0]} {notes}"),
        ("CONTROL: cabling its output onward makes it repairable",
         one["cables"][0]["inputId"] == 2, str(one["cables"][0])),
    ]


# ── A guessed role may never reject a patch ──────────────────────────────────

def test_an_inferred_role_can_never_block(inv):
    """Structural, not a policy check: there is no field on the inferred
    verdict that a later edit could set to make it blocking."""
    return [
        ("an inferred verdict is not blocking",
         SP._Verdict(SP.INFERRED).blocking is False, "inferred"),
        ("CONTROL: a cartographed verdict IS blocking",
         SP._Verdict(SP.CARTOGRAPHED).blocking is True, "cartographed"),
    ]


def test_the_linter_rejects_only_a_cartographed_dead_path(inv):
    shut = voice([{"id": 0, "value": 0}])
    hard = SP.dead_signal_path_errors(shut, inv)
    soft = SP.dead_signal_path_errors(copy.deepcopy(shut),
                                      inferred_inventory())
    advisory = SP.advisory_notes(copy.deepcopy(shut), inferred_inventory())
    # CONTROL: the opened patch must clear the blocking check, or an error
    # list that was always empty would pass the inferred check.
    open_ = voice([{"id": 0, "value": 0.5}])
    return [
        ("a cartographed closed stage is a blocking error",
         len(hard) == 1, str(hard)),
        ("the same patch on inferred roles is not",
         soft == [], str(soft)),
        ("it is still told to the reader",
         len(advisory) == 1, str(advisory)),
        ("CONTROL: an open stage is not an error either way",
         SP.dead_signal_path_errors(open_, inv) == [],
         str(SP.dead_signal_path_errors(open_, inv))),
    ]


def test_a_misrouted_carrier_is_a_blocking_error(inv):
    wrong = filtered(0)
    hard = SP.dead_signal_path_errors(wrong, inv)
    soft = SP.dead_signal_path_errors(filtered(0), inferred_inventory())
    return [
        ("a cartographed misroute is a blocking error",
         len(hard) == 1 and "modulation jack" in hard[0], str(hard)),
        ("an inferred misroute is not",
         soft == [], str(soft)),
        ("CONTROL: the repaired patch is not an error",
         SP.dead_signal_path_errors(filtered(2), inv) == [],
         str(SP.dead_signal_path_errors(filtered(2), inv))),
    ]


# ── The pipeline ─────────────────────────────────────────────────────────────

def test_prepare_and_lint_repairs_before_judging(inv):
    shut = voice([{"id": 0, "value": 0}])
    fixed, errs = P.prepare_and_lint(copy.deepcopy(shut), inv)
    wrong = filtered(0)
    routed, route_errs = P.prepare_and_lint(copy.deepcopy(wrong), inv)
    # CONTROL: with the writers skipped, lint must reject both. Without it,
    # a lint that never fired would pass the two checks above.
    raw = SP.dead_signal_path_errors(shut, inv)
    raw_route = SP.dead_signal_path_errors(wrong, inv)
    return [
        ("a closed stage is opened, then passes lint",
         values(fixed, 2).get(0) == 1.0
         and not any("multiplied to silence" in e for e in errs),
         f"{values(fixed, 2)} {errs}"),
        ("a misrouted carrier is moved, then passes lint",
         routed["cables"][0]["inputId"] == 2
         and not any("modulation jack" in e for e in route_errs),
         f"{routed['cables'][0]} {route_errs}"),
        ("CONTROL: without the writers, lint rejects both",
         len(raw) == 1 and len(raw_route) == 1, f"{raw} {raw_route}"),
    ]


def test_explain_tells_the_reader_about_a_guessed_role(inv):
    text = P.explain(voice([{"id": 0, "value": 0}]), inferred_inventory())
    clean = P.explain(voice([{"id": 0, "value": 0.5}]), inferred_inventory())
    return [
        ("the advisory reaches the reader",
         "SIGNAL THAT MAY NOT GET THROUGH" in text, text[:400]),
        ("CONTROL: a healthy patch says nothing",
         "SIGNAL THAT MAY NOT GET THROUGH" not in clean, clean[:400]),
    ]


def test_a_repaired_patch_is_stable_under_a_second_pass(inv):
    once = voice([{"id": 0, "value": 0}])
    SP.open_gain_stages(once, inv)
    first = copy.deepcopy(once)
    SP.open_gain_stages(once, inv)
    return [
        ("running the writer twice changes nothing the second time",
         once == first, f"{values(once, 2)} vs {values(first, 2)}"),
        ("CONTROL: the first pass did change something",
         values(first, 2).get(0) == 1.0, str(values(first, 2))),
    ]


# ── Against the installed library ────────────────────────────────────────────

def _installed():
    inv = P.inventory()
    total = sum(len((p or {}).get("modules") or {}) for p in inv.values())
    return inv, total


def test_the_rules_reach_real_modules():
    """VACUITY GUARD: with no installed inventory there is nothing to reach,
    and reporting a pass would be reporting that the rule works."""
    inv, total = _installed()
    if total < 50:
        return [("SKIP: no installed Rack inventory — this proves nothing",
                 True, f"{total} modules")]
    stages = [(pl, mo)
              for pl, plugin in inv.items()
              for mo in (plugin or {}).get("modules") or {}
              if SP.is_gain_stage(inv, pl, mo)
              and SP.level_associations(inv, pl, mo)]
    inert = [(pl, mo)
             for pl, plugin in inv.items()
             for mo in (plugin or {}).get("modules") or {}
             if not SP.is_gain_stage(inv, pl, mo)
             and SP.level_associations(inv, pl, mo)]
    return [
        ("the level association reaches real installed gain stages",
         len(stages) > 0, f"{len(stages)} of {total} modules"),
        # CONTROL: it must not reach EVERY module, or it is not selecting.
        ("CONTROL: it does not fire on most of the library",
         len(stages) + len(inert) < total / 2,
         f"{len(stages) + len(inert)} of {total}"),
    ]


def test_a_real_filter_publishes_both_jack_kinds():
    """The shape Rule B exists for, read off the installed inventory."""
    inv, total = _installed()
    if total < 50:
        return [("SKIP: no installed Rack inventory — this proves nothing",
                 True, f"{total} modules")]
    vcf = ((inv.get("Fundamental") or {}).get("modules") or {}).get("VCF")
    if not vcf or not vcf.get("inputs"):
        return [("SKIP: Fundamental/VCF not installed — this proves nothing",
                 True, "absent")]
    audio, cv = SP.audio_inputs(vcf), SP.cv_inputs(vcf)
    return [
        ("it publishes a signal jack a carrier belongs on",
         len(audio) == 1, str(audio)),
        ("it also publishes modulation jacks a carrier could land on",
         len(cv) >= 1, str(cv)),
    ]


def test_the_evidence_flag_actually_discriminates():
    """`roles_in_inferred` must separate READ from GUESSED on the installed
    library. If it returned one answer for everything, every severity
    decision resting on it would be decoration."""
    inv, total = _installed()
    if total < 50:
        return [("SKIP: no installed Rack inventory — this proves nothing",
                 True, f"{total} modules")]
    carto = [f"{pl}/{mo}"
             for pl, plugin in inv.items()
             for mo, m in ((plugin or {}).get("modules") or {}).items()
             if m.get("roles_in")
             and SP.role_evidence(m, "in") == SP.CARTOGRAPHED]
    inferred = [f"{pl}/{mo}"
                for pl, plugin in inv.items()
                for mo, m in ((plugin or {}).get("modules") or {}).items()
                if m.get("roles_in")
                and SP.role_evidence(m, "in") == SP.INFERRED]
    return [
        ("some installed modules have roles that were read",
         len(carto) > 0, f"{len(carto)} cartographed"),
        # CONTROL, opposite direction: and some do not. Both counts must be
        # non-zero or the flag is not a discriminator.
        ("CONTROL: and some have roles that were guessed",
         len(inferred) > 0, f"{len(inferred)} inferred"),
        # Named so the detection floor is visible rather than implied: the
        # vendor modules the rules most need are on the guessed side, so
        # their findings are advisory and only the WRITERS reach them.
        ("a Core module is on the read side",
         any(name.startswith("Core/") for name in carto), str(carto[:4])),
    ]


CASES = [
    test_a_closed_gain_stage_in_the_path_is_opened,
    test_opening_restores_the_modules_own_default,
    test_a_cv_fed_stage_is_left_alone,
    test_a_read_vendors_cv_fed_stage_is_still_judged,
    test_a_bipolar_control_is_not_a_level,
    test_a_stage_nothing_passes_through_is_not_touched,
    test_a_partly_open_mixer_is_reported_not_written,
    test_a_master_level_is_in_series_with_every_channel,
    test_the_stem_rule_resolves_each_channel_separately,
    test_alias_folding_reaches_an_abbreviated_jack,
    test_a_carrier_on_a_modulation_jack_is_moved,
    test_a_modulation_source_is_never_treated_as_a_carrier,
    test_frequency_modulation_between_oscillators_is_protected,
    test_two_audio_inputs_are_an_abstention,
    test_two_carriers_are_an_abstention,
    test_a_fed_signal_jack_stops_the_rule,
    test_a_dangling_processor_is_not_repaired,
    test_an_inferred_role_can_never_block,
    test_the_linter_rejects_only_a_cartographed_dead_path,
    test_a_misrouted_carrier_is_a_blocking_error,
    test_prepare_and_lint_repairs_before_judging,
    test_explain_tells_the_reader_about_a_guessed_role,
    test_a_repaired_patch_is_stable_under_a_second_pass,
    test_the_rules_reach_real_modules,
    test_a_real_filter_publishes_both_jack_kinds,
    test_the_evidence_flag_actually_discriminates,
]


def main() -> int:
    inv = inventory()
    bad = ran = skipped = 0
    for case in CASES:
        try:
            results = (case(inv) if case.__code__.co_argcount else case())
        except Exception as error:                      # noqa: BLE001
            print(f"  ERROR  {case.__name__}: {error!r}")
            bad += 1
            ran += 1
            continue
        for name, ok, detail in results:
            if name.startswith("SKIP:"):
                skipped += 1
                print(f"  SKIP   {name}\n         {detail}")
                continue
            ran += 1
            if not ok:
                bad += 1
                print(f"  WRONG  {name}\n         {detail}")
    tail = f" ({skipped} skipped)" if skipped else ""
    print(f"signal-path: {ran - bad}/{ran} correct{tail}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
